"""Qualification at the production-sized paging knobs.

The main integration matrix deliberately uses 4 KiB pages and a threshold of
one byte so small test objects exercise many page faults.  That acceleration
does not prove that the 128 KiB default page format, a low-single-digit-MiB
conversion point, and multi-MiB replies survive checkpoint/restart.  This
bounded S0 scenario supplies that missing release configuration check.

Run it against a single-node EloqStore deployment configured with a 4 MiB
conversion threshold and 128 KiB pages.  The test keeps the first incarnation
below the threshold, crosses it with the same logical key, checkpoints, sheds,
faults, restarts, deletes every field, restarts absent, and recreates the same
key above the threshold.  It also verifies the O(1) logical MEMORY USAGE value
and that the old incarnation's TTL cannot leak into the replacement.
"""

from __future__ import annotations

import argparse
import time

from paged_lifecycle_matrix import (assert_absent, restart_instance,
                                    wait_absent)
from paged_testlib import RespClient, require_equal, shed_all_pages


def field_name(index: int) -> bytes:
    return b"field:%06d" % index


def field_value(index: int, generation: bytes) -> bytes:
    prefix = generation + b":value:%06d:" % index
    return prefix + bytes([index & 0xff]) * (1024 - len(prefix))


def add_range(client: RespClient, model: dict[bytes, bytes], begin: int,
              end: int, generation: bytes) -> None:
    commands = []
    for index in range(begin, end):
        field = field_name(index)
        value = field_value(index, generation)
        model[field] = value
        commands.append((b"HSET", KEY, field, value))
    replies = client.pipeline(commands)
    if any(reply != 1 for reply in replies):
        raise AssertionError("population [%d,%d) did not insert every field: "
                             "%r" % (begin, end, replies[:8]))


def pairs_to_map(reply) -> dict[bytes, bytes]:
    if not isinstance(reply, list) or len(reply) % 2:
        raise AssertionError("malformed HGETALL reply: %r" % (reply,))
    return dict(zip(reply[0::2], reply[1::2]))


def is_paged(client: RespClient) -> bool:
    reply = client.command(b"HSCAN", KEY, b"0", b"COUNT", b"1",
                           b"NOVALUES")
    if (not isinstance(reply, list) or len(reply) != 2 or
            not isinstance(reply[0], bytes) or not isinstance(reply[1], list)):
        raise AssertionError("malformed HSCAN representation probe: %r" %
                             (reply,))
    return reply[0] != b"0"


def logical_bytes(model: dict[bytes, bytes]) -> int:
    return sum(len(field) + len(value) for field, value in model.items())


def assert_model(client: RespClient, model: dict[bytes, bytes],
                 label: str) -> None:
    require_equal(client.command(b"HLEN", KEY), len(model), label + " HLEN")
    require_equal(client.command(b"MEMORY", b"USAGE", KEY),
                  logical_bytes(model) + len(KEY), label + " MEMORY USAGE")
    keys = client.command(b"HKEYS", KEY)
    if not isinstance(keys, list):
        raise AssertionError(label + " HKEYS returned %r" % (keys,))
    require_equal(set(keys), set(model), label + " HKEYS")
    require_equal(pairs_to_map(client.command(b"HGETALL", KEY)), model,
                  label + " HGETALL")


KEY = b"paged:production-config:reused-key"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--log-dir", required=True)
    parser.add_argument("--start-script", required=True)
    parser.add_argument("--threshold", type=int, default=4 * 1024 * 1024)
    parser.add_argument("--below-fields", type=int, default=3000)
    parser.add_argument("--total-fields", type=int, default=4800)
    parser.add_argument("--checkpoint-wait", type=float, default=15.0)
    args = parser.parse_args()
    if args.below_fields <= 0 or args.total_fields <= args.below_fields:
        parser.error("field counts must satisfy 0 < below < total")

    client = RespClient(port=args.port, timeout=180)
    try:
        client.command(b"DEL", KEY)
        model: dict[bytes, bytes] = {}
        add_range(client, model, 0, args.below_fields, b"first")
        below_bytes = logical_bytes(model)
        if below_bytes >= args.threshold:
            raise AssertionError("below-threshold model is %d bytes, threshold "
                                 "is %d" % (below_bytes, args.threshold))
        if is_paged(client):
            raise AssertionError("hash converted before the configured "
                                 "threshold")
        print("phase below threshold: %d fields / %d logical bytes remained "
              "monolithic" % (len(model), below_bytes), flush=True)

        require_equal(client.command(b"PEXPIRE", KEY, 180000), 1,
                      "pre-conversion PEXPIRE")
        add_range(client, model, args.below_fields, args.total_fields,
                  b"first")
        above_bytes = logical_bytes(model)
        if above_bytes < args.threshold:
            raise AssertionError("above-threshold model is only %d bytes" %
                                 above_bytes)
        if not is_paged(client):
            raise AssertionError("hash did not convert after crossing the "
                                 "threshold")
        ttl = client.command(b"PTTL", KEY)
        if not isinstance(ttl, int) or ttl <= 0:
            raise AssertionError("conversion lost TTL: %r" % (ttl,))
        assert_model(client, model, "converted resident")
        print("phase conversion: %d fields / %d logical bytes paged; TTL "
              "preserved" % (len(model), above_bytes), flush=True)

        time.sleep(args.checkpoint_wait)
        shed = shed_all_pages(client, args.log_dir, timeout=60)
        probes = [0, args.below_fields - 1, args.below_fields,
                  args.total_fields - 1]
        for index in probes:
            require_equal(client.command(b"HGET", KEY, field_name(index)),
                          model[field_name(index)],
                          "post-shed HGET %d" % index)
        assert_model(client, model, "post-shed refault")
        print("phase checkpoint/shed/refault: shed=%d; full replies exact" %
              shed, flush=True)

        # Exercise dirty pages of different sizes before the durable reload.
        for index in range(0, args.total_fields, 401):
            field = field_name(index)
            value = b"expanded:" + bytes([index & 0xff]) * 8192
            require_equal(client.command(b"HSET", KEY, field, value), 0,
                          "expanded update %d" % index)
            model[field] = value
        deleted = list(model)[37::17]
        require_equal(client.command(b"HDEL", KEY, *deleted), len(deleted),
                      "multi-page delete")
        for field in deleted:
            del model[field]
        time.sleep(args.checkpoint_wait)
        client.close()
        restart_instance(args.start_script, args.port)
        client = RespClient(port=args.port, timeout=180)
        assert_model(client, model, "production-config restart")
        ttl = client.command(b"PTTL", KEY)
        if not isinstance(ttl, int) or ttl <= 0:
            raise AssertionError("restart lost the paged TTL: %r" % (ttl,))
        print("phase update/delete/restart: %d fields and TTL exact" %
              len(model), flush=True)

        require_equal(client.command(b"HDEL", KEY, *model.keys()), len(model),
                      "delete every production-sized field")
        assert_absent(client, KEY, "production-sized delete-all")
        time.sleep(args.checkpoint_wait)
        client.close()
        restart_instance(args.start_script, args.port)
        client = RespClient(port=args.port, timeout=180)
        wait_absent(args.port, KEY, "production-sized deleted key restart")
        assert_absent(client, KEY, "deleted key after restart")

        replacement: dict[bytes, bytes] = {}
        add_range(client, replacement, 0, args.total_fields, b"second")
        if not is_paged(client):
            raise AssertionError("same-key recreation did not convert")
        require_equal(client.command(b"PTTL", KEY), -1,
                      "recreation inherited old TTL")
        assert_model(client, replacement, "same-key recreation")
        require_equal(client.command(b"PING"), b"PONG", "final health")
        print("phase delete/restart/recreate: no resurrection, stale page "
              "reference, or TTL leak", flush=True)
    except (AssertionError, OSError, TimeoutError) as exc:
        print("RESULT: FAIL: %s" % exc, flush=True)
        return 1
    finally:
        client.close()

    print("RESULT: PASS", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
