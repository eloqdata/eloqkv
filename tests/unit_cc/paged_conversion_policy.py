"""Conversion is a property of the OBJECT, not of the command that grew it.

docs/08-paged-objects.md §11 states that a hash converts when its post-image
crosses the threshold. That is an invariant about the object, so every path
that can grow a hash must apply it: `HSET`, `HSETNX`, `HINCRBY`,
`HINCRBYFLOAT`. Tied to one command it produced hashes of identical size in
different representations depending on how they were built, and left
everything grown by the other mutators permanently monolithic — the exact
scaling problem paging exists to remove.

Representation is probed with HSCAN: a paged hash honours COUNT (a small
COUNT on a large object returns a nonzero cursor), while a monolithic hash
returns the entire object in one reply with cursor 0.

The RESTORE import arms and TTL-survival-across-conversion live in the C++
unit test (`TestInlineRecordCap`, `TestConversionPolicy`): with the §4/§14
inline-record cap enforced at the command, an oversized record can no longer
be written to KEEP a hash monolithic under threshold=1, so the DUMP-based
escape hatch this test once used does not exist any more — by design.

Run against a server with --paged_hash_convert_threshold=1. Usage:
    python3 tests/unit_cc/paged_conversion_policy.py [port] [page_size]
"""
import sys

sys.path.insert(0, "tests/unit_cc")
from standby_paged_apply import conn, read_reply, send  # noqa: E402
from paged_dark_feature import hscan_first  # noqa: E402


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 7399
    s = conn(port)
    bad = []

    def cmd(*args):
        send(s, *args)
        return read_reply(s)

    cmd(b"FLUSHALL")

    # --- every growth path converts -------------------------------------
    n = 60
    grow = [
        ("g_hset", lambda k, i: (b"HSET", k, b"f%03d" % i, b"v" * 64)),
        ("g_hsetnx", lambda k, i: (b"HSETNX", k, b"f%03d" % i, b"v" * 64)),
        ("g_hincrby", lambda k, i: (b"HINCRBY", k, b"f%03d" % i, b"5")),
        ("g_hincrbyfloat",
         lambda k, i: (b"HINCRBYFLOAT", k, b"f%03d" % i, b"1.5")),
    ]
    for name, mk in grow:
        key = name.encode()
        for i in range(n):
            r = cmd(*mk(key, i))
            if r[:1] == b"-":
                bad.append((name + " write refused", r[:40], b"accepted"))
                break
        cursor, fields = hscan_first(s, key.decode(), 10)
        paged = cursor != b"0"
        print("  %-16s -> HSCAN COUNT 10 cursor=%r (paged=%s)"
              % (name, cursor, paged), flush=True)
        if not paged:
            bad.append((name + " did not convert", b"", b"paged"))

    # --- HSETNX on an EXISTING field: reply 0, value untouched ----------
    # (A reported bug: the paged path correctly skipped the write but
    # replied 1; the stock TCL tests assert only the value, so the wire
    # reply needs its own assertion.)
    r = cmd(b"HSETNX", b"g_hsetnx", b"f000", b"loser")
    if r != b":0":
        bad.append(("HSETNX existing reply", r[:20], b":0"))
    send(s, b"HGET", b"g_hsetnx", b"f000")
    r = read_reply(s)
    if not r.startswith(b"$64"):
        bad.append(("HSETNX existing value", r[:20], b"$64 (unchanged)"))
    print("  HSETNX existing  -> reply 0, value unchanged: %s"
          % (len(bad) == 0), flush=True)

    # --- other types are untouched by the hash policy -------------------
    for c, want in [([b"SET", b"t_str", b"v"], b"+"),
                    ([b"RPUSH", b"t_list", b"a"], b":"),
                    ([b"SADD", b"t_set", b"a"], b":"),
                    ([b"ZADD", b"t_zset", b"1", b"a"], b":")]:
        r = cmd(*c)
        if r[:1] != want:
            bad.append((c[0].decode(), r[:40], want))
    if cmd(b"PING")[:5] != b"+PONG":
        bad.append(("server dead", b"", b"+PONG"))

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
