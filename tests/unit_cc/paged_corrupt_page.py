"""Corrupt or missing store rows for LIVE pages: a deterministic error.

Before this was fixed, the fetch completion woke waiters with the TRANSPORT
flag alone, so a live page whose store row was missing (KEY_NOT_FOUND with
error_code 0) or whose bytes failed validation woke its waiters as SUCCESS —
the command re-ran, found the page still absent, and faulted it again,
forever. The contract now (docs/08 §5): the completion classifies benign
discards (payload superseded, id freed mid-flight) apart from corruption on
a live page, and the latter errors the waiter.

Two Debug-only injectors drive the two corruption classes:

  page_fetch_missing_row    the store "loses" the row (status Deleted).
  page_fetch_corrupt_bytes  the row arrives with garbage bytes, which the
                            §5 image validation rejects.

Both scenarios assert: the read returns a specific ERROR within bounded time
(never hangs, never loops), the server stays alive, and after disarming the
SAME read succeeds with the right value — the corruption was never installed.

The whole-record (metadata row) corruption path is covered end to end by
`paged_corrupt_record.py`, which reaches a record fetch via a restart, plus
the unit truncation sweep (TestBoundedStoreParse).

Run against a Debug server with --paged_hash_convert_threshold=1. Usage:
    python3 tests/unit_cc/paged_corrupt_page.py [port] [log_dir]
"""
import glob
import os
import sys
import time

sys.path.insert(0, "tests/unit_cc")
from standby_paged_apply import conn, read_reply, send  # noqa: E402
from paged_fault_matrix import arm, disarm  # noqa: E402

LOG_DIR = "bld-standby/single_log"


def fault_count(token):
    n = 0
    for path in glob.glob(os.path.join(LOG_DIR, "eloqdb.log.INFO*")):
        try:
            with open(path, errors="ignore") as fh:
                n += sum(1 for line in fh if token in line)
        except OSError:
            pass
    return n


def main():
    global LOG_DIR
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 7399
    if len(sys.argv) > 2:
        LOG_DIR = sys.argv[2]
    c = conn(port)
    bad = []

    def cmd(*args):
        send(c, *args)
        return read_reply(c)

    def strip_pages():
        before = fault_count("FAULTLOG shed_all_pages")
        arm(c, "shed_all_pages")
        arm(c, "force_shard_clean_now")
        time.sleep(3)
        disarm(c, "shed_all_pages")
        return fault_count("FAULTLOG shed_all_pages") > before

    cmd(b"FLUSHALL")
    for i in range(300):
        cmd(b"HSET", b"ck", b"f%04d" % i, b"v" * 200)
    # Pages must be durable before they can be shed.
    print("  waiting for a checkpoint to flush the pages", flush=True)
    time.sleep(8)

    # ONE strip serves both scenarios: each faults a DIFFERENT field, whose
    # page is still non-resident from the strip (only the previously probed
    # field's page gets refetched).
    if not strip_pages():
        bad.append(("precondition: nothing shed", b"", b"shed"))
    scenarios = ((b"page_fetch_corrupt_bytes",
                  "FAULTLOG page_fetch_corrupt_bytes", b"f0007"),
                 (b"page_fetch_missing_row",
                  "FAULTLOG page_fetch_missing_row", b"f0100"))
    for name, marker, field in scenarios:
        before = fault_count(marker)
        arm(c, name)
        t0 = time.time()
        r = cmd(b"HGET", b"ck", field)
        elapsed = time.time() - t0
        disarm(c, name)
        fired = fault_count(marker) > before
        print("  %-26s -> %r in %.1fs (injector fired=%s)"
              % (name.decode(), r[:44], elapsed, fired), flush=True)
        if not fired:
            bad.append((name.decode() + " never fired", b"", b"fired"))
        if r[:1] != b"-":
            bad.append((name.decode() + ": read did not error", r[:44],
                        b"deterministic error"))
        if elapsed > 30:
            bad.append((name.decode() + ": took too long", b"%.1fs" % elapsed,
                        b"bounded"))
        # Server alive, and the same read now succeeds with clean bytes.
        if cmd(b"PING")[:5] != b"+PONG":
            bad.append((name.decode() + ": server dead", b"", b"+PONG"))
        r2 = cmd(b"HGET", b"ck", field)
        print("  %-26s    after disarm -> %r" % ("", r2[:20]), flush=True)
        if r2 != b"$200":
            bad.append((name.decode() + ": read after disarm", r2[:44],
                        b"$200"))

    # The object is intact end to end.
    r = cmd(b"HLEN", b"ck")
    if r != b":300":
        bad.append(("final HLEN", r[:20], b":300"))

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
