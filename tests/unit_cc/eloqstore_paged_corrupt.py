"""Test-only durable page-row corruption through DataStoreService.

This deliberately mutates a live EloqStore namespace. It refuses to run
without --confirm-corruption and emits a JSON backup that can be restored with
--restore. Tests should checkpoint first, corrupt one row, restart so cached
pages cannot hide the row, assert a bounded storage error, restore, and restart
again before reusing the deployment.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

from eloqstore_paged_rows import (active_table, decode_page_key, decoded,
                                 encoded, exact_row, page_prefix,
                                 partition_of, rpc, scan)


def page_rows(args, table, key):
    prefix = page_prefix(key)
    return scan(args.host, args.dss_port, table, partition_of(key), args.shard,
                prefix, prefix + b"\xff")


def page_id(encoded_key, object_key):
    envelope = decode_page_key(encoded_key)
    if decoded(envelope["object_key_base64"]) != object_key:
        raise ValueError("page row belongs to another object")
    if envelope["kind"] != "hash":
        raise ValueError("expected a hash page row")
    return envelope["page_id"]


def write(args, table, partition, key, value, timestamp, delete=False):
    write_items(args, table, partition, [{
        "key": encoded(key),
        "value": encoded(value),
        "op_type": 0 if delete else 1,
        "ts": timestamp,
        "ttl": 0,
    }])


def write_items(args, table, partition, items):
    rpc(args.host, args.dss_port, "BatchWriteRecords", {
        "kv_table_name": table,
        "partition_id": partition,
        "shard_id": args.shard,
        "skip_wal": True,
        "items": items,
    })


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--dss-port", type=int, required=True)
    parser.add_argument("--database", type=int, default=0)
    parser.add_argument("--shard", type=int, default=0)
    parser.add_argument("--table")
    parser.add_argument("--key", required=True)
    parser.add_argument("--page-id", type=int)
    parser.add_argument("--mutation",
                        choices=("delete", "truncate", "wrong-size",
                                 "wrong-page"))
    parser.add_argument("--backup")
    parser.add_argument("--restore")
    parser.add_argument("--confirm-corruption", action="store_true")
    args = parser.parse_args()
    if not args.confirm_corruption:
        parser.error("refusing durable mutation without --confirm-corruption")
    if bool(args.restore) == bool(args.mutation):
        parser.error("choose exactly one of --mutation or --restore")
    if args.mutation and args.page_id is None:
        parser.error("--mutation requires --page-id")

    try:
        key = args.key.encode("utf-8")
        table = args.table or active_table(
            args.host, args.dss_port, args.database, args.shard)
        partition = partition_of(key)

        if args.restore:
            with open(args.restore, encoding="utf-8") as stream:
                backup = json.load(stream)
            if decoded(backup["object_key_base64"]) != key:
                raise RuntimeError("backup belongs to another object")
            if backup["table"] != table:
                raise RuntimeError("backup belongs to physical table %s, not %s" %
                                   (backup["table"], table))
            rows = page_rows(args, table, key)
            newest = max([int(row.get("ts", 0)) for row in rows] +
                         [int(backup["ts"]),
                          int(backup.get("injected_ts", 0)),
                          int(backup.get("metadata_ts", 0))])
            repair_ts = newest + 1
            write_items(args, table, partition, [{
                "key": encoded(key),
                "value": backup["metadata_value_base64"],
                "op_type": 1,
                "ts": repair_ts,
                "ttl": int(backup["metadata_ttl"]),
            }, {
                "key": backup["key_base64"],
                "value": backup["value_base64"],
                "op_type": 1,
                "ts": repair_ts,
                "ttl": 0,
            }])
            print("restored page %d at timestamp %d" %
                  (backup["page_id"], repair_ts))
            return 0

        rows = page_rows(args, table, key)
        metadata = exact_row(args.host, args.dss_port, table, partition,
                             args.shard, key)
        if metadata is None:
            raise RuntimeError("metadata row not found")
        indexed = {page_id(decoded(row["key"]), key): row for row in rows}
        if args.page_id not in indexed:
            raise RuntimeError("page %d not found; live ids=%r" %
                               (args.page_id, sorted(indexed)))
        target = indexed[args.page_id]
        target_key = decoded(target["key"])
        target_value = decoded(target["value"])
        backup = {
            "table": table,
            "partition_id": partition,
            "object_key_base64": encoded(key),
            "page_id": args.page_id,
            "key_base64": encoded(target_key),
            "value_base64": encoded(target_value),
            "ts": int(target.get("ts", 0)),
            "ttl": int(target.get("ttl", 0)),
            "metadata_value_base64": metadata["value"],
            "metadata_ts": int(metadata.get("ts", 0)),
            "metadata_ttl": int(metadata.get("ttl", 0)),
        }
        newest = max([int(row.get("ts", 0)) for row in rows] +
                     [int(metadata.get("ts", 0))]) + 1
        backup["injected_ts"] = newest
        backup_path = args.backup or (
            "paged-page-%d-backup.json" % args.page_id)
        if os.path.exists(backup_path):
            raise RuntimeError("backup path already exists: %s" % backup_path)
        with open(backup_path, "x", encoding="utf-8") as stream:
            json.dump(backup, stream, indent=2, sort_keys=True)
            stream.write("\n")

        if args.mutation == "delete":
            write(args, table, partition, target_key, b"", newest,
                  delete=True)
        elif args.mutation == "truncate":
            write(args, table, partition, target_key,
                  target_value[:max(0, len(target_value) // 2)], newest)
        elif args.mutation == "wrong-size":
            write(args, table, partition, target_key,
                  target_value + b"\x00", newest)
        else:
            sources = [row for pid, row in indexed.items()
                       if pid != args.page_id]
            if not sources:
                raise RuntimeError("wrong-page requires at least two pages")
            write(args, table, partition, target_key,
                  decoded(sources[0]["value"]), newest)
        print("injected %s into page %d at timestamp %d; backup=%s" %
              (args.mutation, args.page_id, newest, backup_path))
        return 0
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as exc:
        print("durable corruption failed: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
