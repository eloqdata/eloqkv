"""Conversion on the standby, for every mutator that can trigger it.

The standby applies commands with `Deserialize` + `CommitOn` and never runs
`ExecuteOn` (#509), which is exactly why the conversion policy lives in
`CommitOn` (docs/08 §11): primary and replica must reach the same verdict from
the same command stream. Centralizing the policy put it on four mutators
rather than one, so all four now run it on the apply path — and a mutator that
converted on the primary but not on the replica would leave the two holding
the same content in different representations, with the replica's copy unable
to page out.

Each mutator grows its own key on the leader; the replica must end up with the
same field count AND the same representation. Representation is probed with
DUMP, which succeeds on a monolithic hash and refuses on a paged one, and the
replica is probed through a read on the replica's own port.

Usage: python3 tests/unit_cc/standby_conversion_paths.py [leader] [replica]
"""
import sys
import time

sys.path.insert(0, "tests/unit_cc")
from standby_paged_apply import conn, read_reply, send  # noqa: E402


def cmd(sock, *args):
    send(sock, *args)
    return read_reply(sock)


def is_paged(sock, key):
    """@return True if DUMP refuses, which only a paged object does."""
    return cmd(sock, b"DUMP", key)[:1] == b"-"


def converge(replica, key, want, timeout=60):
    """@return the replica's HLEN once it reaches `want`, else the last seen."""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        r = cmd(replica, b"HLEN", key)
        last = int(r[1:]) if r[:1] == b":" else None
        if last == want:
            return last
        time.sleep(0.5)
    return last


def main():
    lp = int(sys.argv[1]) if len(sys.argv) > 1 else 7401
    rp = int(sys.argv[2]) if len(sys.argv) > 2 else 7501
    leader, replica = conn(lp), conn(rp)
    bad = []

    # Read-only probes must go to the replica; a write there is refused.
    cases = [
        (b"sbc:hset", lambda i: (b"HSET", b"sbc:hset", b"f%03d" % i,
                                 b"v" * 64)),
        (b"sbc:hsetnx", lambda i: (b"HSETNX", b"sbc:hsetnx", b"f%03d" % i,
                                   b"v" * 64)),
        (b"sbc:hincrby", lambda i: (b"HINCRBY", b"sbc:hincrby",
                                    b"f%03d" % i, b"5")),
        (b"sbc:hincrbyfloat", lambda i: (b"HINCRBYFLOAT",
                                         b"sbc:hincrbyfloat",
                                         b"f%03d" % i, b"1.5")),
    ]
    n = 60
    for key, mk in cases:
        cmd(leader, b"DEL", key)
        for i in range(n):
            r = cmd(leader, *mk(i))
            if r[:1] == b"-":
                bad.append((key.decode() + " write", r[:50], b"accepted"))
                break
        lp_paged = is_paged(leader, key)
        got = converge(replica, key, n)
        rp_paged = is_paged(replica, key)
        print("  %-18s leader paged=%s | replica HLEN=%s paged=%s"
              % (key.decode(), lp_paged, got, rp_paged), flush=True)
        if not lp_paged:
            bad.append((key.decode() + ": leader not paged", b"", b"paged"))
        if got != n:
            bad.append((key.decode() + ": replica HLEN", str(got).encode(),
                        str(n).encode()))
        if rp_paged != lp_paged:
            bad.append((key.decode() + ": representation diverged",
                        str(rp_paged).encode(), str(lp_paged).encode()))

    # A TTL set before the converting write must survive on BOTH sides.
    key = b"sbc:ttl"
    cmd(leader, b"DEL", key)
    cmd(leader, b"HSET", key, b"a", b"1")
    cmd(leader, b"EXPIRE", key, b"10000")
    for i in range(n):
        cmd(leader, b"HINCRBY", key, b"f%03d" % i, b"1")
    l_ttl = cmd(leader, b"TTL", key)
    converge(replica, key, n + 1)
    r_ttl = cmd(replica, b"TTL", key)
    print("  %-18s leader TTL=%r replica TTL=%r"
          % ("sbc:ttl", l_ttl, r_ttl), flush=True)
    for label, v in (("leader", l_ttl), ("replica", r_ttl)):
        if v in (b":-1", b":-2"):
            bad.append(("TTL lost on " + label, v, b"positive"))

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
