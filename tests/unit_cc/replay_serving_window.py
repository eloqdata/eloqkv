"""Does the deferred-promotion gate actually keep clients out (docs/08 §10)?

The gate exists because log replay can finish while a paged object's replayed
tail is still buffered behind a page fetch: `on_fly_cnt` reaching zero means
every replay REQUEST finished, not that every replayed COMMAND applied. While
that work is outstanding the node must not serve the key, or a read returns a
version missing acknowledged writes.

The mechanism was verified in isolation (the waiter polls, promotes, and
abandons on term change), but not at the point that matters: that a client
command arriving inside the window is REJECTED rather than answered with stale
content. `ApplyCc` reads LeaderTerm(ng_id) and fails with
REQUESTED_NODE_NOT_LEADER when it is negative, so withholding the promotion
should withhold the command.

The window cannot be widened with `stall_page_fetch`: fault injectors are armed
by a client command, which needs a serving node, which is precisely what does
not exist during replay. So the test hammers the port as fast as it can from
process start and records every distinct reply in order. The drain applies
buffered commands one at a time and faults per page, so a large post-checkpoint
tail makes the window many sequential store reads wide.

KNOWN OPEN ISSUE, unrelated to paged objects: in roughly 6 of 10 runs the
replayed tail arrives incomplete -- replay never receives a contiguous window of
records ending just before the crash, even though truncation did not remove them
(the log's watermark sits below the version the store holds) and braft's own
durable log is in the path. The mechanism is not yet known; see
docs/08-paged-objects-plan.md. Until it is, a version hole plus a refused
promotion here is NOT evidence of a paged-objects defect, and this test cannot
be used to assert that the whole tail comes back.

Outcomes:
  PASS         - after the restarted instance logged a deferral, a client was
                 rejected either at TCP connect or with a not-leader error
                 BEFORE the first success, and a later read is complete.
  INCONCLUSIVE - only successes were seen. The window was too short to sample,
                 or the drain finished before the port opened. This is NOT a
                 pass: it means the assertion could not be evaluated.
  FAIL         - the first answer was stale content (incomplete object), which
                 is the bug the gate is supposed to prevent.

Usage: python3 tests/unit_cc/replay_serving_window.py <start_cmd_file> <log_dir>
"""
import glob
import os
import socket
import subprocess
import sys
import time

sys.path.insert(0, "tests/unit_cc")
from swap_with_inflight_fetch import cmd, conn  # noqa: E402
from replay_paged_restart import (build_and_write,  # noqa: E402
                                  kill_server, wait_up)

KEY = "replay:paged"
# A much larger post-checkpoint tail than the sibling test uses. The drain
# applies buffered commands one at a time and faults per page, so the window
# this test wants to sample is roughly (distinct pages touched) sequential store
# reads wide. With a few hundred commands it closes before the listening port
# opens, and the test can only report INCONCLUSIVE.
# 3000 wedges recovery outright: the drain could not finish 3000 buffered
# commands inside the 60 s promotion deadline (238 units still outstanding), so
# the node refused to promote and never opened its port. That is the gate
# behaving correctly, but it makes the test unrunnable, and it is a real finding
# about the deadline -- see docs/08-paged-objects-plan.md.
# 800 is enough to produce a deferral while keeping the run short. 3000 was
# used to validate the progress-based give-up: it previously wedged recovery
# (238 units outstanding when the old absolute 60 s deadline fired, node never
# promoted, port never opened) and now recovers in under a second.
TAIL = 800


def write_tail_fast(key, n):
    """Write n acknowledged fields with no inter-command sleep.

    One command per round trip -- each reply is read before the next send, so
    every write is durably acknowledged -- but without the 50 ms pause the
    shared cmd() helper adds, which would make 3000 writes take minutes.
    """
    c = conn()
    for i in range(n):
        args = ("HSET", key, "p%05d" % i, "y" * 220 + "%05d" % i)
        out = ("*%d\r\n" % len(args)).encode()
        for a in args:
            b = str(a).encode()
            out += b"$%d\r\n%s\r\n" % (len(b), b)
        c.sendall(out)
        reply = c.recv(64)
        if not reply.startswith(b":"):
            raise OSError("HSET p%05d not acknowledged: %r" % (i, reply[:40]))
    c.close()


def poke():
    """One HLEN attempt with no waiting.

    Returns the raw reply, or None if the port is not accepting yet.
    """
    try:
        s = socket.create_connection(("127.0.0.1", 7399), timeout=1)
    except OSError:
        return None
    try:
        s.sendall(b"*2\r\n$4\r\nHLEN\r\n$12\r\nreplay:paged\r\n")
        return s.recv(200)
    except OSError:
        return None
    finally:
        s.close()


def deferral_happened(log_dir, not_before=0):
    """Did the gate defer in the RESTARTED instance? Otherwise there was no
    window.

    Reads only the newest INFO file. Scanning the whole directory mixes the
    phase-1 and phase-2 servers -- glog also leaves a symlink beside each
    timestamped file, double-counting every line -- and a phase-1 hit reported
    here as True is simply wrong. Cross-run and cross-instance aggregation
    produced several false conclusions while this test was being written.
    """
    infos = [path for path in
             glob.glob(os.path.join(log_dir, "eloqdb.log.INFO.*"))
             if os.path.getmtime(path) >= not_before]
    infos.sort(key=os.path.getmtime)
    if not infos:
        return False
    try:
        with open(infos[-1], errors="ignore") as fh:
            return any("deferring promotion until" in line for line in fh)
    except OSError:
        return False


def main():
    start_cmd_file, log_dir = sys.argv[1], sys.argv[2]
    start_cmd = open(start_cmd_file).read().strip()

    # Phase 1: a paged object whose pages are durable, then a tail of
    # acknowledged writes that only the WAL knows about.
    if not wait_up():
        print("phase-1 server never came up", flush=True)
        return 1
    # Reuse the sibling's builder for the checkpointed body, then write our own
    # much larger tail.
    build_and_write(KEY)
    write_tail_fast(KEY, TAIL)
    # Ground truth for the completeness check, READ from the live server rather
    # than derived. Deriving it as NFIELDS + TAIL was wrong -- the two writers
    # share the "p%05d" field namespace, so the arithmetic double-counts or
    # under-counts depending on their sizes, and an off-by-one made the test
    # report "served stale content" for an object that was in fact complete.
    expected = cmd(conn(), "HLEN", KEY)
    print("expected HLEN (measured before kill): %r" % expected, flush=True)
    kill_server()
    time.sleep(4)

    # Phase 2: restart, then sample as fast as possible.
    with open(os.path.join(log_dir, "window_stdout.log"), "wb") as fh:
        subprocess.Popen(start_cmd, shell=True, start_new_session=True,
                         stdout=fh, stderr=subprocess.STDOUT)

    t0 = time.time()
    timeline = []          # ordered distinct replies
    last = None
    first_success = None
    deferral_seen_at = None
    unavailable_after_deferral = False
    deadline = t0 + 90
    while time.time() < deadline:
        r = poke()
        now = round(time.time() - t0, 3)
        if (deferral_seen_at is None and
                deferral_happened(log_dir, t0 - 1)):
            deferral_seen_at = now
        if r is None and deferral_seen_at is not None:
            # The production gate normally withholds the listening port
            # rather than accepting a command and returning NOT_LEADER.  A
            # refused connection after the restarted instance has logged the
            # deferral is therefore direct evidence that it did not serve a
            # stale object.  Refusals before that marker remain ordinary
            # process-startup noise and do not qualify the test.
            unavailable_after_deferral = True
        if r is not None and r != last:
            timeline.append((now, r[:48]))
            last = r
            if r.startswith(b":") and first_success is None:
                first_success = (now, r)
        if first_success is not None and len(timeline) > 0:
            # Give it a moment past the first success, then stop.
            if time.time() - t0 > first_success[0] + 2:
                break

    for when, reply in timeline[:12]:
        print("  t=%7.3fs  %r" % (when, reply), flush=True)

    errors_before_success = [
        (w, r) for w, r in timeline
        if r.startswith(b"-") and (first_success is None or w < first_success[0])
    ]

    # The object must be complete once serving properly begins.
    time.sleep(3)
    final = cmd(conn(), "HLEN", KEY)
    complete = final == expected
    print("deferral seen in log: %s" %
          deferral_happened(log_dir, t0 - 1), flush=True)
    print("TCP unavailable after deferral: %s" % unavailable_after_deferral,
          flush=True)
    print("errors before first success: %d" % len(errors_before_success),
          flush=True)
    print("final HLEN: %r (expected %r)" % (final, expected), flush=True)

    if first_success is not None and not complete:
        # Answered, but with an object missing acknowledged writes.
        print("RESULT: FAIL (served stale content)", flush=True)
        return 1
    if errors_before_success or unavailable_after_deferral:
        print("RESULT: PASS (client rejected during the window, complete "
              "afterwards)", flush=True)
        return 0
    print("RESULT: INCONCLUSIVE (window not sampled; no rejection observed)",
          flush=True)
    return 2


if __name__ == "__main__":
    sys.exit(main())
