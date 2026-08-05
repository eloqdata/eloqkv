"""Concurrent readers/writers whose first access must page-fault.

The test creates and checkpoints one multi-page object, sheds all pages, then
starts two readers and one writer on three pre-existing fields behind a
barrier.  ``stall_page_fetch`` widens the race and its log marker proves that
the operations did not accidentally hit resident data.  A second round has
three readers request the same absent page to exercise fetch coalescing.

Usage: python3 tests/unit_cc/paged_concurrency_matrix.py \
           --port 7399 --log-dir bld-paged-matrix/single-off/logs
"""

from __future__ import annotations

import argparse
import threading
import time

from paged_testlib import (RespClient, RespError, arm_fault, disarm_fault,
                           marker_count, require_equal, shed_all_pages,
                           wait_until)


def concurrent_round(port: int, actions):
    barrier = threading.Barrier(len(actions) + 1)
    replies = [None] * len(actions)
    failures = []

    def actor(index, command):
        try:
            with RespClient(port=port, timeout=60) as client:
                barrier.wait(timeout=10)
                replies[index] = client.command(*command)
        except BaseException as exc:  # report every actor, do not strand peers
            failures.append((index, repr(exc)))

    threads = [threading.Thread(target=actor, args=(i, command), daemon=True)
               for i, command in enumerate(actions)]
    for thread in threads:
        thread.start()
    barrier.wait(timeout=10)
    for thread in threads:
        thread.join(timeout=65)
    if any(thread.is_alive() for thread in threads):
        raise AssertionError("a concurrent page-fault actor did not return")
    if failures:
        raise AssertionError("concurrent actors failed: %r" % failures)
    return replies


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=7399)
    parser.add_argument("--log-dir", required=True)
    parser.add_argument("--checkpoint-wait", type=float, default=12.0)
    parser.add_argument("--fields", type=int, default=480)
    args = parser.parse_args()
    if args.fields < 12:
        parser.error("--fields must be at least 12")

    reader_fields = (b"f%05d" % 3,
                     b"f%05d" % (args.fields // 2))
    writer_field = b"f%05d" % (args.fields - 9)

    key = b"paged:concurrency"
    values = {}
    with RespClient(port=args.port) as control:
        control.command(b"DEL", key)
        commands = []
        for i in range(args.fields):
            field = b"f%05d" % i
            value = b"v%05d:" % i + bytes([i & 0xff]) * 180
            values[field] = value
            commands.append((b"HSET", key, field, value))
        if any(reply != 1 for reply in control.pipeline(commands)):
            raise AssertionError("population did not insert every field")

        # COUNT must expose the paged representation before fault tests begin.
        scan = control.command(b"HSCAN", key, b"0", b"COUNT", b"5",
                               b"NOVALUES")
        if not isinstance(scan, list) or scan[0] == b"0":
            raise AssertionError("precondition failed: object is not paged")
        time.sleep(args.checkpoint_wait)

        # Different fields: both readers and the update must discover absent
        # pages. The fields are spaced widely to make distinct production-hash
        # pages overwhelmingly likely; correctness does not depend on that.
        shed_all_pages(control, args.log_dir, timeout=30)
        before = marker_count(args.log_dir, "FAULTLOG stall_page_fetch")
        arm_fault(control, "stall_page_fetch")
        started = time.monotonic()
        try:
            actions = [
                (b"HGET", key, reader_fields[0]),
                (b"HGET", key, reader_fields[1]),
                (b"HSET", key, writer_field, b"writer-won"),
            ]
            replies = concurrent_round(args.port, actions)
        finally:
            disarm_fault(control, "stall_page_fetch")
        wait_until(lambda: marker_count(args.log_dir,
                                        "FAULTLOG stall_page_fetch") > before,
                   5, label="stalled page-fetch marker")
        if time.monotonic() - started < 2.0:
            raise AssertionError("fault race was not widened by the 3s stall")
        require_equal(replies[0], values[reader_fields[0]], "reader 1")
        require_equal(replies[1], values[reader_fields[1]], "reader 2")
        require_equal(replies[2], 0, "writer existing-field reply")
        require_equal(control.command(b"HGET", key, writer_field), b"writer-won",
                      "writer readback")
        require_equal(control.command(b"HLEN", key), args.fields,
                      "cardinality after concurrent update")

        # Same page: first identify a page that is still nonresident. A failed
        # fetch never installs its bytes, so the exact field whose failure
        # marker fired remains a deterministic coalescing target. This avoids
        # assuming that a second clean pass will promptly revisit a now-hot
        # object in a deployment with a large LRU.
        failure_marker = "FAULTLOG fail_page_fetch"
        failure_before = marker_count(args.log_dir, failure_marker)
        coalesced_field = None
        arm_fault(control, "fail_page_fetch")
        try:
            for i in range(args.fields):
                candidate = b"f%05d" % i
                reply = control.command(b"HGET", key, candidate)
                if marker_count(args.log_dir, failure_marker) > failure_before:
                    if not isinstance(reply, RespError):
                        raise AssertionError(
                            "failed page fetch returned %r, want error" % reply)
                    coalesced_field = candidate
                    break
        finally:
            disarm_fault(control, "fail_page_fetch")
        if coalesced_field is None:
            raise AssertionError("could not prove a page remained nonresident")

        # Concurrent readers must all wake from one coalesced fetch and see
        # the same bytes. No waiter may be lost or answered twice.
        before = marker_count(args.log_dir, "FAULTLOG stall_page_fetch")
        arm_fault(control, "stall_page_fetch")
        try:
            same = (b"HGET", key, coalesced_field)
            replies = concurrent_round(args.port, [same, same, same])
        finally:
            disarm_fault(control, "stall_page_fetch")
        wait_until(lambda: marker_count(args.log_dir,
                                        "FAULTLOG stall_page_fetch") > before,
                   5, label="coalesced fetch marker")
        require_equal(replies, [values[coalesced_field]] * 3,
                      "coalesced readers")

        # Quiescent health check catches leaked locks/waiters that only become
        # visible to the following transaction.
        require_equal(control.command(b"HSET", key, b"post", b"ok"), 1,
                      "post-race write")
        require_equal(control.command(b"HGET", key, b"post"), b"ok",
                      "post-race read")
        require_equal(control.command(b"PING"), b"PONG", "post-race PING")

    print("RESULT: PASS", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
