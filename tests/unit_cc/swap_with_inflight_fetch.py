"""Payload swap / retire while a page fetch is IN FLIGHT (docs/08 §7).

This is the path ApplyPayloadSwapRule exists for: before the committed paged
payload is replaced or dropped, its in-flight fetches must be orphaned and
every request parked on one of its pages re-enqueued. Otherwise those commands
wait forever and a completion installs into a successor incarnation.

Reaching it needs two things a client cannot arrange on its own, so both come
from fault injectors (Debug builds only):

  shed_all_pages    - strips every evictable page from a paged object and KEEPS
                      the entry, producing "metadata resident, pages shed". A
                      merely-cold object is evicted WHOLE instead, and a read is
                      then a record fetch, not a page fetch -- which is why an
                      earlier version of this test passed while orphaning
                      exactly zero fetches.
  stall_page_fetch  - holds a fetch completion in the shard queue ~3s, so the
                      disruptor lands while the fetch is genuinely in flight.

Run against a Debug server with paged conversion enabled and a small
--node_memory_limit_mb: the clean pass only runs under real memory pressure.
"""
import socket
import sys
import threading
import time

HOST, PORT = "127.0.0.1", 7399
NFIELDS = 900
FILLERS = 20


def conn(timeout=60):
    return socket.create_connection((HOST, PORT), timeout=timeout)


def cmd(s, *args):
    out = ("*%d\r\n" % len(args)).encode()
    for a in args:
        b = a if isinstance(a, bytes) else str(a).encode()
        out += b"$%d\r\n%s\r\n" % (len(b), b)
    s.sendall(out)
    time.sleep(0.05)
    try:
        return s.recv(400000)
    except socket.timeout:
        return b"<TIMEOUT>"


def drain_lines(s, n):
    """Read exactly n one-line replies (`:1`, `+OK`, ...) and no more.

    A batch of 150 pipelined HSETs produces 150 replies, and reading them with
    a single sleep-then-recv leaves whatever had not arrived yet sitting in the
    socket. Those leftovers shift every later reply on that connection: the
    disruptor's reply reads back as four stale `:1`s and the closing PING reads
    as the disruptor's, so the scenario fails on a healthy server. Counting
    them is the fix -- the pause length never is, since it only changes how
    often the race is lost, and a faster server loses it more often.
    """
    buf = b""
    while buf.count(b"\r\n") < n:
        chunk = s.recv(65536)
        if not chunk:
            raise OSError("connection closed with %d of %d replies read"
                          % (buf.count(b"\r\n"), n))
        buf += chunk


def build_paged(s, key, nfields=NFIELDS):
    cmd(s, "DEL", key)
    batch = 150
    for base in range(0, nfields, batch):
        out = b""
        for i in range(base, base + batch):
            args = ("HSET", key, "f%05d" % i, "x" * 220 + "%05d" % i)
            out += ("*%d\r\n" % len(args)).encode()
            for a in args:
                b = str(a).encode()
                out += b"$%d\r\n%s\r\n" % (len(b), b)
        s.sendall(out)
        drain_lines(s, batch)


_filled = [False]


def make_pressure(s):
    """The clean pass only runs when the shard heap is actually over budget."""
    if _filled[0]:
        return
    for k in range(FILLERS):
        build_paged(s, "filler:%03d" % k)
    _filled[0] = True
    time.sleep(8)


def scenario(name, disruptor):
    a, b = conn(), conn()
    key = "swap:" + name
    # Pressure FIRST, target SECOND. Building the target before the fillers
    # makes it the coldest entry, so the clean pass evicts it WHOLE during the
    # filler phase and the later read is a record fetch -- the object must
    # still be resident for shed_all_pages to leave it metadata-only.
    make_pressure(b)
    build_paged(a, key)
    # Freshly written pages are DIRTY, and dirty pages are never shed (the
    # durability invariant is per page). Wait for a checkpoint to make them
    # clean, or shed_all_pages skips this object entirely and the read never
    # faults -- which is exactly how this test used to pass while doing nothing.
    time.sleep(9)

    # Force "metadata resident, pages shed": arm shed_all_pages, then run the
    # clean pass directly. force_shard_clean_now is required because the pass
    # is otherwise unreachable without genuine memory pressure -- it is only
    # scheduled when the memory wait list is non-empty, and its Execute() then
    # gates on shard_heap->Full() as well.
    cmd(b, "fault_inject", "shed_all_pages", -1)
    cmd(b, "fault_inject", "force_shard_clean_now", -1)
    time.sleep(3)

    # Disarm shedding before the read: leaving it armed re-sheds the page after
    # each successful install, so the reader faults in a loop instead of making
    # progress -- a test artifact, not product behaviour.
    cmd(b, "fault_inject", "shed_all_pages", -1, "remove")
    cmd(b, "fault_inject", "stall_page_fetch", -1)
    result = {}

    def reader():
        try:
            result["read"] = cmd(a, "HGET", key, "f00880")
        except OSError as e:
            result["read"] = ("<ERR %s>" % e).encode()

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    time.sleep(0.8)                      # fetch issued and stalled by now
    result["disrupt"] = disruptor(b, key)
    t.join(timeout=60)
    result["reader_returned"] = not t.is_alive()

    cmd(b, "fault_inject", "stall_page_fetch", -1, "remove")
    cmd(b, "fault_inject", "shed_all_pages", -1, "remove")
    time.sleep(0.5)
    result["after"] = cmd(b, "PING")
    # Only close `a` once the reader is definitely done with it; closing a
    # socket under a blocked thread raises EBADF in that thread.
    if result["reader_returned"]:
        a.close()
    b.close()
    return result


def main():
    ok = True
    for name, fn in [
        ("overwrite", lambda s, k: cmd(s, "SET", k, "now-a-string")),
        ("delete", lambda s, k: cmd(s, "DEL", k)),
        ("expire", lambda s, k: cmd(s, "PEXPIRE", k, 1)),
    ]:
        r = scenario(name, fn)
        good = (r["reader_returned"] and r["after"].strip() == b"+PONG"
                and r["read"] != b"<TIMEOUT>")
        ok = ok and good
        print("%-10s reader_returned=%s read=%r disrupt=%r ping=%r -> %s"
              % (name, r["reader_returned"], r["read"][:24], r["disrupt"][:16],
                 r["after"].strip(), "PASS" if good else "FAIL"), flush=True)

    s = conn()
    print("server responsive:", cmd(s, "PING").strip(), flush=True)
    print("RESULT:", "PASS" if ok else "FAIL", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
