"""Binary-safe differential trace for paged versus reference hash behavior.

The reference endpoint may be Redis/Valkey or a conversion-disabled EloqKV
control.  The latter is what the checked-in runner can start without an
external package: it still gives an independent representation oracle
(monolithic versus paged), while the explicit Python model is the semantic
oracle shared by neither server.

Every run proves the target is paged, proves a metadata-only transition, then
continues the same deterministic trace.  On failure it writes a replayable
JSON trace containing the seed and every fully encoded command.
"""

from __future__ import annotations

import argparse
import base64
import collections
import json
import random
import time
from pathlib import Path

from paged_testlib import (RespClient, RespError, hscan_all, require_equal,
                           shed_all_pages)


def pairs_to_map(items):
    if not isinstance(items, list) or len(items) % 2:
        raise AssertionError("expected a field/value array, got %r" % items)
    return dict(zip(items[0::2], items[1::2]))


def encode_command(command):
    return [base64.b64encode(arg if isinstance(arg, bytes)
                             else str(arg).encode()).decode()
            for arg in command]


class Differential:
    def __init__(self, paged, reference, trace):
        self.paged = paged
        self.reference = reference
        self.trace = trace

    def exact(self, *command, label=None):
        self.trace.append(command)
        got = self.paged.command(*command)
        want = self.reference.command(*command)
        if got != want:
            raise AssertionError("%s differential: paged=%r reference=%r" %
                                 (label or command[0], got, want))
        return got

    def pipeline(self, commands, label):
        commands = list(commands)
        self.trace.extend(commands)
        got = self.paged.pipeline(commands)
        want = self.reference.pipeline(commands)
        if got != want:
            raise AssertionError("%s differential: paged=%r reference=%r" %
                                 (label, got, want))
        return got

    def model(self, key, model, label):
        for endpoint_name, client in (("paged", self.paged),
                                      ("reference", self.reference)):
            require_equal(client.command(b"HLEN", key), len(model),
                          "%s %s HLEN" % (label, endpoint_name))
            keys = client.command(b"HKEYS", key)
            require_equal(set(keys), set(model),
                          "%s %s HKEYS" % (label, endpoint_name))
            require_equal(len(keys), len(model),
                          "%s %s HKEYS count" % (label, endpoint_name))
            require_equal(collections.Counter(client.command(b"HVALS", key)),
                          collections.Counter(model.values()),
                          "%s %s HVALS" % (label, endpoint_name))
            require_equal(pairs_to_map(client.command(b"HGETALL", key)),
                          model, "%s %s HGETALL" % (label, endpoint_name))
            scan = hscan_all(client, key, count=3)
            require_equal(pairs_to_map(scan), model,
                          "%s %s HSCAN" % (label, endpoint_name))
            probes = list(model)[:12] + [b"missing", b"missing"]
            require_equal(client.command(b"HMGET", key, *probes),
                          [model.get(field) for field in probes],
                          "%s %s HMGET" % (label, endpoint_name))


def make_value(rng, step):
    lengths = [0, 1, 2, 127, 128, 255, 512]
    size = lengths[rng.randrange(len(lengths))]
    prefix = b"v%05d:\x00\xff" % step
    return (prefix + rng.randbytes(size))[:size]


def randomized_trace(diff, key, model, rng, steps):
    counters = [b"counter:%d" % i for i in range(8)]
    for step in range(steps):
        field = b"r%03d:\x00" % rng.randrange(160)
        op = rng.randrange(100)
        if op < 25:
            value = make_value(rng, step)
            want = 0 if field in model else 1
            require_equal(diff.exact(b"HSET", key, field, value), want,
                          "random HSET %d" % step)
            model[field] = value
        elif op < 35:
            want = 1 if field in model else 0
            require_equal(diff.exact(b"HDEL", key, field, field), want,
                          "random duplicate HDEL %d" % step)
            model.pop(field, None)
        elif op < 42:
            value = make_value(rng, step)
            want = 0 if field in model else 1
            require_equal(diff.exact(b"HSETNX", key, field, value), want,
                          "random HSETNX %d" % step)
            if want:
                model[field] = value
        elif op < 50:
            counter = counters[rng.randrange(len(counters))]
            delta = rng.randrange(-1000, 1001)
            old = int(model.get(counter, b"0"))
            got = diff.exact(b"HINCRBY", key, counter, delta)
            require_equal(got, old + delta, "random HINCRBY %d" % step)
            model[counter] = str(old + delta).encode()
        elif op < 68:
            require_equal(diff.exact(b"HGET", key, field), model.get(field),
                          "random HGET %d" % step)
        elif op < 76:
            fields = [field, b"missing", field,
                      b"r%03d:\x00" % rng.randrange(160)]
            require_equal(diff.exact(b"HMGET", key, *fields),
                          [model.get(item) for item in fields],
                          "random HMGET %d" % step)
        elif op < 82:
            require_equal(diff.exact(b"HEXISTS", key, field),
                          int(field in model), "random HEXISTS %d" % step)
            require_equal(diff.exact(b"HSTRLEN", key, field),
                          len(model.get(field, b"")),
                          "random HSTRLEN %d" % step)
        elif op < 88:
            diff.model(key, model, "random enumeration %d" % step)
        elif op < 94:
            commands = []
            for offset in range(4):
                f = b"pipe:%03d:%d" % (step, offset)
                v = make_value(rng, step + offset)
                commands.append((b"HSET", key, f, v))
                model[f] = v
            commands.extend(((b"HLEN", key), (b"HGET", key, commands[0][1])))
            diff.pipeline(commands, "pipeline %d" % step)
        else:
            a = b"txn:%03d:a" % step
            b = b"txn:%03d:b" % step
            diff.exact(b"MULTI")
            diff.exact(b"HSET", key, a, b"one")
            diff.exact(b"HSET", key, b, b"two")
            diff.exact(b"HDEL", key, a)
            require_equal(diff.exact(b"EXEC"), [1, 1, 1],
                          "transaction %d" % step)
            model.pop(a, None)
            model[b] = b"two"

        if step % 53 == 0:
            diff.model(key, model, "checkpoint %d" % step)
    diff.model(key, model, "random final")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--paged-port", type=int, default=7399)
    parser.add_argument("--reference-port", type=int, required=True)
    parser.add_argument("--log-dir", required=True)
    parser.add_argument("--seed", type=int, default=57612)
    parser.add_argument("--steps", type=int, default=500)
    parser.add_argument("--checkpoint-wait", type=float, default=8.0)
    parser.add_argument("--trace-out")
    args = parser.parse_args()
    if args.steps < 100:
        parser.error("--steps must be at least 100")

    rng = random.Random(args.seed)
    key = b"paged:differential:%d:\x00\xff" % args.seed
    trace = []
    try:
        with (RespClient(port=args.paged_port) as paged,
              RespClient(port=args.reference_port) as reference):
            diff = Differential(paged, reference, trace)
            diff.exact(b"DEL", key)

            model = {b"": b"", b"nul\x00field": b"nul\x00value\r\n"}
            command = [b"HSET", key]
            for field, value in model.items():
                command.extend((field, value))
            require_equal(diff.exact(*command), len(model), "binary setup")

            population = []
            for i in range(240):
                field = b"f%05d" % i
                value = b"v%05d:" % i + bytes([i & 0xff]) * (80 + i % 97)
                population.append((b"HSET", key, field, value))
                model[field] = value
            require_equal(diff.pipeline(population, "population"),
                          [1] * len(population), "population replies")
            diff.model(key, model, "resident")

            paged_scan = paged.command(b"HSCAN", key, b"0", b"COUNT", b"1",
                                       b"NOVALUES")
            if not isinstance(paged_scan, list) or paged_scan[0] == b"0":
                raise AssertionError("target precondition failed: not paged")
            # NOVALUES is needed only as EloqKV's representation probe and is
            # newer than the Redis 7.0 compatibility target. Keep the
            # reference-side precondition on portable HSCAN grammar; the
            # logical differential below compares ordinary HSCAN replies.
            reference_scan = reference.command(b"HSCAN", key, b"0", b"COUNT",
                                               b"1")
            if not isinstance(reference_scan, list):
                raise AssertionError("reference HSCAN reply malformed")

            time.sleep(args.checkpoint_wait)
            shed = shed_all_pages(paged, args.log_dir, timeout=30)
            diff.model(key, model, "metadata-only target")
            print("proved paged target and metadata-only transition: %d marker(s)"
                  % shed, flush=True)

            # Exact parser and wrong-type behavior is representation-neutral.
            wrong = key + b":wrong"
            diff.exact(b"SET", wrong, b"string")
            for command in ((b"HGET", wrong, b"f"),
                            (b"HSET", key),
                            (b"HSCAN", key, b"invalid"),
                            (b"HINCRBY", key, b"f00000", b"1.5")):
                reply = diff.exact(*command)
                if not isinstance(reply, RespError):
                    raise AssertionError("parser/wrong-type command returned %r" %
                                         (reply,))

            randomized_trace(diff, key, model, rng, args.steps)
            require_equal(diff.exact(b"PING"), b"PONG", "final health")
    except (AssertionError, OSError, TimeoutError) as exc:
        trace_path = Path(args.trace_out or
                          ("bld-paged-matrix/differential-%d.json" % args.seed))
        trace_path.parent.mkdir(parents=True, exist_ok=True)
        trace_path.write_text(json.dumps({
            "seed": args.seed,
            "paged_port": args.paged_port,
            "reference_port": args.reference_port,
            "error": str(exc),
            "commands_base64": [encode_command(command) for command in trace],
        }, indent=2) + "\n")
        print("RESULT: FAIL: %s" % exc, flush=True)
        print("trace: %s" % trace_path, flush=True)
        return 1

    print("RESULT: PASS (%d deterministic operations, seed=%d)" %
          (args.steps, args.seed), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
