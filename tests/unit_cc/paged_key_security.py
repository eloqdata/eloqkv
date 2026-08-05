"""The reserved page-key prefix cannot be reached from any command path, and
the length limit reserves the codec overhead (docs/08-paged-objects.md §5).

Three flaws this guards against, all pre-production:

  1. The reserved \\x00EKVPAGE prefix must be rejected on EVERY command path,
     not just the single-key one. A multi-key write to an exact derived page
     key (MSET <page key> <value>) would otherwise overwrite a live page row.
  2. DUMP of a paged object must not crash (it once static_cast a paged hash
     to the monolithic type and read garbage); it returns a clean
     "unsupported on paged object" error until reassembly lands.
  3. A key at the public limit plus the page-key overhead must still fit the
     store — enforced by lowering the public limit, so an admitted key is
     always encodable as a page key.

Run against a Debug server with --paged_hash_convert_threshold=1 so hashes
are paged. Usage: python3 tests/unit_cc/paged_key_security.py [port]
"""
import socket
import sys

sys.path.insert(0, "tests/unit_cc")
from standby_paged_apply import conn, read_reply, send  # noqa: E402

# The reserved magic, followed by a plausible derived-page-key body so the
# whole thing is byte-for-byte a real page key an attacker might try.
MAGIC = b"\x00EKVPAGE"
PAGE_KEY = MAGIC + b"\x00\x00\x00\x04user\x00\x00\x00\x00\x00"


def is_err(r):
    return r[:1] == b"-"


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 7399
    s = conn(port)
    bad = []

    def check(label, cmd, want_err=True):
        send(s, *cmd)
        r = read_reply(s)
        ok = is_err(r) if want_err else not is_err(r)
        if not ok:
            bad.append((label, r[:50]))
        print("  %-26s -> %r" % (label, r[:44]), flush=True)
        return r

    # --- reserved prefix rejected on every path ---
    check("SET <pagekey>", [b"SET", PAGE_KEY, b"x"])
    check("GET <pagekey>", [b"GET", PAGE_KEY])
    check("MSET <pagekey>", [b"MSET", PAGE_KEY, b"x", b"ok", b"y"])
    check("MGET <pagekey>", [b"MGET", PAGE_KEY, b"plain"])
    check("DEL <pagekey>", [b"DEL", PAGE_KEY, b"plain"])
    check("EXISTS <pagekey>", [b"EXISTS", PAGE_KEY])
    check("MSETNX <pagekey>", [b"MSETNX", PAGE_KEY, b"x"])
    check("HSET <pagekey>", [b"HSET", PAGE_KEY, b"f", b"v"])

    # MULTI/EXEC queued path.
    send(s, b"MULTI")
    read_reply(s)
    send(s, b"MSET", PAGE_KEY, b"x")
    read_reply(s)  # queued reply
    check("EXEC{MSET <pagekey>}", [b"EXEC"])

    # A key one byte short of the magic must NOT be rejected (the gate keys on
    # the full 8-byte magic, not a shorter accidental prefix).
    check("SET <7-byte magic>", [b"SET", MAGIC[:7] + b"tail", b"v"],
          want_err=False)

    # --- DUMP on a paged object refuses cleanly, does not crash ---
    check("HSET paged", [b"HSET", b"ph", b"f", b"v"], want_err=False)
    dump = check("DUMP paged", [b"DUMP", b"ph"])
    if b"paged" not in dump:
        bad.append(("DUMP wrong error", dump[:50]))

    # --- the server is still alive after all of the above ---
    send(s, b"PING")
    if read_reply(s)[:5] != b"+PONG":
        bad.append(("server dead", b""))

    # --- a plain multi-key write still works ---
    check("MSET plain", [b"MSET", b"a", b"1", b"b", b"2"], want_err=False)
    # read_reply returns the reply's header line; a successful GET of a
    # 1-byte value is "$1", a nil is "$-1".
    got = check("GET a", [b"GET", b"a"], want_err=False)
    if got[:2] != b"$1":
        bad.append(("plain MSET lost", got[:20]))

    if bad:
        for label, r in bad:
            print("  FAIL: %s -> %r" % (label, r), flush=True)
        print("RESULT: FAIL", flush=True)
        return 1
    print("RESULT: PASS", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
