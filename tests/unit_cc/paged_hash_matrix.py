"""Model-based protocol and lifecycle qualification for a paged hash.

Run against a Debug EloqKV/EloqStore node with conversion threshold 1.  With
``--faults`` the suite waits for a checkpoint, proves that pages were shed,
and repeats the complete read oracle from metadata-only state.

Usage:
  python3 tests/unit_cc/paged_hash_matrix.py --port 7399
  python3 tests/unit_cc/paged_hash_matrix.py --port 7399 --faults \
      --log-dir bld-standby/single_log --checkpoint-wait 12
"""

from __future__ import annotations

import argparse
import collections
import random
import time

from paged_testlib import (RespClient, RespError, hscan_all, require_equal,
                           require_error, scan_all, shed_all_pages)


def pairs_to_map(items):
    if len(items) % 2:
        raise AssertionError("odd HGETALL/HSCAN reply: %r" % (items,))
    return dict(zip(items[0::2], items[1::2]))


def assert_model(c: RespClient, key: bytes, model: dict[bytes, bytes],
                 label: str) -> None:
    require_equal(c.command(b"HLEN", key), len(model), label + " HLEN")
    keys = c.command(b"HKEYS", key)
    require_equal(set(keys), set(model), label + " HKEYS")
    require_equal(len(keys), len(model), label + " HKEYS cardinality")
    require_equal(collections.Counter(c.command(b"HVALS", key)),
                  collections.Counter(model.values()), label + " HVALS")
    require_equal(pairs_to_map(c.command(b"HGETALL", key)), model,
                  label + " HGETALL")
    probes = list(model)[:8] + [b"missing", b"missing"]
    require_equal(c.command(b"HMGET", key, *probes),
                  [model.get(field) for field in probes], label + " HMGET")
    scanned = hscan_all(c, key, count=5)
    require_equal(pairs_to_map(scanned), model, label + " HSCAN")


def absent_semantics(c: RespClient, key: bytes, label: str) -> None:
    require_equal(c.command(b"EXISTS", key), 0, label + " EXISTS")
    require_equal(c.command(b"TYPE", key), b"none", label + " TYPE")
    require_equal(c.command(b"HGET", key, b"f"), None, label + " HGET")
    require_equal(c.command(b"HMGET", key, b"f", b"f"), [None, None],
                  label + " HMGET")
    require_equal(c.command(b"HEXISTS", key, b"f"), 0, label + " HEXISTS")
    require_equal(c.command(b"HLEN", key), 0, label + " HLEN")
    require_equal(c.command(b"HSTRLEN", key, b"f"), 0, label + " HSTRLEN")
    require_equal(c.command(b"HDEL", key, b"f", b"f"), 0, label + " HDEL")
    require_equal(c.command(b"HKEYS", key), [], label + " HKEYS")
    require_equal(c.command(b"HVALS", key), [], label + " HVALS")
    require_equal(c.command(b"HGETALL", key), [], label + " HGETALL")
    require_equal(c.command(b"HRANDFIELD", key), None, label + " HRANDFIELD")
    require_equal(c.command(b"HRANDFIELD", key, 2), [],
                  label + " HRANDFIELD count")


def parser_and_wrong_type(c: RespClient, key: bytes) -> None:
    require_error(c.command(b"HSET", key), "HSET missing args")
    require_error(c.command(b"HSET", key, b"f"), "HSET odd args")
    require_error(c.command(b"HGET", key, b"f", b"extra"), "HGET extra arg")
    require_error(c.command(b"HINCRBY", key, b"f", b"1.0"),
                  "HINCRBY invalid integer")
    require_error(c.command(b"HSCAN", key, b"not-a-cursor"),
                  "HSCAN invalid cursor")
    require_error(c.command(b"HRANDFIELD", key, b"NaN"),
                  "HRANDFIELD invalid count")

    string_key = key + b":string"
    require_equal(c.command(b"SET", string_key, b"v"), b"OK", "SET")
    for command in ((b"HGET", string_key, b"f"),
                    (b"HSET", string_key, b"f", b"v"),
                    (b"HSCAN", string_key, b"0")):
        require_error(c.command(*command), command[0].decode() + " wrong type",
                      b"WRONGTYPE")


def populate(c: RespClient, key: bytes, nfields: int) -> dict[bytes, bytes]:
    model = {
        b"": b"",
        b"nul\x00field": b"nul\x00value\r\n",
        b"\xff\x80field": b"\x00\xff",
        b"duplicate-value-a": b"same",
        b"duplicate-value-b": b"same",
    }
    args = [b"HSET", key]
    for field, value in model.items():
        args.extend((field, value))
    require_equal(c.command(*args), len(model), "initial binary HSET")

    # Duplicate pairs are applied left to right but count one newly inserted
    # field only once. HMSET has the legacy status reply.
    require_equal(c.command(b"HSET", key, b"dup", b"one", b"dup", b"two"),
                  1, "HSET duplicate field")
    model[b"dup"] = b"two"
    require_equal(c.command(b"HMSET", key, b"hm", b"one", b"hm2", b"two"),
                  b"OK", "HMSET reply")
    model.update({b"hm": b"one", b"hm2": b"two"})

    commands = []
    for i in range(nfields):
        field = b"f%05d" % i
        value = (b"v%05d:" % i) + bytes([i & 0xff]) * (32 + i % 113)
        commands.append((b"HSET", key, field, value))
        model[field] = value
    replies = c.pipeline(commands)
    if any(reply != 1 for reply in replies):
        raise AssertionError("bulk population had non-insert replies")
    return model


def command_semantics(c: RespClient, key: bytes,
                      model: dict[bytes, bytes], test_ttl: bool = True) -> list[str]:
    failures = []
    require_equal(c.command(b"HGET", key, b""), b"", "empty value HGET")
    require_equal(c.command(b"HSTRLEN", key, b"nul\x00field"), 11,
                  "binary HSTRLEN")
    require_equal(c.command(b"HEXISTS", key, b""), 1, "empty field HEXISTS")
    require_equal(c.command(b"HEXISTS", key, b"absent"), 0,
                  "missing HEXISTS")

    existing_reply = c.command(b"HSETNX", key, b"dup", b"lost")
    existing_value = c.command(b"HGET", key, b"dup")
    if existing_reply != 0 or existing_value != model[b"dup"]:
        failures.append("HSETNX existing: reply=%r value=%r want=(0,%r)" %
                        (existing_reply, existing_value, model[b"dup"]))
    require_equal(c.command(b"HSETNX", key, b"nx", b"won"), 1,
                  "HSETNX new")
    model[b"nx"] = b"won"
    require_equal(c.command(b"HSET", key, b"nx", b"w"), 0,
                  "HSET shorter update")
    model[b"nx"] = b"w"
    require_equal(c.command(b"HSET", key, b"nx", b"much-longer-value"), 0,
                  "HSET longer update")
    model[b"nx"] = b"much-longer-value"

    require_equal(c.command(b"HINCRBY", key, b"counter", -4), -4,
                  "HINCRBY create")
    model[b"counter"] = b"-4"
    require_equal(c.command(b"HINCRBY", key, b"counter", 9), 5,
                  "HINCRBY update")
    model[b"counter"] = b"5"
    require_equal(c.command(b"HSET", key, b"bad-int", b"1.5"), 1,
                  "install bad integer")
    model[b"bad-int"] = b"1.5"
    require_error(c.command(b"HINCRBY", key, b"bad-int", 1),
                  "HINCRBY invalid stored value")
    require_equal(c.command(b"HGET", key, b"bad-int"), b"1.5",
                  "failed HINCRBY unchanged")
    require_equal(c.command(b"HSET", key, b"max-int", b"9223372036854775807"),
                  1, "install max integer")
    model[b"max-int"] = b"9223372036854775807"
    require_error(c.command(b"HINCRBY", key, b"max-int", 1),
                  "HINCRBY overflow")
    require_equal(c.command(b"HGET", key, b"max-int"), model[b"max-int"],
                  "overflow unchanged")

    first = c.command(b"HINCRBYFLOAT", key, b"float", b"1.5")
    if not isinstance(first, bytes) or abs(float(first) - 1.5) > 1e-12:
        raise AssertionError("HINCRBYFLOAT create: %r" % (first,))
    second = c.command(b"HINCRBYFLOAT", key, b"float", b"-0.25")
    if not isinstance(second, bytes) or abs(float(second) - 1.25) > 1e-12:
        raise AssertionError("HINCRBYFLOAT update: %r" % (second,))
    model[b"float"] = second

    # Random replies are nondeterministic; assert Redis's membership,
    # cardinality, uniqueness, repetition, and WITHVALUES association rules.
    one = c.command(b"HRANDFIELD", key)
    if one not in model:
        raise AssertionError("HRANDFIELD returned non-member %r" % (one,))
    distinct = c.command(b"HRANDFIELD", key, len(model) + 10)
    require_equal(len(distinct), len(model), "HRANDFIELD positive cap")
    require_equal(set(distinct), set(model), "HRANDFIELD distinct membership")
    repeated = c.command(b"HRANDFIELD", key, -len(model) * 2)
    require_equal(len(repeated), len(model) * 2, "HRANDFIELD negative count")
    if any(field not in model for field in repeated):
        raise AssertionError("HRANDFIELD negative returned a non-member")
    with_values = c.command(b"HRANDFIELD", key, 20, b"WITHVALUES")
    if len(with_values) != 40:
        raise AssertionError("HRANDFIELD WITHVALUES length %d" % len(with_values))
    for field, value in zip(with_values[0::2], with_values[1::2]):
        require_equal(value, model[field], "HRANDFIELD field/value association")

    if test_ttl:
        # TTL belongs to the object, not its current representation or page
        # set. Cluster lanes run the same transitions in the focused standby
        # test, after non-TTL replication has qualified, so a TTL apply crash
        # cannot mask every later scenario.
        require_equal(c.command(b"PEXPIRE", key, 60000), 1, "PEXPIRE")
        before = c.command(b"PTTL", key)
        if not 0 < before <= 60000:
            raise AssertionError("unexpected PTTL after expiry: %r" % before)
        require_equal(c.command(b"HSET", key, b"ttl-write", b"kept"), 1,
                      "write with TTL")
        model[b"ttl-write"] = b"kept"
        after = c.command(b"PTTL", key)
        if not 0 < after <= before:
            raise AssertionError("hash mutation lost/extended TTL: %r -> %r" %
                                 (before, after))
        require_equal(c.command(b"PERSIST", key), 1, "PERSIST")
        require_equal(c.command(b"PTTL", key), -1, "PTTL after PERSIST")
    return failures


def delete_recreate(c: RespClient, key: bytes,
                    model: dict[bytes, bytes], nfields: int) -> None:
    # One HDEL includes duplicates and misses; each existing field counts once.
    fields = list(model)
    reply = c.command(b"HDEL", key, b"missing", *fields, fields[0], b"missing")
    require_equal(reply, len(model), "one-command delete all")
    model.clear()
    absent_semantics(c, key, "after one-command delete-all")

    # Recreate below then above the threshold and delete in several commands.
    require_equal(c.command(b"HSET", key, b"small", b"v"), 1,
                  "small recreation")
    require_equal(c.command(b"HDEL", key, b"small"), 1,
                  "small recreation delete")
    absent_semantics(c, key, "after small recreation")
    model.update(populate(c, key, nfields // 2))
    for chunk in [list(model)[i:i + 37] for i in range(0, len(model), 37)]:
        require_equal(c.command(b"HDEL", key, *chunk), len(chunk),
                      "chunked HDEL")
        for field in chunk:
            model.pop(field)
    absent_semantics(c, key, "after chunked delete-all")

    # The transaction that removes the last fields must delete the key, not
    # leave an empty hash payload or resurrect an earlier incarnation.
    require_equal(c.command(b"HSET", key, b"a", b"1", b"b", b"2"), 2,
                  "transaction recreation")
    require_equal(c.command(b"MULTI"), b"OK", "MULTI")
    require_equal(c.command(b"HDEL", key, b"a"), b"QUEUED", "queued HDEL a")
    require_equal(c.command(b"HDEL", key, b"b"), b"QUEUED", "queued HDEL b")
    require_equal(c.command(b"EXEC"), [1, 1], "EXEC delete-all")
    absent_semantics(c, key, "after transactional delete-all")


def random_model(c: RespClient, key: bytes, seed: int, steps: int) -> list[str]:
    rng = random.Random(seed)
    model: dict[bytes, bytes] = {}
    failures = []
    for step in range(steps):
        field = b"r%03d" % rng.randrange(120)
        op = rng.randrange(10)
        if op < 6:
            value = rng.randbytes(rng.randrange(0, 96))
            want = 0 if field in model else 1
            require_equal(c.command(b"HSET", key, field, value), want,
                          "random HSET step %d" % step)
            model[field] = value
        elif op < 8:
            want = 1 if field in model else 0
            require_equal(c.command(b"HDEL", key, field, field), want,
                          "random duplicate HDEL step %d" % step)
            model.pop(field, None)
        else:
            value = rng.randbytes(rng.randrange(0, 48))
            want = 0 if field in model else 1
            got = c.command(b"HSETNX", key, field, value)
            if got != want:
                failures.append("random HSETNX step %d: got %r want %r" %
                                (step, got, want))
            if want:
                model[field] = value
        if step % 73 == 0:
            assert_model(c, key, model, "random cut %d" % step)
    assert_model(c, key, model, "random final")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=7399)
    parser.add_argument("--fields", type=int, default=320)
    parser.add_argument("--seed", type=int, default=0xE10C)
    parser.add_argument("--random-steps", type=int, default=600)
    parser.add_argument("--faults", action="store_true")
    parser.add_argument("--log-dir")
    parser.add_argument("--checkpoint-wait", type=float, default=12.0)
    parser.add_argument(
        "--skip-ttl", action="store_true",
        help="defer TTL transitions to paged_standby_ttl.py (cluster lanes)")
    args = parser.parse_args()
    if args.faults and not args.log_dir:
        parser.error("--faults requires --log-dir")

    key = b"paged:matrix:\x00\x80"
    failures = []
    with RespClient(port=args.port) as c:
        c.command(b"DEL", key, key + b":string", key + b":random")
        absent_semantics(c, key, "initial absent")
        parser_and_wrong_type(c, key)
        model = populate(c, key, args.fields)
        failures.extend(command_semantics(c, key, model,
                                          test_ttl=not args.skip_ttl))
        assert_model(c, key, model, "resident")

        first_scan = c.command(b"HSCAN", key, b"0", b"COUNT", b"5",
                               b"NOVALUES")
        if not isinstance(first_scan, list) or first_scan[0] == b"0":
            raise AssertionError("precondition failed: object is not paged")

        if args.faults:
            time.sleep(args.checkpoint_wait)
            shed = shed_all_pages(c, args.log_dir)
            print("proved page shedding: %d marker(s)" % shed, flush=True)
            assert_model(c, key, model, "metadata-only/refaulted")

        visible = scan_all(c)
        if key not in visible or any(k.startswith(b"\x00EKVPAGE") for k in visible):
            raise AssertionError("SCAN key visibility/page-row leak: %r" % visible)
        keys = c.command(b"KEYS", b"paged:matrix:*")
        if key not in keys or any(k.startswith(b"\x00EKVPAGE") for k in keys):
            raise AssertionError("KEYS key visibility/page-row leak: %r" % keys)

        delete_recreate(c, key, model, args.fields)
        failures.extend(random_model(c, key + b":random", args.seed,
                                     args.random_steps))
        require_equal(c.command(b"PING"), b"PONG", "final PING")

    if failures:
        for failure in failures[:20]:
            print("FAIL: " + failure, flush=True)
        if len(failures) > 20:
            print("FAIL: ... %d more" % (len(failures) - 20), flush=True)
        print("RESULT: FAIL (%d semantic mismatches)" % len(failures),
              flush=True)
        return 1
    print("RESULT: PASS", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
