"""The §11 gating story, end to end (docs/08-paged-objects.md).

The conversion threshold is the feature's ONLY gate, and it gates creation
alone. Two consequences must both hold, and this test drives them across a
real restart:

  dark by default   a server started without the threshold flag converts
                    nothing: a large new hash stays monolithic.
  reading is        the same dark server still serves paged objects written
  permanent         while the feature was on — completely, and writably.

This is the rolling-upgrade story: a cluster that once enabled conversion can
be restarted with the feature dark (config rollback, a node whose config is
behind) and loses neither the data nor the ability to write it. Only NEW
conversions stop.

Representation is probed through the one protocol-visible difference: HSCAN.
A paged hash honours COUNT (a small COUNT on a large object returns a nonzero
cursor); the monolithic hash returns the entire object in one reply with
cursor 0.

  Phase A (threshold=1): build hash P -> paged (HSCAN cursor != 0); wait out
          a checkpoint so P's rows are durable.
  Phase B (no threshold flag): restart dark. P still paged, complete, and
          writable; a NEW 500-field hash M is monolithic (cursor == 0, one
          reply) despite being far over any plausible threshold.

Usage: python3 tests/unit_cc/paged_dark_feature.py <start_paged.sh>
                                                   <start_dark.sh>
"""
import subprocess
import sys
import time

sys.path.insert(0, "tests/unit_cc")
from standby_paged_apply import conn, read_reply, send  # noqa: E402
from replay_paged_restart import kill_server, wait_up  # noqa: E402


def hscan_first(sock, key, count):
    """One HSCAN step. Returns (cursor bytes, number of fields in reply)."""
    send(sock, "HSCAN", key, "0", "COUNT", str(count), "NOVALUES")
    buf = b""

    def more():
        nonlocal buf
        chunk = sock.recv(65536)
        if not chunk:
            raise OSError("closed")
        buf += chunk

    def line():
        nonlocal buf
        while b"\r\n" not in buf:
            more()
        ln, buf = buf.split(b"\r\n", 1)
        return ln

    def bulk(n):
        nonlocal buf
        while len(buf) < n + 2:
            more()
        out, buf = buf[:n], buf[n + 2:]
        return out

    head = line()
    assert head.startswith(b"*"), head
    cursor = None
    fields = 0
    for _ in range(int(head[1:])):
        el = line()
        if el.startswith(b"$"):
            v = bulk(int(el[1:]))
            if cursor is None:
                cursor = v
            else:
                fields += 1
        elif el.startswith(b"*"):
            for _ in range(int(el[1:])):
                el2 = line()
                bulk(int(el2[1:]))
                fields += 1
    return cursor, fields


def build(sock, key, n, prefix):
    for i in range(n):
        send(sock, "HSET", key, "%s%05d" % (prefix, i), "x" * 200)
        r = read_reply(sock)
        if r[:1] != b":":
            raise OSError("HSET not acked: %r" % r[:40])


def hlen(sock, key):
    send(sock, "HLEN", key)
    r = read_reply(sock)
    return int(r[1:]) if r[:1] == b":" else None


def main():
    start_paged, start_dark = sys.argv[1], sys.argv[2]
    ok = True

    # ---- Phase A: feature on ---------------------------------------------
    if not wait_up():
        print("paged-mode server not up", flush=True)
        return 1
    c = conn(7399)
    send(c, "DEL", "dark:paged")
    read_reply(c)
    build(c, "dark:paged", 500, "p")
    cursor, fields = hscan_first(c, "dark:paged", 10)
    paged_a = cursor != b"0"
    print("phase A: HSCAN COUNT 10 -> cursor=%r fields=%d (paged=%s)"
          % (cursor, fields, paged_a), flush=True)
    ok = ok and paged_a
    # A checkpoint (interval 5s in the harness) makes the paged rows durable.
    time.sleep(15)
    c.close()

    # ---- Phase B: restart dark -------------------------------------------
    kill_server()
    time.sleep(4)
    cmd = open(start_dark).read().strip()
    subprocess.Popen(cmd, shell=True, start_new_session=True,
                     stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    if not wait_up():
        print("dark server never came up", flush=True)
        return 1
    c = conn(7399)

    # The old paged object: still paged, complete, writable.
    n = hlen(c, "dark:paged")
    cursor, fields = hscan_first(c, "dark:paged", 10)
    paged_b = cursor != b"0"
    print("phase B: old object HLEN=%r, HSCAN cursor=%r (still paged=%s)"
          % (n, cursor, paged_b), flush=True)
    ok = ok and n == 500 and paged_b
    send(c, "HSET", "dark:paged", "afterdark", "y")
    r = read_reply(c)
    print("phase B: write to the paged object -> %r" % r[:20], flush=True)
    ok = ok and r[:1] == b":" and hlen(c, "dark:paged") == 501

    # A new large hash: stays monolithic.
    send(c, "DEL", "dark:mono")
    read_reply(c)
    build(c, "dark:mono", 500, "m")
    cursor, fields = hscan_first(c, "dark:mono", 10)
    mono = cursor == b"0" and fields == 500
    print("phase B: new hash HSCAN cursor=%r fields=%d (monolithic=%s)"
          % (cursor, fields, mono), flush=True)
    ok = ok and mono

    print("RESULT:", "PASS" if ok else "FAIL", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
