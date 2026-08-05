"""TTL-twin lifecycle for a paged hash across shedding and restart.

Exercises the key-level TTL requirements in the paged-object test plan:
relative and absolute deadlines, NX/XX/GT/LT conditions, PERSIST, mutation
without deadline loss, expiry from metadata-only residency, restart before and
after a deadline, and last-field deletion followed by same-key recreation.
"""

from __future__ import annotations

import argparse
import time

from paged_lifecycle_matrix import (assert_absent, assert_model, is_paged,
                                    populate, restart_instance, wait_absent,
                                    wait_model)
from paged_testlib import RespClient, require_equal, shed_all_pages, wait_until


def positive_pttl(client: RespClient, key: bytes, label: str,
                  upper: int | None = None) -> int:
    ttl = client.command(b"PTTL", key)
    if not isinstance(ttl, int) or ttl <= 0:
        raise AssertionError("%s: expected positive PTTL, got %r" % (label, ttl))
    if upper is not None and ttl > upper:
        raise AssertionError("%s: PTTL %d exceeds %d" % (label, ttl, upper))
    return ttl


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=7399)
    parser.add_argument("--log-dir", required=True)
    parser.add_argument("--start-script", required=True)
    parser.add_argument("--checkpoint-wait", type=float, default=15.0)
    parser.add_argument("--fields", type=int, default=96)
    args = parser.parse_args()
    if args.fields < 32:
        parser.error("--fields must be at least 32")

    key = b"paged:ttl:matrix"
    last = b"paged:ttl:last-field"
    control = RespClient(port=args.port)
    try:
        control.command(b"DEL", key, last)
        model = populate(control, key, args.fields, b"ttl")
        if not is_paged(control, key):
            raise AssertionError("TTL matrix precondition: hash is not paged")

        # Relative TTL and conditional option grammar/semantics.
        require_equal(control.command(b"PEXPIRE", key, 60000), 1,
                      "initial PEXPIRE")
        initial = positive_pttl(control, key, "initial PEXPIRE", 60000)
        require_equal(control.command(b"PEXPIRE", key, 70000, b"NX"), 0,
                      "PEXPIRE NX with existing TTL")
        require_equal(control.command(b"PEXPIRE", key, 70000, b"XX"), 1,
                      "PEXPIRE XX")
        after_xx = positive_pttl(control, key, "PEXPIRE XX", 70000)
        require_equal(control.command(b"PEXPIRE", key, after_xx - 5000, b"GT"),
                      0, "PEXPIRE GT rejects shorter deadline")
        require_equal(control.command(b"PEXPIRE", key, after_xx + 10000, b"GT"),
                      1, "PEXPIRE GT accepts longer deadline")
        after_gt = positive_pttl(control, key, "PEXPIRE GT")
        require_equal(control.command(b"PEXPIRE", key, after_gt + 10000, b"LT"),
                      0, "PEXPIRE LT rejects longer deadline")
        require_equal(control.command(b"PEXPIRE", key, after_gt - 5000, b"LT"),
                      1, "PEXPIRE LT accepts shorter deadline")

        # Absolute deadline reporting remains metadata-only and preserves the
        # paged TTL twin.
        deadline_ms = int(time.time() * 1000) + 90000
        require_equal(control.command(b"PEXPIREAT", key, deadline_ms), 1,
                      "PEXPIREAT")
        reported_ms = control.command(b"PEXPIRETIME", key)
        if not isinstance(reported_ms, int) or abs(reported_ms - deadline_ms) > 2:
            raise AssertionError("PEXPIRETIME got %r, want %d" %
                                 (reported_ms, deadline_ms))
        reported_s = control.command(b"EXPIRETIME", key)
        if not isinstance(reported_s, int) or abs(reported_s - deadline_ms // 1000) > 1:
            raise AssertionError("EXPIRETIME got %r for %d" %
                                 (reported_s, deadline_ms))
        if not is_paged(control, key):
            raise AssertionError("setting TTL changed the paged representation")
        require_equal(control.command(b"PERSIST", key), 1, "PERSIST")
        require_equal(control.command(b"PTTL", key), -1, "PTTL after PERSIST")
        require_equal(control.command(b"PERSIST", key), 0,
                      "second PERSIST")
        assert_model(control, key, model, "after conditional TTL/PERSIST")
        print("phase TTL options/PERSIST: replies and model exact", flush=True)

        # Expire while every page is absent. TTL is metadata, so expiration
        # must not need to reconstruct the hash or leave an empty key.
        time.sleep(args.checkpoint_wait)
        shed = shed_all_pages(control, args.log_dir, timeout=30)
        require_equal(control.command(b"PEXPIRE", key, 1200), 1,
                      "metadata-only PEXPIRE")
        wait_until(lambda: control.command(b"EXISTS", key) == 0,
                   15, 0.1, "metadata-only natural expiry")
        assert_absent(control, key, "metadata-only expiry")
        print("phase metadata-only expiry: shed=%d and key absent" % shed,
              flush=True)

        # Persist the expiry itself, then cold-restart. An old metadata row or
        # stale page-row set must not resurrect the key.
        time.sleep(args.checkpoint_wait)
        control.close()
        restart_instance(args.start_script, args.port)
        control = RespClient(port=args.port)
        wait_absent(args.port, key, "expired TTL key restart")
        assert_absent(control, key, "expired key after restart")
        print("phase expired checkpoint/restart: no resurrection", flush=True)

        # Restart before a long deadline: both content and remaining TTL must
        # survive. A following mutation must not reset or remove the deadline.
        model = populate(control, key, args.fields, b"restart")
        require_equal(control.command(b"PEXPIRE", key, 120000), 1,
                      "pre-restart PEXPIRE")
        before_restart = positive_pttl(control, key, "before restart", 120000)
        time.sleep(args.checkpoint_wait)
        shed_all_pages(control, args.log_dir, timeout=30)
        control.close()
        restart_instance(args.start_script, args.port)
        control = RespClient(port=args.port)
        wait_model(args.port, key, model, "TTL object restart")
        assert_model(control, key, model, "TTL object after restart")
        after_restart = positive_pttl(control, key, "after restart", before_restart)
        require_equal(control.command(b"HSET", key, b"after-restart", b"kept"),
                      1, "mutation under TTL")
        model[b"after-restart"] = b"kept"
        after_write = positive_pttl(control, key, "after mutation", after_restart)
        if after_write > after_restart:
            raise AssertionError("mutation extended TTL: %d -> %d" %
                                 (after_restart, after_write))
        assert_model(control, key, model, "after mutation under TTL")
        require_equal(control.command(b"PERSIST", key), 1,
                      "post-restart PERSIST")
        require_equal(control.command(b"PTTL", key), -1,
                      "post-restart persisted TTL")
        print("phase restart-before-deadline: content and TTL preserved",
              flush=True)

        # Deleting the last field retires the TTL twin. A new incarnation must
        # not inherit its deadline and disappear when the old timer elapses.
        require_equal(control.command(b"HSET", last, b"f", b"old"), 1,
                      "last-field setup")
        require_equal(control.command(b"PEXPIRE", last, 1200), 1,
                      "last-field PEXPIRE")
        require_equal(control.command(b"HDEL", last, b"f"), 1,
                      "last-field HDEL")
        assert_absent(control, last, "after last-field deletion")
        require_equal(control.command(b"HSET", last, b"new", b"incarnation"),
                      1, "last-field recreation")
        require_equal(control.command(b"PTTL", last), -1,
                      "recreation does not inherit TTL")
        time.sleep(1.5)
        require_equal(control.command(b"HGET", last, b"new"), b"incarnation",
                      "recreation survives old deadline")
        require_equal(control.command(b"PING"), b"PONG", "final health")
        print("phase last-field/recreate: old deadline did not leak", flush=True)
    except (AssertionError, OSError, TimeoutError) as exc:
        print("RESULT: FAIL: %s" % exc, flush=True)
        return 1
    finally:
        control.close()

    print("RESULT: PASS", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
