"""Raw EloqStore page corruption, bounded failure, repair, and refetch."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time

from eloqstore_paged_rows import active_table, inspect
from paged_hash_matrix import pairs_to_map
from paged_persistence_matrix import restart
from paged_testlib import RespClient, RespError, require_equal


KEY = b"paged:durable-corruption"
MUTATIONS = ("delete", "truncate", "wrong-size", "wrong-page")


def model(fields):
    return {
        b"field:%05d" % index:
            b"value:%05d:" % index + bytes([index & 0xff]) * 180
        for index in range(fields)
    }


def populate(client, values):
    client.command(b"DEL", KEY)
    command = [b"HSET", KEY]
    for field, value in values.items():
        command.extend((field, value))
    require_equal(client.command(*command), len(values), "population")


def assert_model(client, values, label):
    reply = client.command(b"HGETALL", KEY)
    if not isinstance(reply, list) or pairs_to_map(reply) != values:
        raise AssertionError("%s model mismatch" % label)


def run_tool(args, *extra):
    command = [sys.executable, args.tool,
               "--host", args.host,
               "--dss-port", str(args.dss_port),
               "--key", KEY.decode(),
               "--confirm-corruption"] + list(extra)
    result = subprocess.run(command, text=True, capture_output=True,
                            timeout=60)
    if result.returncode:
        raise AssertionError("corruption tool failed: %s%s" %
                             (result.stdout, result.stderr))
    print(result.stdout.strip(), flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--dss-port", type=int, required=True)
    parser.add_argument("--start-script", required=True)
    parser.add_argument("--run-dir", required=True)
    parser.add_argument("--log-dir", required=True)
    parser.add_argument("--page-size", type=int, default=4096)
    parser.add_argument("--fields", type=int, default=180)
    parser.add_argument("--checkpoint-wait", type=float, default=12.0)
    parser.add_argument(
        "--tool",
        default=os.path.join(os.path.dirname(__file__),
                             "eloqstore_paged_corrupt.py"))
    args = parser.parse_args()
    values = model(args.fields)

    try:
        table = active_table(args.host, args.dss_port, 0, 0)
        with RespClient(host=args.host, port=args.port, timeout=60) as client:
            populate(client, values)
        time.sleep(args.checkpoint_wait)

        options = argparse.Namespace(
            host=args.host, dss_port=args.dss_port, database=0, shard=0,
            table=table, include_values=False, batch_size=1000,
            expect_pages=2, page_size=args.page_size, assert_coherent=True)
        report = inspect(options, KEY)
        target_page = report["pages"][0]["page_id"]

        for mutation in MUTATIONS:
            backup = os.path.join(args.run_dir,
                                  "durable-%s-page-%d.json" %
                                  (mutation, target_page))
            if os.path.exists(backup):
                os.unlink(backup)
            run_tool(args, "--page-id", str(target_page),
                     "--mutation", mutation, "--backup", backup)
            restart(args.start_script, args.run_dir, args.port,
                    "durable-corrupt-" + mutation)
            started = time.monotonic()
            with RespClient(host=args.host, port=args.port,
                            timeout=30) as client:
                reply = client.command_deadline(30, b"HKEYS", KEY)
                require_equal(client.command(b"PING"), b"PONG",
                              mutation + " server health")
            if not isinstance(reply, RespError):
                raise AssertionError("%s corruption returned success: %r" %
                                     (mutation, reply))
            print("%s: bounded error in %.3fs: %s" %
                  (mutation, time.monotonic() - started, reply.message),
                  flush=True)

            run_tool(args, "--restore", backup)
            restart(args.start_script, args.run_dir, args.port,
                    "durable-repair-" + mutation)
            with RespClient(host=args.host, port=args.port,
                            timeout=60) as client:
                assert_model(client, values, mutation + " repair")
            repaired = inspect(options, KEY)
            if not repaired["metadata_dominates_pages"]:
                raise AssertionError("%s repair left timestamp inversion" %
                                     mutation)
            os.unlink(backup)
    except (AssertionError, OSError, RuntimeError, TimeoutError,
            subprocess.TimeoutExpired) as exc:
        print("RESULT: FAIL: %s" % exc, flush=True)
        return 1

    print("RESULT: PASS (durable missing/truncated/wrong-size/wrong-page)",
          flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
