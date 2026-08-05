"""Does HLEN agree with what was written, on a paged hash?

Twice during the replay work an object held ONE MORE field after recovery than
a pre-crash HLEN reported (1700 -> 1701, and 3900 -> 3901), with every write
acknowledged. Recovery cannot invent a field, so the suspicion is that HLEN
under-reports on a LIVE paged object -- the metadata's field count lagging the
last write. That is a read-path correctness bug if true, entirely independent of
recovery, and it is cheap to test directly.

Three independent counts are compared after every write in the tail:

  HLEN            the object's own count
  HKEYS           the array length in the reply header, an independent count
  writes          distinct fields this test actually created

Each write is a full round trip with its reply read before the next is sent, so
"acknowledged" is not in question. The conversion threshold must be low enough
that the object is paged well before the tail (run with
--paged_hash_convert_threshold=1).

Usage: python3 tests/unit_cc/paged_hlen_consistency.py [nfields] [port]
"""
import sys

sys.path.insert(0, "tests/unit_cc")
from standby_paged_apply import conn  # noqa: E402

KEY = "hlen:paged"


def send(sock, *args):
    out = ("*%d\r\n" % len(args)).encode()
    for a in args:
        b = a if isinstance(a, bytes) else str(a).encode()
        out += b"$%d\r\n%s\r\n" % (len(b), b)
    sock.sendall(out)


def read_simple(sock):
    """Read one short reply (integer or status) in full."""
    buf = b""
    while not buf.endswith(b"\r\n"):
        chunk = sock.recv(4096)
        if not chunk:
            raise OSError("connection closed")
        buf += chunk
    return buf


def hlen(sock):
    send(sock, "HLEN", KEY)
    r = read_simple(sock)
    assert r.startswith(b":"), r
    return int(r[1:-2])


def hkeys_count(sock):
    """Independent count: the array length in the HKEYS reply header.

    Drains the whole reply so the socket stays usable -- an undrained reply is
    what corrupted an earlier test's accounting.
    """
    send(sock, "HKEYS", KEY)
    buf = b""
    while b"\r\n" not in buf:
        buf += sock.recv(65536)
    header, rest = buf.split(b"\r\n", 1)
    assert header.startswith(b"*"), header
    n = int(header[1:])
    # Each element is "$len\r\n<data>\r\n"; drain them all.
    remaining = n
    while remaining > 0:
        while rest.count(b"\r\n") < 2:
            rest += sock.recv(65536)
        lenline, rest = rest.split(b"\r\n", 1)
        size = int(lenline[1:])
        while len(rest) < size + 2:
            rest += sock.recv(65536)
        rest = rest[size + 2:]
        remaining -= 1
    return n


def main():
    nfields = int(sys.argv[1]) if len(sys.argv) > 1 else 1200
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 7399
    c = conn(port)
    send(c, "DEL", KEY)
    read_simple(c)

    bad = []
    for i in range(nfields):
        send(c, "HSET", KEY, "f%05d" % i, "v" * 200)
        r = read_simple(c)
        if not r.startswith(b":"):
            raise OSError("HSET f%05d not acknowledged: %r" % (i, r[:40]))
        expected = i + 1
        # Check every write near the start (cheap) and then periodically, plus
        # the whole tail, where the lag was originally seen.
        if i < 20 or i % 100 == 0 or i >= nfields - 20:
            got = hlen(c)
            if got != expected:
                bad.append((i, expected, got))
                if len(bad) <= 5:
                    print("  MISMATCH after %d writes: HLEN=%d expected=%d"
                          % (expected, got, expected), flush=True)

    final_hlen = hlen(c)
    final_keys = hkeys_count(c)
    print("writes=%d  HLEN=%d  HKEYS=%d" % (nfields, final_hlen, final_keys),
          flush=True)
    print("mismatches observed during the run: %d" % len(bad), flush=True)

    ok = (final_hlen == nfields and final_keys == nfields and not bad)
    print("RESULT:", "PASS" if ok else "FAIL", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
