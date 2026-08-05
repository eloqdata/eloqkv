"""HSCAN over a paged hash, through the real cursor (docs/08 §12).

The monolithic hash returns everything in one reply with cursor 0; the paged
one honours the cursor, so this is the first command whose *protocol-visible*
behaviour differs by representation. Three things are asserted:

  completeness  a full cursor loop with a small COUNT returns every field
                exactly-at-least once (duplicates tolerated, skips fatal), and
                the deduplicated result matches HKEYS
  progress      the loop terminates well within a bounded number of steps --
                COUNT bounds work, so a loop that never advances or that
                ignores COUNT both fail this
  match         the same loop with MATCH returns exactly the matching subset,
                and does NOT degenerate into one giant reply (the work bound
                must hold when the pattern filters everything out)

Also scans WHILE WRITING: a writer keeps adding fields during the loop, and
every field present before the scan started must still be returned.

Usage: python3 tests/unit_cc/paged_hscan.py [port] [nfields]
"""
import sys

sys.path.insert(0, "tests/unit_cc")
from standby_paged_apply import conn, read_reply, send  # noqa: E402

KEY = "hscan:paged"


def read_array(sock):
    """Read one reply, returning (header, [bulk strings])."""
    buf = b""

    def more():
        nonlocal buf
        chunk = sock.recv(65536)
        if not chunk:
            raise OSError("connection closed")
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
        s, buf = buf[:n], buf[n + 2:]
        return s

    head = line()
    if not head.startswith(b"*"):
        return head, []
    out = []
    for _ in range(int(head[1:])):
        el = line()
        if el.startswith(b"$"):
            n = int(el[1:])
            out.append(bulk(n) if n >= 0 else None)
        elif el.startswith(b"*"):
            # HSCAN replies are [cursor, [elements]] on RESP2 as a nested
            # array; flatten the nesting.
            for _ in range(int(el[1:])):
                el2 = line()
                assert el2.startswith(b"$"), el2
                n2 = int(el2[1:])
                out.append(bulk(n2) if n2 >= 0 else None)
        else:
            out.append(el)
    return head, out


def hscan_loop(sock, count, pattern=None, novalues=True, writer=None,
               max_steps=100000):
    """Drive the cursor to completion.

    @return (set of fields seen, number of steps taken).
    """
    seen = set()
    cursor = b"0"
    steps = 0
    while True:
        args = ["HSCAN", KEY, cursor, "COUNT", str(count)]
        if pattern is not None:
            args += ["MATCH", pattern]
        if novalues:
            args += ["NOVALUES"]
        send(sock, *args)
        head, flat = read_array(sock)
        assert head.startswith(b"*"), (head, flat[:4])
        cursor = flat[0]
        for f in flat[1:]:
            seen.add(f)
        steps += 1
        if writer is not None:
            writer(steps)
        if cursor == b"0":
            return seen, steps
        if steps >= max_steps:
            raise AssertionError("cursor never completed after %d steps"
                                 % steps)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 7399
    nfields = int(sys.argv[2]) if len(sys.argv) > 2 else 2000

    c = conn(port)
    send(c, "DEL", KEY)
    read_reply(c)
    for i in range(nfields):
        send(c, "HSET", KEY, "f%05d" % i, "v%05d" % i)
        r = read_reply(c)
        if r[:1] != b":":
            raise OSError("HSET not acknowledged: %r" % r[:40])
    expected = {b"f%05d" % i for i in range(nfields)}

    # -- completeness + progress ------------------------------------------
    seen, steps = hscan_loop(c, count=10)
    missing = expected - seen
    extra = seen - expected
    # COUNT bounds work from below, so the step count is bounded above by
    # roughly nfields/COUNT plus slack for run-to-end overshoot.
    bound = nfields // 10 * 3 + 50
    print("full scan: %d fields in %d steps (bound %d), missing=%d extra=%d"
          % (len(seen), steps, bound, len(missing), len(extra)), flush=True)
    ok = not missing and not extra and 1 < steps <= bound

    # -- match subset ------------------------------------------------------
    want = {f for f in expected if f.startswith(b"f0001")}  # f00010..f00019
    got, msteps = hscan_loop(c, count=10, pattern="f0001?")
    print("match scan: %d of %d expected in %d steps"
          % (len(got & want), len(want), msteps), flush=True)
    # The work bound must hold even though almost nothing matches: the step
    # count stays around nfields/COUNT rather than collapsing to 1.
    ok = ok and got == want and 1 < msteps <= bound

    # -- scan while writing ------------------------------------------------
    w = conn(port)

    def writer(step):
        send(w, "HSET", KEY, "w%05d" % step, "x")
        if read_reply(w)[:1] != b":":
            raise OSError("concurrent HSET failed")

    seen2, steps2 = hscan_loop(c, count=10, writer=writer)
    missing2 = expected - seen2
    print("scan under writes: %d steps, %d pre-existing fields missing"
          % (steps2, len(missing2)), flush=True)
    ok = ok and not missing2

    print("RESULT:", "PASS" if ok else "FAIL", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
