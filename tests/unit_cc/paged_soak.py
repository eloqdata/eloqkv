"""Bounded model-based churn/pressure soak for paged hashes.

The same driver is usable as a short nightly regression or a multi-hour release
qualification.  It mixes a hot object with many competing objects, holds fixed
cardinality while creating dead bytes, repeatedly deletes/recreates keys, and
periodically forces a proved clean-page shedding cycle.  Exact logical state is
checked throughout and once more after the final refault.
"""

from __future__ import annotations

import argparse
import random
import statistics
import time

from paged_hash_matrix import assert_model
from paged_testlib import RespClient, require_equal, shed_all_pages


def value_for(rng, step):
    sizes = [0, 1, 31, 127, 128, 255, 511]
    size = sizes[rng.randrange(len(sizes))]
    return (b"s%08d:\x00" % step + rng.randbytes(size))[:size]


def populate(client, key, model, fields, rng, generation):
    commands = []
    for i in range(fields):
        field = b"f%05d" % i
        value = (b"g%03d:" % generation + rng.randbytes(160))
        commands.append((b"HSET", key, field, value))
        model[field] = value
    replies = client.pipeline(commands)
    if any(reply != 1 for reply in replies):
        raise AssertionError("population did not insert every field")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=7399)
    parser.add_argument("--log-dir", required=True)
    parser.add_argument("--seed", type=int, default=57612)
    parser.add_argument("--steps", type=int, default=10000)
    parser.add_argument("--duration", type=float, default=0,
                        help="optional wall-time floor in seconds")
    parser.add_argument("--keys", type=int, default=12)
    parser.add_argument("--fields", type=int, default=96)
    parser.add_argument("--verify-every", type=int, default=500)
    parser.add_argument("--shed-every", type=int, default=2500)
    parser.add_argument("--checkpoint-wait", type=float, default=8.0)
    args = parser.parse_args()
    if args.steps < 100 or args.keys < 2 or args.fields < 24:
        parser.error("need --steps >= 100, --keys >= 2, --fields >= 24")

    rng = random.Random(args.seed)
    keys = [b"paged:soak:%d:%03d" % (args.seed, i)
            for i in range(args.keys)]
    models = {key: {} for key in keys}
    generations = {key: 0 for key in keys}
    latencies = []
    shed_markers = 0
    last_shed_step = -1
    started = time.monotonic()
    step = 0

    try:
        with RespClient(port=args.port, timeout=90) as client:
            client.command(b"DEL", *keys)
            for key in keys:
                populate(client, key, models[key], args.fields, rng, 0)
            first = client.command(b"HSCAN", keys[0], b"0", b"COUNT", b"1",
                                   b"NOVALUES")
            if not isinstance(first, list) or first[0] == b"0":
                raise AssertionError("precondition failed: hot hash is not paged")
            time.sleep(args.checkpoint_wait)
            shed_markers += shed_all_pages(client, args.log_dir, timeout=30)
            for key in keys:
                assert_model(client, key, models[key], "initial refault")

            while step < args.steps or time.monotonic() - started < args.duration:
                # Half the operations hit key 0; the other half make many
                # paged objects compete for the shard budget.
                key = keys[0] if rng.randrange(2) == 0 else rng.choice(keys[1:])
                model = models[key]
                op = rng.randrange(100)
                before = time.monotonic()
                if op < 50:
                    # Fixed-cardinality churn: remove one and add another in a
                    # pipeline, stressing dead bytes, split reuse, and pending
                    # deletes without letting the logical object grow forever.
                    if model:
                        old = rng.choice(list(model))
                        reply = client.command(b"HDEL", key, old)
                        require_equal(reply, 1, "soak HDEL")
                        model.pop(old)
                    field = b"c%08d:%03d" % (step, rng.randrange(1000))
                    value = value_for(rng, step)
                    reply = client.command(b"HSET", key, field, value)
                    require_equal(reply, 1, "soak churn HSET")
                    model[field] = value
                elif op < 72:
                    field = rng.choice(list(model)) if model else b"missing"
                    require_equal(client.command(b"HGET", key, field),
                                  model.get(field), "soak HGET")
                elif op < 84:
                    fields = [rng.choice(list(model)) if model else b"missing"
                              for _ in range(6)] + [b"missing"]
                    require_equal(client.command(b"HMGET", key, *fields),
                                  [model.get(field) for field in fields],
                                  "soak HMGET")
                elif op < 94:
                    field = rng.choice(list(model)) if model else b"new"
                    value = value_for(rng, step)
                    require_equal(client.command(b"HSET", key, field, value),
                                  0 if field in model else 1,
                                  "soak update HSET")
                    model[field] = value
                else:
                    # Full deletion/recreation repeatedly exercises the empty
                    # object transition and incarnation/pending-delete rules.
                    require_equal(client.command(b"DEL", key),
                                  int(bool(model)), "soak DEL")
                    model.clear()
                    generations[key] += 1
                    populate(client, key, model, args.fields, rng,
                             generations[key])
                latencies.append(time.monotonic() - before)
                step += 1

                if step % args.verify_every == 0:
                    for verify_key in keys:
                        assert_model(client, verify_key, models[verify_key],
                                     "soak cut %d" % step)
                    print("verified step %d (elapsed %.1fs)" %
                          (step, time.monotonic() - started), flush=True)
                if args.shed_every and step % args.shed_every == 0:
                    # A short checkpoint wait makes the current dirty set
                    # eligible. Failure to observe the marker is vacuous, not
                    # a reason to silently skip the residency transition.
                    time.sleep(args.checkpoint_wait)
                    shed_markers += shed_all_pages(client, args.log_dir,
                                                   timeout=30)
                    last_shed_step = step

            # Do not demand two consecutive sheds. If the last operation cut
            # already shed every resident page, a second clean pass has
            # correctly got nothing to report. Otherwise make the final
            # residency transition here, then refault while checking the full
            # model below.
            if last_shed_step != step:
                time.sleep(args.checkpoint_wait)
                shed_markers += shed_all_pages(client, args.log_dir,
                                               timeout=30)
            for key in keys:
                assert_model(client, key, models[key], "soak final")
            require_equal(client.command(b"PING"), b"PONG", "soak health")
    except (AssertionError, OSError, TimeoutError) as exc:
        print("RESULT: FAIL at step %d seed=%d: %s" %
              (step, args.seed, exc), flush=True)
        return 1

    ordered = sorted(latencies)
    p99 = ordered[min(len(ordered) - 1, int(len(ordered) * 0.99))]
    print("latency seconds: median=%.4f p99=%.4f max=%.4f" %
          (statistics.median(latencies), p99, max(latencies)), flush=True)
    print("RESULT: PASS steps=%d elapsed=%.1fs shed_markers=%d seed=%d" %
          (step, time.monotonic() - started, shed_markers, args.seed),
          flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
