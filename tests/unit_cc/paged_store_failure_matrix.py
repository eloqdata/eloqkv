"""Coalesced page-fetch failure with readers, writer, and disconnect.

Covers required failure-matrix combination 3 from the paged-object test plan:
two readers plus one writer share one missing-page fetch, the store completion
fails, and no waiter is lost or allowed to mutate. A second round disconnects
one reader after the fetch is in flight; the remaining reader/writer must still
receive the error and the object must cleanly refetch after the injector is
removed.
"""

from __future__ import annotations

import argparse
import time

from paged_concurrency_matrix import concurrent_round
from paged_testlib import (RespClient, RespError, arm_fault, disarm_fault,
                           marker_count, require_equal, shed_all_pages,
                           wait_until)


def require_store_errors(replies, label: str) -> None:
    for index, reply in enumerate(replies):
        if not isinstance(reply, RespError):
            raise AssertionError("%s actor %d got %r, want store error" %
                                 (label, index, reply))


def run_failed_round(control: RespClient, port: int, log_dir: str,
                     actions, label: str):
    stall_marker = "FAULTLOG stall_page_fetch"
    fail_marker = "FAULTLOG fail_page_fetch"
    stall_before = marker_count(log_dir, stall_marker)
    fail_before = marker_count(log_dir, fail_marker)
    arm_fault(control, "stall_page_fetch")
    arm_fault(control, "fail_page_fetch")
    started = time.monotonic()
    try:
        replies = concurrent_round(port, actions)
    finally:
        disarm_fault(control, "fail_page_fetch")
        disarm_fault(control, "stall_page_fetch")
    wait_until(lambda: marker_count(log_dir, stall_marker) > stall_before,
               5, 0.05, label + " stall marker")
    wait_until(lambda: marker_count(log_dir, fail_marker) > fail_before,
               5, 0.05, label + " failure marker")
    if time.monotonic() - started < 2.0:
        raise AssertionError(label + " did not traverse the bounded fetch stall")
    require_store_errors(replies, label)
    return replies


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=7399)
    parser.add_argument("--log-dir", required=True)
    parser.add_argument("--checkpoint-wait", type=float, default=12.0)
    parser.add_argument("--fields", type=int, default=160)
    args = parser.parse_args()
    if args.fields < 24:
        parser.error("--fields must be at least 24")

    key = b"paged:store-failure"
    field = b"f00003"
    original = b"v00003:" + bytes([3]) * 180
    try:
        with RespClient(port=args.port) as control:
            control.command(b"DEL", key)
            commands = []
            for i in range(args.fields):
                value = b"v%05d:" % i + bytes([i & 0xff]) * 180
                commands.append((b"HSET", key, b"f%05d" % i, value))
            if any(reply != 1 for reply in control.pipeline(commands)):
                raise AssertionError("population did not insert every field")
            scan = control.command(b"HSCAN", key, b"0", b"COUNT", b"1",
                                   b"NOVALUES")
            if not isinstance(scan, list) or scan[0] == b"0":
                raise AssertionError("precondition failed: object is not paged")
            time.sleep(args.checkpoint_wait)

            # All three commands name the same field, so their first pass
            # joins exactly one page fetch rather than relying on hash layout.
            shed_all_pages(control, args.log_dir, timeout=30)
            actions = [
                (b"HGET", key, field),
                (b"HGET", key, field),
                (b"HSET", key, field, b"must-not-land"),
            ]
            run_failed_round(control, args.port, args.log_dir, actions,
                             "2R+1W coalesced failure")
            require_equal(control.command(b"HGET", key, field), original,
                          "state unchanged after coalesced failure")
            require_equal(control.command(b"HLEN", key), args.fields,
                          "cardinality after coalesced failure")
            print("phase 2R+1W store error: every waiter errored, state intact",
                  flush=True)

            # Identify a page that is still absent rather than assuming a
            # second global clean pass will promptly revisit this now-hot
            # object. A failed fetch installs nothing, so the field whose
            # marker fires is a proven target for the disconnect race.
            probe_marker = "FAULTLOG fail_page_fetch"
            probe_before = marker_count(args.log_dir, probe_marker)
            disconnected_field = None
            disconnected_original = None
            arm_fault(control, "fail_page_fetch")
            try:
                for i in range(args.fields):
                    candidate = b"f%05d" % i
                    reply = control.command(b"HGET", key, candidate)
                    if marker_count(args.log_dir, probe_marker) > probe_before:
                        if not isinstance(reply, RespError):
                            raise AssertionError(
                                "nonresident probe returned %r, want error" %
                                reply)
                        disconnected_field = candidate
                        disconnected_original = (b"v%05d:" % i +
                                                 bytes([i & 0xff]) * 180)
                        break
            finally:
                disarm_fault(control, "fail_page_fetch")
            if disconnected_field is None or disconnected_original is None:
                raise AssertionError("could not prove a page remained absent")

            # Make one reader disappear while its proven-absent page fetch is
            # held. Remaining waiters must not be stranded or woken twice when
            # the same completion reports failure.
            stall_marker = "FAULTLOG stall_page_fetch"
            fail_marker = "FAULTLOG fail_page_fetch"
            stall_before = marker_count(args.log_dir, stall_marker)
            fail_before = marker_count(args.log_dir, fail_marker)
            arm_fault(control, "stall_page_fetch")
            arm_fault(control, "fail_page_fetch")
            try:
                disconnected = RespClient(port=args.port, timeout=10)
                disconnected.send(b"HGET", key, disconnected_field)
                disconnected.close()
                wait_until(
                    lambda: marker_count(args.log_dir, stall_marker) >
                    stall_before,
                    5, 0.05, "disconnected reader fetch marker")
                actions = [
                    (b"HGET", key, disconnected_field),
                    (b"HSET", key, disconnected_field,
                     b"also-must-not-land"),
                ]
                replies = concurrent_round(args.port, actions)
            finally:
                disarm_fault(control, "fail_page_fetch")
                disarm_fault(control, "stall_page_fetch")
            wait_until(lambda: marker_count(args.log_dir, fail_marker) >
                       fail_before, 5, 0.05,
                       "disconnected-reader failure marker")
            require_store_errors(replies, "disconnect + reader/writer")
            require_equal(control.command(b"HGET", key, disconnected_field),
                          disconnected_original,
                          "state unchanged after disconnect/error")
            require_equal(control.command(b"HSET", key, b"post", b"ok"), 1,
                          "post-error write")
            require_equal(control.command(b"HGET", key, b"post"), b"ok",
                          "post-error readback")
            require_equal(control.command(b"PING"), b"PONG", "final health")
            print("phase disconnect + store error: survivors completed cleanly",
                  flush=True)
    except (AssertionError, OSError, TimeoutError) as exc:
        print("RESULT: FAIL: %s" % exc, flush=True)
        return 1

    print("RESULT: PASS", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
