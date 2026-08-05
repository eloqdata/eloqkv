"""Durable-row lifecycle oracle for a paged hash in live EloqStore."""

from __future__ import annotations

import argparse
import time
from types import SimpleNamespace

from eloqstore_paged_rows import (active_table, exact_row, inspect,
                                 page_prefix, partition_of, scan)
from paged_testlib import RespClient, require_equal


KEY = b"paged:durable-row-oracle"


def image(fields, generation, value_size):
    return {
        b"field:%05d" % index:
            (b"generation:%d:%05d:" % (generation, index) +
             bytes([(index + generation) & 0xff]) * value_size)
        for index in range(fields)
    }


def hset_image(client, key, values):
    command = [b"HSET", key]
    for field, value in values.items():
        command.extend((field, value))
    return client.command(*command)


def inspect_key(args, table, key):
    options = SimpleNamespace(
        host=args.host,
        dss_port=args.dss_port,
        database=0,
        shard=args.shard,
        table=table,
        include_values=False,
        batch_size=1000,
        expect_pages=2,
        page_size=args.page_size,
        assert_coherent=True,
    )
    return inspect(options, key)


def raw_page_rows(args, table, key):
    prefix = page_prefix(key)
    return scan(args.host, args.dss_port, table, partition_of(key),
                args.shard, prefix, prefix + b"\xff")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--dss-port", type=int, required=True)
    parser.add_argument("--shard", type=int, default=0)
    parser.add_argument("--page-size", type=int, default=4096)
    parser.add_argument("--fields", type=int, default=180)
    parser.add_argument("--checkpoint-wait", type=float, default=12.0)
    args = parser.parse_args()

    try:
        table = active_table(args.host, args.dss_port, 0, args.shard)
        hdel_key = KEY + b":hdel"
        del_key = KEY + b":del"
        failures = []
        baseline = image(args.fields, 0, 160)
        with RespClient(host=args.host, port=args.port, timeout=60) as client:
            client.command(b"DEL", hdel_key, del_key)
            require_equal(hset_image(client, hdel_key, baseline), args.fields,
                          "durable baseline")
            require_equal(client.command(b"PEXPIRE", hdel_key, 120000), 1,
                          "durable baseline TTL")
        time.sleep(args.checkpoint_wait)

        first = inspect_key(args, table, hdel_key)
        if first["metadata"]["representation"] != "ttl-paged-hash":
            raise AssertionError("durable metadata lost TTL twin: %r" %
                                 first["metadata"])
        if first["metadata"]["ttl"] <= int(time.time() * 1000) + 60000:
            raise AssertionError(
                "metadata store TTL lacks expected slack: %r" %
                first["metadata"]["ttl"])
        print("baseline: metadata + %d fixed-size TTL-free pages" %
              first["page_count"], flush=True)

        updated = image(args.fields, 1, 220)
        with RespClient(host=args.host, port=args.port, timeout=60) as client:
            require_equal(hset_image(client, hdel_key, updated), 0,
                          "durable all-page update")
        time.sleep(args.checkpoint_wait)
        second = inspect_key(args, table, hdel_key)
        if second["metadata"]["commit_ts"] <= first["metadata"]["commit_ts"]:
            raise AssertionError("metadata timestamp did not advance")
        wrong_ts = [(row["page_id"], row["commit_ts"])
                    for row in second["pages"]
                    if row["commit_ts"] != second["metadata"]["commit_ts"]]
        if wrong_ts:
            raise AssertionError("all-page atomic batch has stale page rows: "
                                 "%r" % wrong_ts)
        print(
            "update: metadata and every live page share commit timestamp %d" %
            second["metadata"]["commit_ts"], flush=True)

        with RespClient(host=args.host, port=args.port, timeout=60) as client:
            require_equal(client.command(b"HDEL", hdel_key, *updated.keys()),
                          args.fields, "durable delete-all")
        time.sleep(args.checkpoint_wait)
        hdel_pages = raw_page_rows(args, table, hdel_key)
        hdel_metadata = exact_row(args.host, args.dss_port, table,
                                  partition_of(hdel_key), args.shard,
                                  hdel_key)
        if hdel_pages or hdel_metadata is not None:
            failures.append("HDEL-to-empty left metadata=%s pages=%d" %
                            (hdel_metadata is not None, len(hdel_pages)))
        else:
            print("HDEL-to-empty: metadata and every page row absent",
                  flush=True)
            recreated = image(args.fields, 2, 180)
            with RespClient(host=args.host, port=args.port,
                            timeout=60) as client:
                require_equal(hset_image(client, hdel_key, recreated),
                              args.fields, "durable recreation")
            time.sleep(args.checkpoint_wait)
            third = inspect_key(args, table, hdel_key)
            if (third["metadata"]["commit_ts"] <=
                    second["metadata"]["commit_ts"]):
                raise AssertionError(
                    "recreated metadata did not supersede delete")
            if any(row["commit_ts"] != third["metadata"]["commit_ts"]
                   for row in third["pages"]):
                raise AssertionError(
                    "recreated metadata/pages are not one batch")
            print("recreate: coherent %d-page new incarnation" %
                  third["page_count"], flush=True)

        # Exercise Redis DEL independently.  A failure above must not prevent
        # this second deletion route from being classified.
        with RespClient(host=args.host, port=args.port, timeout=60) as client:
            require_equal(hset_image(client, del_key, baseline), args.fields,
                          "explicit DEL baseline")
        time.sleep(args.checkpoint_wait)
        del_before = raw_page_rows(args, table, del_key)
        if len(del_before) < 2:
            raise AssertionError("explicit DEL baseline did not page")
        with RespClient(host=args.host, port=args.port, timeout=60) as client:
            require_equal(client.command(b"DEL", del_key), 1,
                          "explicit DEL")
        time.sleep(args.checkpoint_wait)
        del_pages = raw_page_rows(args, table, del_key)
        del_metadata = exact_row(args.host, args.dss_port, table,
                                 partition_of(del_key), args.shard, del_key)
        if del_pages or del_metadata is not None:
            failures.append("DEL left metadata=%s pages=%d" %
                            (del_metadata is not None, len(del_pages)))
        else:
            print("DEL: metadata and every page row absent", flush=True)

        if failures:
            raise AssertionError("; ".join(failures))
    except (AssertionError, OSError, RuntimeError, TimeoutError) as exc:
        print("RESULT: FAIL: %s" % exc, flush=True)
        return 1

    print("RESULT: PASS (raw EloqStore durable rows)", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
