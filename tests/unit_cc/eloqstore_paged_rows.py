"""Read-only raw EloqStore row inspector for one Redis key.

The tool talks to the already-running DataStoreService HTTP/protobuf endpoint;
it does not consult EloqKV's cache.  It discovers the current physical table
name (including FLUSHDB suffixes), reads the metadata row by the logical key,
then scans exactly the reserved derived-key prefix for that object's pages.

Examples:
  python3 tests/unit_cc/eloqstore_paged_rows.py --dss-port 17406 \
      --key paged:object --expect-pages 2
  python3 tests/unit_cc/eloqstore_paged_rows.py --dss-port 17406 \
      --key-base64 AEVLVg== --include-values
"""

from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import http.client
import json
import sys


PAGE_MAGIC = b"\x00EKVPAGE"
HASH_PARTITIONS = 0x400


def encoded(value):
    return base64.b64encode(value).decode("ascii")


def decoded(value):
    return base64.b64decode(value, validate=True)


def rpc(host, port, method, request, timeout=30):
    body = json.dumps(request, separators=(",", ":")).encode("utf-8")
    connection = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        connection.request(
            "POST",
            "/EloqDS.remote.DataStoreRpcService/" + method,
            body=body,
            headers={"Content-Type": "application/json"},
        )
        response = connection.getresponse()
        payload = response.read()
    finally:
        connection.close()
    if response.status != 200:
        raise RuntimeError("%s HTTP %d: %s" %
                           (method, response.status,
                            payload.decode("utf-8", "replace")))
    result = json.loads(payload)
    common = result.get("result", {})
    error = int(common.get("error_code", 0))
    if error:
        raise RuntimeError("%s data-store error %d: %s" %
                           (method, error, common.get("error_msg", "")))
    return result


def scan(host, port, table, partition, shard, begin=b"", end=b"",
         batch_size=1000):
    rows = []
    start = begin
    inclusive = True
    while True:
        request = {
            "kv_table_name_str": table,
            "partition_id": partition,
            "start_key": encoded(start),
            "end_key": encoded(end),
            "inclusive_start": inclusive,
            "inclusive_end": False,
            "scan_forward": True,
            "batch_size": batch_size,
            "generate_session_id": False,
            "shard_id": shard,
        }
        response = rpc(host, port, "ScanNext", request)
        page = response.get("items", [])
        rows.extend(page)
        if len(page) < batch_size:
            return rows
        next_start = decoded(page[-1]["key"])
        if next_start == start and not inclusive:
            raise RuntimeError("ScanNext made no progress")
        start = next_start
        inclusive = False


def exact_row(host, port, table, partition, shard, key):
    """Return one live row, including attributes omitted by the Read RPC.

    EloqStore's current Read adapter does not copy ``expire_ts_`` into the
    response TTL field.  ScanNext does expose the stored attribute, so the
    durable oracle uses the half-open interval [key, key + NUL), which can
    contain only the exact binary key.
    """
    rows = scan(host, port, table, partition, shard, key, key + b"\x00",
                batch_size=2)
    exact = [row for row in rows if decoded(row["key"]) == key]
    if len(exact) > 1:
        raise RuntimeError("multiple live rows found for one exact key")
    return exact[0] if exact else None


def active_table(host, port, database, shard):
    logical = ("eloqkv_data_table_%d" % database).encode("ascii")
    rows = scan(host, port, "table_catalogs", 0, shard, batch_size=256)
    matches = []
    for row in rows:
        if decoded(row["key"]) == logical:
            matches.append(decoded(row["value"]).decode("utf-8"))
    if len(matches) != 1:
        raise RuntimeError("expected one catalog row for %r, found %r" %
                           (logical, matches))
    return matches[0]


def hash_tag(key):
    left = key.find(b"{")
    if left < 0:
        return key
    right = key.find(b"}", left + 1)
    if right < 0 or right == left + 1:
        return key
    return key[left + 1:right]


def partition_of(key):
    return binascii.crc_hqx(hash_tag(key), 0) % HASH_PARTITIONS


def page_prefix(key):
    return PAGE_MAGIC + len(key).to_bytes(4, "big") + key


def decode_page_key(key):
    if len(key) < 17 or not key.startswith(PAGE_MAGIC):
        raise ValueError("missing page-key magic")
    key_len = int.from_bytes(key[8:12], "big")
    if len(key) != 17 + key_len:
        raise ValueError("page-key length mismatch")
    kind_offset = 12 + key_len
    kind = key[kind_offset]
    if kind not in (0, 1):
        raise ValueError("unknown page kind %d" % kind)
    return {
        "object_key_base64": encoded(key[12:kind_offset]),
        "kind": "hash" if kind == 0 else "large-value",
        "page_id": int.from_bytes(key[kind_offset + 1:kind_offset + 5],
                                  "big"),
    }


def row_value(row, include_values):
    value = decoded(row.get("value", ""))
    result = {
        "status": "normal",
        "commit_ts": int(row.get("ts", 0)),
        "ttl": int(row.get("ttl", 0)),
        "byte_length": len(value),
        "sha256": hashlib.sha256(value).hexdigest(),
    }
    if include_values:
        result["value_base64"] = encoded(value)
    return result


def inspect(args, key):
    table = args.table or active_table(
        args.host, args.dss_port, args.database, args.shard)
    partition = partition_of(key)

    metadata_response = exact_row(args.host, args.dss_port, table, partition,
                                  args.shard, key)
    if metadata_response is None:
        raise RuntimeError("metadata row not found")
    metadata_value = decoded(metadata_response.get("value", ""))
    metadata = {
        "status": "normal",
        "commit_ts": int(metadata_response.get("ts", 0)),
        "ttl": int(metadata_response.get("ttl", 0)),
        "byte_length": len(metadata_value),
        "sha256": hashlib.sha256(metadata_value).hexdigest(),
    }
    object_tag = metadata_value[0] if metadata_value else None
    metadata["object_tag"] = object_tag
    metadata["representation"] = {
        12: "paged-hash",
        13: "ttl-paged-hash",
    }.get(object_tag, "non-paged-or-unknown")
    if args.include_values:
        metadata["value_base64"] = encoded(metadata_value)

    prefix = page_prefix(key)
    raw_pages = scan(args.host, args.dss_port, table, partition, args.shard,
                     prefix, prefix + b"\xff", args.batch_size)
    pages = []
    for row in raw_pages:
        encoded_key = decoded(row["key"])
        envelope = decode_page_key(encoded_key)
        if decoded(envelope["object_key_base64"]) != key:
            raise RuntimeError("prefix scan returned another object's row")
        item = dict(envelope)
        item.update(row_value(row, args.include_values))
        pages.append(item)
    pages.sort(key=lambda row: (row["kind"], row["page_id"]))

    if args.expect_pages is not None and len(pages) < args.expect_pages:
        raise RuntimeError("expected at least %d page rows, found %d" %
                           (args.expect_pages, len(pages)))
    if args.page_size is not None:
        wrong = [(row["page_id"], row["byte_length"]) for row in pages
                 if row["kind"] == "hash" and
                 row["byte_length"] != args.page_size]
        if wrong:
            raise RuntimeError("wrong-sized hash pages: %r" % (wrong,))

    max_page_ts = max((row["commit_ts"] for row in pages), default=0)
    metadata_dominates_pages = metadata["commit_ts"] >= max_page_ts
    page_ttls_clear = all(row["ttl"] == 0 for row in pages)
    if args.assert_coherent:
        if metadata["representation"] not in ("paged-hash",
                                               "ttl-paged-hash"):
            raise RuntimeError("metadata is not a paged hash: tag=%r" %
                               object_tag)
        if not metadata_dominates_pages:
            raise RuntimeError("page timestamp %d outruns metadata %d" %
                               (max_page_ts, metadata["commit_ts"]))
        if not page_ttls_clear:
            raise RuntimeError("one or more page rows carries store TTL")
    return {
        "table": table,
        "partition_id": partition,
        "shard_id": args.shard,
        "object_key_base64": encoded(key),
        "metadata": metadata,
        "page_count": len(pages),
        "max_page_commit_ts": max_page_ts,
        "metadata_dominates_pages": metadata_dominates_pages,
        "page_ttls_clear": page_ttls_clear,
        "pages": pages,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--dss-port", type=int, required=True)
    parser.add_argument("--database", type=int, default=0)
    parser.add_argument("--shard", type=int, default=0)
    parser.add_argument("--table", help="physical table; normally discovered")
    keys = parser.add_mutually_exclusive_group(required=True)
    keys.add_argument("--key")
    keys.add_argument("--key-base64")
    parser.add_argument("--batch-size", type=int, default=1000)
    parser.add_argument("--expect-pages", type=int)
    parser.add_argument("--page-size", type=int)
    parser.add_argument("--assert-coherent", action="store_true")
    parser.add_argument("--include-values", action="store_true")
    args = parser.parse_args()
    if args.batch_size < 1:
        parser.error("--batch-size must be positive")
    try:
        key = (args.key.encode("utf-8") if args.key is not None else
               decoded(args.key_base64))
        report = inspect(args, key)
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as exc:
        print("eloqstore row inspection failed: %s" % exc, file=sys.stderr)
        return 1
    json.dump(report, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
