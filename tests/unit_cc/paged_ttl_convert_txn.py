"""Conversion and a TTL reset inside ONE transaction (a review follow-up).

The hazard window: a monolithic TTL hash crosses the conversion threshold
mid-transaction, so the DIRTY payload is already a paged twin while the
committed payload is still monolithic; a PERSIST later in the same
transaction then runs on the paged dirty object. The engine's §16 guard
originally examined the COMMITTED payload, set ttl_reset_, and staged the
forbidden paged recover image — which WAL replay dies on (by design, loudly).

The guard now follows the payload the TTL came from, and the command layer
never builds the image for a paged object. This test drives the window end
to end and then CRASH-RESTARTS so WAL replay actually consumes what was
logged: pre-fix the restart aborts on the recover image; post-fix the plain
commands replay through the CommitOn twin swaps.

Run against a server with a threshold ABOVE a small hash and BELOW the
transaction's growth. Reproducible from a fresh checkout via the checked-in
harness:

    env ELOQKV_PAGED_THRESHOLD=4096 ELOQKV_PAGED_PAGE_SIZE=131072 \\
        tests/unit_cc/paged_single_node.sh up --wal on
    python3 tests/unit_cc/paged_ttl_convert_txn.py <printed start.sh> [port]

(<start.sh> is the generated script the harness prints; any equivalently
configured start script works.)
"""
import subprocess
import sys
import time

sys.path.insert(0, "tests/unit_cc")
from standby_paged_apply import conn, read_reply, send  # noqa: E402
from replay_paged_restart import kill_server, wait_up  # noqa: E402


def main():
    start_script = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 7399
    bad = []
    c = conn(port)

    def cmd(*args):
        send(c, *args)
        return read_reply(c)

    def paged(key):
        """HSCAN COUNT 1 cursor: nonzero = paged, 0 = monolithic."""
        send(c, b"HSCAN", key, b"0", b"COUNT", b"1", b"NOVALUES")
        # Array header, then cursor bulk; read_reply drains the whole array.
        # Re-issue via read_reply's parsing: simplest is HSCAN then compare
        # the raw reply prefix — instead use two probes below.
        return read_reply(c)

    cmd(b"DEL", b"h")
    r = cmd(b"HSET", b"h", b"a", b"1")
    if r != b":1":
        bad.append(("setup HSET", r[:20], b":1"))
    r = cmd(b"PEXPIRE", b"h", b"600000")
    if r != b":1":
        bad.append(("setup PEXPIRE", r[:20], b":1"))
    print("  monolithic TTL hash ready; waiting out a checkpoint", flush=True)
    time.sleep(15)

    # The transaction: conversion (5 KB post-image > 4 KB threshold), then
    # the TTL reset — both on the DIRTY payload.
    big = b"x" * 5000
    if cmd(b"MULTI") != b"+OK":
        bad.append(("MULTI", b"", b"+OK"))
    cmd(b"HSET", b"h", b"big", big)   # +QUEUED
    cmd(b"PERSIST", b"h")             # +QUEUED
    r = cmd(b"EXEC")
    print("  EXEC -> %r" % r[:30], flush=True)
    if r[:1] != b"*":
        bad.append(("EXEC", r[:40], b"array"))

    ttl = cmd(b"PTTL", b"h")
    hlen = cmd(b"HLEN", b"h")
    print("  after txn: PTTL=%r HLEN=%r" % (ttl, hlen), flush=True)
    if ttl != b":-1":
        bad.append(("PTTL after txn", ttl, b":-1"))
    if hlen != b":2":
        bad.append(("HLEN after txn", hlen, b":2"))

    # Crash BEFORE the next checkpoint so WAL replay must consume the
    # transaction's log records — the pre-fix abort site.
    kill_server()
    time.sleep(3)
    subprocess.Popen(open(start_script).read().strip(), shell=True,
                     start_new_session=True, stdout=subprocess.DEVNULL,
                     stderr=subprocess.STDOUT)
    if not wait_up():
        print("server did not come back — the pre-fix failure mode",
              flush=True)
        print("RESULT: FAIL", flush=True)
        return 1
    c2 = conn(port)

    def cmd2(*args):
        send(c2, *args)
        return read_reply(c2)

    time.sleep(3)
    ttl = cmd2(b"PTTL", b"h")
    hlen = cmd2(b"HLEN", b"h")
    val = cmd2(b"HGET", b"h", b"a")
    print("  after crash-restart replay: PTTL=%r HLEN=%r HGET a=%r"
          % (ttl, hlen, val), flush=True)
    if ttl != b":-1":
        bad.append(("PTTL after replay", ttl, b":-1"))
    if hlen != b":2":
        bad.append(("HLEN after replay", hlen, b":2"))

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
