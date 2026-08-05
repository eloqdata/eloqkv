"""Deterministic page-admission refusal followed by successful retry.

The §8 integration contract is stronger than an allocator unit test: a request
whose missing-page buffers cannot currently be admitted must mutate nothing,
yield the shard to cleaning, and re-run successfully when memory becomes
available.  This test drives that production path through the Debug-only
``force_page_admission_refusal`` hook, first for one reader and then for two
readers plus one writer whose accesses all begin with nonresident pages.

The refusal is global only for the brief armed window. Same-key page-faulting
commands must remain pending, while PING and an unrelated-key write remain
live. Disarming represents successful reclamation; every pending command must
then complete exactly once with the expected state and no leaked lock/waiter.
"""

from __future__ import annotations

import argparse
import threading
import time

from paged_testlib import (RespClient, arm_fault, disarm_fault, marker_count,
                           require_equal, shed_all_pages, wait_until)


def concurrent_round(port: int, actions, started: threading.Event,
                     release: threading.Event):
    barrier = threading.Barrier(len(actions) + 1)
    replies = [None] * len(actions)
    failures = []

    def actor(index, command):
        try:
            with RespClient(port=port, timeout=60) as client:
                barrier.wait(timeout=10)
                if index == 0:
                    started.set()
                replies[index] = client.command(*command)
        except BaseException as exc:
            failures.append((index, repr(exc)))
        finally:
            release.set()

    threads = [threading.Thread(target=actor, args=(i, command), daemon=True)
               for i, command in enumerate(actions)]
    for thread in threads:
        thread.start()
    barrier.wait(timeout=10)
    return threads, replies, failures


def wait_threads(threads, failures) -> None:
    for thread in threads:
        thread.join(timeout=65)
    if any(thread.is_alive() for thread in threads):
        raise AssertionError("a memory-refused actor never resumed")
    if failures:
        raise AssertionError("memory-refused actors failed: %r" % failures)


def best_effort_disarm(client: RespClient, name: str) -> None:
    """Do not mask a server crash with the cleanup connection error."""
    try:
        disarm_fault(client, name)
    except OSError:
        pass


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=7399)
    parser.add_argument("--log-dir", required=True)
    parser.add_argument("--checkpoint-wait", type=float, default=12.0)
    parser.add_argument("--fields", type=int, default=180)
    args = parser.parse_args()
    if args.fields < 24:
        parser.error("--fields must be at least 24")

    key = b"paged:memory-pressure"
    values = {}
    try:
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
            scan = control.command(b"HSCAN", key, b"0", b"COUNT", b"1",
                                   b"NOVALUES")
            if not isinstance(scan, list) or scan[0] == b"0":
                raise AssertionError("precondition failed: object is not paged")
            time.sleep(args.checkpoint_wait)

            # Single reader: prove refusal, no premature reply, other-key
            # progress, then exact retry completion after disarm.
            shed_all_pages(control, args.log_dir, timeout=30)
            marker = "FAULTLOG force_page_admission_refusal"
            before = marker_count(args.log_dir, marker)
            arm_fault(control, "force_page_admission_refusal")
            started = threading.Event()
            finished = threading.Event()
            threads = []
            try:
                actions = [(b"HGET", key, b"f00003")]
                threads, replies, failures = concurrent_round(
                    args.port, actions, started, finished)
                wait_until(started.is_set, 5, label="single refused reader start")
                wait_until(lambda: marker_count(args.log_dir, marker) > before,
                           5, 0.05, "page-admission refusal marker")
                if finished.is_set() or not threads[0].is_alive():
                    raise AssertionError("reader completed while admission was refused")
                require_equal(control.command(b"SET", b"paged:memory:other",
                                              b"live"), b"OK",
                              "other-key write during refusal")
                require_equal(control.command(b"PING"), b"PONG",
                              "PING during refusal")
            finally:
                best_effort_disarm(control, "force_page_admission_refusal")
            wait_threads(threads, failures)
            require_equal(replies, [values[b"f00003"]],
                          "single reader after reclaim")
            print("phase single refusal/reclaim: exact read resumed", flush=True)

            # Three actors: all start against a metadata-only object while the
            # gate refuses every reservation attempt. Once admitted, readers
            # see committed bytes and the writer updates one existing field.
            # The first refusal starts a real cleaning campaign.  After the
            # reader is admitted, that still-running campaign may shed its
            # newly fetched page before this test can arm shed_all_pages
            # again (observed reliably in the WAL lanes).  In that case a
            # missing shed marker means the desired metadata-only state was
            # already reached, not that the next round is vacuous.  Make the
            # second shed best-effort; the admission-refusal marker below and
            # the fact that all three clients remain pending are the stronger
            # proof that their commands actually faulted missing pages.
            try:
                reshed = shed_all_pages(control, args.log_dir, timeout=5)
                print("phase concurrent setup: shed=%d" % reshed,
                      flush=True)
            except TimeoutError:
                print("phase concurrent setup: no resident clean page "
                      "remained to shed; proving faults via admission",
                      flush=True)
            before = marker_count(args.log_dir, marker)
            arm_fault(control, "force_page_admission_refusal")
            started = threading.Event()
            finished = threading.Event()
            threads = []
            try:
                reader1 = b"f00005"
                reader2 = b"f%05d" % (args.fields // 2)
                writer = b"f%05d" % (args.fields - 7)
                actions = [
                    (b"HGET", key, reader1),
                    (b"HGET", key, reader2),
                    (b"HSET", key, writer, b"writer-after-reclaim"),
                ]
                threads, replies, failures = concurrent_round(
                    args.port, actions, started, finished)
                wait_until(started.is_set, 5, label="concurrent refused actors")
                wait_until(lambda: marker_count(args.log_dir, marker) > before,
                           5, 0.05, "concurrent admission refusal marker")
                time.sleep(0.25)
                if any(not thread.is_alive() for thread in threads):
                    raise AssertionError(
                        "an actor completed while page admission was refused")
            finally:
                best_effort_disarm(control, "force_page_admission_refusal")
            wait_threads(threads, failures)
            require_equal(replies[0], values[reader1], "reader 1 after reclaim")
            require_equal(replies[1], values[reader2], "reader 2 after reclaim")
            require_equal(replies[2], 0, "writer reply after reclaim")
            require_equal(control.command(b"HGET", key, writer),
                          b"writer-after-reclaim", "writer readback")
            require_equal(control.command(b"HLEN", key), args.fields,
                          "cardinality after pressure")
            require_equal(control.command(b"HSET", key, b"post", b"ok"), 1,
                          "post-pressure write")
            require_equal(control.command(b"PING"), b"PONG",
                          "post-pressure health")
            print("phase 2R+1W refusal/reclaim: all resumed exactly", flush=True)
    except (AssertionError, OSError, TimeoutError) as exc:
        print("RESULT: FAIL: %s" % exc, flush=True)
        return 1

    print("RESULT: PASS", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
