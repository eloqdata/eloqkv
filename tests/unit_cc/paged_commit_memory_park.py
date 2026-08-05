"""The §8 write-side memory park: a paged commit stalls, never fails.

A committed-in-WAL transaction cannot be refused, so when the shard heap is
over budget the commit parks BEFORE mutating, holding the write lock, and is
re-driven until memory admits it (docs/08-paged-objects.md §8, §16). Three
properties are asserted here, driven by the `force_paged_commit_park`
injector because filling a real shard heap on demand is neither fast nor
reliable:

  stall, not fail   an HSET to a paged key issued while the park is forced
                    does not reply until the injector is disarmed, then
                    succeeds — the write is delayed, never errored or lost.
  reads keep going  lock-free ReadCommitted reads of the SAME key (HGET) and
                    other keys work during the park: the pre-image stays
                    visible, exactly as in the WAL gap.
  lock is held      a second writer to the same key, sent during the park,
                    completes only after the parked one — and both land
                    (field count proves neither was dropped).

Requires a Debug build with WAL ENABLED. The no-WAL ``apply_and_commit`` path
is deliberately ungated (docs/08 §8), so running this test in S0/P0 is a
vacuous harness error: it cannot reach PostWriteCc or its injector. Run against
an S1/P1 paged server:
    python3 tests/unit_cc/paged_commit_memory_park.py PORT LOG_DIR --wal-enabled
"""
import argparse
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(__file__))
from paged_testlib import marker_count  # noqa: E402
from standby_paged_apply import conn, read_reply, send  # noqa: E402


def cmd(sock, *args):
    send(sock, *args)
    return read_reply(sock)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("port", nargs="?", type=int, default=7399)
    parser.add_argument("log_dir")
    parser.add_argument(
        "--wal-enabled", action="store_true",
        help="required acknowledgement that this is an S1/P1 deployment")
    args = parser.parse_args()
    if not args.wal_enabled:
        parser.error("memory-park coverage requires --wal-enabled (S1/P1)")
    port = args.port
    c = conn(port)
    bad = []

    cmd(c, b"FLUSHALL")
    # A paged object (threshold=1 in the harness) with a few fields.
    for i in range(20):
        cmd(c, b"HSET", b"park", b"f%02d" % i, b"v" * 64)
    r = cmd(c, b"DUMP", b"park")
    if r[:1] != b"-":
        bad.append(("precondition: key is not paged", r[:40], b"paged"))

    marker = "FAULTLOG force_paged_commit_park"
    marker_before = marker_count(args.log_dir, marker)
    cmd(c, b"fault_inject", b"force_paged_commit_park", b"-1")

    results = {}
    t1 = None
    t2 = None

    def writer(tag, field):
        w = conn(port)
        t0 = time.time()
        r = cmd(w, b"HSET", b"park", field, b"delayed")
        results[tag] = (time.time() - t0, r)
        w.close()

    try:
        t1 = threading.Thread(target=writer, args=("w1", b"parked1"))
        t1.start()
        time.sleep(1.0)

        if marker_count(args.log_dir, marker) <= marker_before:
            bad.append(("force_paged_commit_park injector never fired",
                        b"no new log marker", b"FAULTLOG marker"))
        t2 = threading.Thread(target=writer, args=("w2", b"parked2"))
        t2.start()
        time.sleep(1.0)

        # Neither writer may have completed while the park is forced.
        for tag in ("w1", "w2"):
            if tag in results:
                bad.append((tag + " completed DURING the park",
                            repr(results[tag]).encode(), b"parked"))

        # Lock-free reads still serve the pre-image, on this key and others.
        r = cmd(c, b"HGET", b"park", b"f00")
        print("  read during park (same key)  -> %r" % r[:20], flush=True)
        if r[:1] != b"$":
            bad.append(("HGET during park", r[:40], b"pre-image"))
        r = cmd(c, b"SET", b"other", b"x")
        print("  write during park (other key)-> %r" % r[:20], flush=True)
        if r[:1] != b"+":
            bad.append(("other-key write during park", r[:40], b"+OK"))
        r = cmd(c, b"PING")
        if r[:5] != b"+PONG":
            bad.append(("PING during park", r[:40], b"+PONG"))
    finally:
        # The matrix continues after a failed case. Never let this injector
        # poison crash-replay or standby scenarios that follow this test.
        cmd(c, b"fault_inject", b"force_paged_commit_park", b"-1", b"remove")
        for thread in (t1, t2):
            if thread is not None:
                thread.join(timeout=60)

    parked_secs = 2.0
    assert t1 is not None and t2 is not None
    if t1.is_alive() or t2.is_alive():
        bad.append(("a parked writer never resumed", b"", b"resumed"))
    else:
        for tag in ("w1", "w2"):
            elapsed, r = results[tag]
            print("  %s: %.1fs -> %r" % (tag, elapsed, r[:20]), flush=True)
            if r[:1] != b":":
                bad.append((tag + " reply after resume", r[:40], b":1"))
            if elapsed < parked_secs - 1.0:
                bad.append((tag + " was not actually parked",
                            str(elapsed).encode(), b">=park window"))

    # Both writes landed: nothing was dropped by the stall.
    r = cmd(c, b"HLEN", b"park")
    print("  final HLEN -> %r (expect :22)" % r[:10], flush=True)
    if r != b":22":
        bad.append(("final field count", r[:20], b":22"))

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
