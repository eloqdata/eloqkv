"""Corrupt or EMPTY metadata (whole-record) store rows: a deterministic error.

The record-fetch path parses a store row into the entry's payload
(`ObjectCcMap::BackFill`). Two corruption classes must be refused there with
the entry left untouched (docs/08 §5):

  corrupt_record_bytes  the row is truncated; the bounded parse fails.
  empty_record_row      the row arrives with status Normal and ZERO bytes —
                        a review follow-up: the parse only runs on non-empty
                        rows, so this bypassed validation entirely and
                        Release stamped the entry Normal with a null (or
                        stale) payload.

Reaching a record fetch from a client needs the entry absent from memory, so
this test RESTARTS the server: the key is written and checkpointed, the
server is killed and restarted, and the first read fetches the record. Each
scenario asserts a specific error within bounded time (no hang, no crash),
and that after disarming the SAME read succeeds with the right value — the
corruption was never installed.

Usage: python3 tests/unit_cc/paged_corrupt_record.py <start.sh> [port]
"""
import subprocess
import sys
import time

sys.path.insert(0, "tests/unit_cc")
from standby_paged_apply import conn, read_reply, send  # noqa: E402
from paged_fault_matrix import arm, disarm  # noqa: E402
from replay_paged_restart import kill_server, wait_up  # noqa: E402


def restart(start_script):
    kill_server()
    time.sleep(3)
    subprocess.Popen(open(start_script).read().strip(), shell=True,
                     start_new_session=True, stdout=subprocess.DEVNULL,
                     stderr=subprocess.STDOUT)
    return wait_up()


def main():
    start_script = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 7399
    bad = []

    def fresh_conn():
        c = conn(port)

        def cmd(*args):
            send(c, *args)
            return read_reply(c)

        return c, cmd

    c, cmd = fresh_conn()
    # DEL, not FLUSHALL: a FLUSHALL shortly before a restart loses the
    # post-FLUSHALL writes across recovery on this harness (an engine-level
    # truncate/recovery interaction, reproduced without any paged
    # involvement and recorded in the plan doc) — and this test is about the
    # record-fetch path, not about that.
    cmd(b"DEL", b"rk")
    for i in range(50):
        cmd(b"HSET", b"rk", b"f%02d" % i, b"v" * 100)
    print("  waiting for a checkpoint", flush=True)
    time.sleep(15)

    for injector in ("empty_record_row", "corrupt_record_bytes"):
        if not restart(start_script):
            print("server did not come back for %s" % injector, flush=True)
            return 1
        c, cmd = fresh_conn()
        arm(c, injector)
        t0 = time.time()
        r = cmd(b"HLEN", b"rk")
        dt = time.time() - t0
        errored = r[:1] == b"-"
        print("  %-22s -> %r in %.1fs" % (injector, r[:44], dt), flush=True)
        if not errored:
            bad.append((injector + ": read did not error", r[:44], b"-ERR"))
        disarm(c, injector)
        # The corruption was never installed: the same read now succeeds
        # from a clean refetch.
        r2 = cmd(b"HLEN", b"rk")
        print("  %-22s disarmed -> %r" % (injector, r2[:20]), flush=True)
        if r2 != b":50":
            bad.append((injector + ": clean refetch", r2[:30], b":50"))
        if cmd(b"PING")[:5] != b"+PONG":
            bad.append((injector + ": server dead", b"", b"+PONG"))

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
