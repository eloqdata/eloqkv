"""More memory waiters than ONE dequeue batch, through the TERMINAL branch.

`DequeueWaitListAfterMemoryFree` releases at most 20 requests per call and
reports whether the list is now empty. Every terminal branch of a cleaning
campaign used to DISCARD that result and retire the cleaner, so with more
than 20 parked waiters the remainder were stranded permanently: the woken 20
may all succeed and therefore never request another campaign, and releasing a
pin does not wake the cleaner (docs/08 §8).

Staging that is harder than it looks, and two earlier attempts proved nothing:

  wrong branch     on a test-sized heap the campaign branch is never entered
                   (it is gated on Full() before NeedMoreCleaning is
                   consulted), so waiters were drained by the outer
                   no-campaign path, which already honoured the result.
                   `force_clean_target_unmet` forces the shortfall.
  wrong list size  25 unfinished CLIENTS is not 25 PARKED requests: the
                   cleaner drains continuously, so requests are dequeued and
                   refused over and over while the list stays under 20. Every
                   terminal marker then reads waiters_remaining=0 and the
                   batch boundary is never crossed. `defer_shard_clean_drain`
                   keeps the cleaner alive while draining nothing, so the
                   list actually accumulates.

Sequence: park 25 under forced refusal WITH the drain deferred, wait until
the shard reports >= 25 parked, then disarm refusal and the defer hook while
keeping the target shortfall armed. The first 20 woken can now succeed
WITHOUT re-requesting a campaign, so only the cleaner's own re-enqueue can
wake 21-25 — precisely the fixed behaviour.

Usage: python3 tests/unit_cc/paged_admission_batch_drain.py <port> <log_dir>
"""
import glob
import os
import re
import sys
import threading
import time

sys.path.insert(0, "tests/unit_cc")
from paged_testlib import (RespClient, arm_fault, disarm_fault,  # noqa: E402
                           shed_all_pages, wait_until)

KEY = b"paged:batch-drain"
FIELDS = 180
WAITERS = 25          # deliberately > the 20-per-call dequeue batch


def _log_lines(log_dir):
    for path in glob.glob(os.path.join(log_dir, "eloqdb.log.INFO*")):
        try:
            with open(path, errors="ignore") as fh:
                for line in fh:
                    yield line
        except OSError:
            pass


def deferred_waiters(log_dir):
    """@return the largest wait-list size the defer hook has reported."""
    best = 0
    for line in _log_lines(log_dir):
        m = re.search(r"FAULTLOG defer_shard_clean_drain waiters=(\d+)", line)
        if m:
            best = max(best, int(m.group(1)))
    return best


def terminal_remaining(log_dir):
    """@return count of terminal markers that still had waiters queued."""
    n = 0
    for line in _log_lines(log_dir):
        m = re.search(r"CLEANLOG terminal .*waiters_remaining=(\d+)", line)
        if m and m.group(1) == "1":
            n += 1
    return n


def main():
    port = int(sys.argv[1])
    log_dir = sys.argv[2]
    bad = []

    c = RespClient(port=port)
    c.command(b"DEL", KEY)
    for i in range(FIELDS):
        c.command(b"HSET", KEY, b"f%05d" % i, b"v" * 180)
    time.sleep(8)
    shed_all_pages(c, log_dir, timeout=30)

    remaining_before = terminal_remaining(log_dir)

    arm_fault(c, "force_clean_target_unmet")
    arm_fault(c, "defer_shard_clean_drain")
    arm_fault(c, "force_page_admission_refusal")

    replies = {}

    def reader(n):
        try:
            replies[n] = RespClient(port=port).command(
                b"HGET", KEY, b"f%05d" % n)
        except Exception as exc:  # noqa: BLE001 - recorded, not raised
            replies[n] = "ERR:%s" % exc

    threads = [threading.Thread(target=reader, args=(n,), daemon=True)
               for n in range(WAITERS)]
    for t in threads:
        t.start()

    try:
        wait_until(lambda: deferred_waiters(log_dir) >= WAITERS, 90, 0.2,
                   "the memory wait list to reach %d parked requests"
                   % WAITERS)
        print("  wait list reached %d parked requests"
              % deferred_waiters(log_dir), flush=True)
        if replies:
            bad.append(("readers completed while refusal armed",
                        str(len(replies)).encode(), b"0"))
    finally:
        for fault in ("force_page_admission_refusal",
                      "defer_shard_clean_drain"):
            try:
                disarm_fault(c, fault)
            except Exception:  # noqa: BLE001
                pass

    for t in threads:
        t.join(timeout=180)
    try:
        disarm_fault(c, "force_clean_target_unmet")
    except Exception:  # noqa: BLE001
        pass

    crossed = terminal_remaining(log_dir) - remaining_before
    stranded = sorted(set(range(WAITERS)) - set(replies))
    wrong = sorted(n for n, v in replies.items()
                   if not (isinstance(v, bytes) and v.startswith(b"v")))
    print("  terminal markers with waiters_remaining=1: %d" % crossed,
          flush=True)
    print("  after disarm: completed=%d/%d stranded=%s wrong=%s"
          % (len(replies), WAITERS, stranded, wrong), flush=True)

    if crossed <= 0:
        bad.append(("no terminal dequeue saw more than one batch", b"0",
                    b">=1 waiters_remaining=1"))
    if stranded:
        bad.append(("waiters stranded past the dequeue batch",
                    str(stranded).encode(), b"none"))
    if wrong:
        bad.append(("waiters got wrong replies", str(wrong).encode(), b"none"))
    if c.command(b"PING") != b"PONG":
        bad.append(("server unresponsive", b"", b"PONG"))

    if bad:
        for label, got, want in bad:
            print("  FAIL: %s -> got %r want %r" % (label, got, want),
                  flush=True)
        print("RESULT: FAIL", flush=True)
        return 1
    print("RESULT: PASS", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
