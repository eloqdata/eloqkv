"""The inline-record cap: an oversized field/value ERRORS the command.

The cap is `max(page_size / 8, 4 KB)`, bounded by page capacity (docs/08 §4,
§14). Records above it belong in the deferred out-of-line large-value runs;
until those land, admitting them either wedged the hash out of paging
forever (the old silent monolithic decline) or crashed `Put` outright — so
with conversion ENABLED the write is refused with a specific error, on both
representations, at Execute time. Pre-WAL matters: replay and standby run
CommitOn with no Execute and can reject nothing, so only storable commands
may reach the log. Dark servers (threshold 0) keep stock Redis behavior —
covered by the unit arms and the dark-restart rehearsal, not here.

At this harness's 4 KB test pages the floor makes the cap equal page
capacity — one record per page is deliberately allowed in testing — so
"oversized" here means larger than a page.

Run against a Debug server with --paged_hash_convert_threshold=1 and a small
--paged_hash_page_size. Usage:
    python3 tests/unit_cc/paged_oversized_record.py [port] [page_size]
"""
import sys

sys.path.insert(0, "tests/unit_cc")
from standby_paged_apply import conn, read_reply, send  # noqa: E402


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 7399
    page_size = int(sys.argv[2]) if len(sys.argv) > 2 else 4096
    cap = max(page_size // 8, 4096)
    big = b"x" * (max(cap, page_size) * 5)  # far beyond the cap and the page
    ok_val = b"x" * min(cap // 2, page_size // 2)  # comfortably under
    s = conn(port)
    bad = []

    def cmd(*args):
        send(s, *args)
        return read_reply(s)

    def expect(label, reply, pred, want):
        ok = pred(reply)
        if not ok:
            bad.append((label, reply[:60], want))
        print("  %-44s -> %r" % (label, reply[:46]), flush=True)
        return ok

    cmd(b"FLUSHALL")

    # --- an oversized record is REFUSED, and the key is not created ---
    expect("HSET oversized value", cmd(b"HSET", b"h", b"f", big),
           lambda r: r[:1] == b"-" and b"too large" in r, "specific error")
    expect("  key was not created", cmd(b"EXISTS", b"h"),
           lambda r: r == b":0", ":0")
    expect("HSET oversized field name", cmd(b"HSET", b"h", big, b"v"),
           lambda r: r[:1] == b"-" and b"too large" in r, "specific error")
    expect("HSETNX oversized value", cmd(b"HSETNX", b"h", b"f", big),
           lambda r: r[:1] == b"-" and b"too large" in r, "specific error")
    expect("HINCRBY oversized field name", cmd(b"HINCRBY", b"h", big, b"1"),
           lambda r: r[:1] == b"-" and b"too large" in r, "specific error")
    expect("HINCRBYFLOAT oversized field name",
           cmd(b"HINCRBYFLOAT", b"h", big, b"1.5"),
           lambda r: r[:1] == b"-" and b"too large" in r, "specific error")
    expect("  server alive", cmd(b"PING"),
           lambda r: r[:5] == b"+PONG", "+PONG")

    # --- an under-cap record is accepted and the hash pages normally ---
    expect("HSET under-cap value", cmd(b"HSET", b"h", b"f", ok_val),
           lambda r: r[:1] == b":", "accepted")
    r = cmd(b"DUMP", b"h")
    print("  paged (DUMP refuses): %s" % (r[:1] == b"-"), flush=True)
    if r[:1] != b"-":
        bad.append(("hash did not page", r[:40], b"paged"))

    # --- the refusal holds on the ALREADY-PAGED object, which stays intact --
    expect("HSET oversized into paged", cmd(b"HSET", b"h", b"big", big),
           lambda r: r[:1] == b"-" and b"too large" in r, "specific error")
    expect("HSETNX oversized into paged",
           cmd(b"HSETNX", b"h", b"big2", big),
           lambda r: r[:1] == b"-" and b"too large" in r, "specific error")
    expect("HINCRBY oversized field into paged",
           cmd(b"HINCRBY", b"h", big, b"1"),
           lambda r: r[:1] == b"-" and b"too large" in r, "specific error")
    expect("  paged object intact", cmd(b"HLEN", b"h"),
           lambda r: r == b":1", ":1")
    expect("  value reads back whole", cmd(b"HGET", b"h", b"f"),
           lambda r: r == b"$%d" % len(ok_val), "full length")
    expect("  normal write still accepted",
           cmd(b"HSET", b"h", b"b", b"2"),
           lambda r: r[:1] == b":", "accepted")

    # --- growth far past the threshold stays healthy ---
    for i in range(200):
        if cmd(b"HSET", b"h", b"s%03d" % i, b"v" * 50)[:1] != b":":
            bad.append(("filler HSET rejected", b"", b""))
            break
    expect("after +200 fields: HLEN", cmd(b"HLEN", b"h"),
           lambda r: r == b":202", ":202")
    expect("  server alive", cmd(b"PING"),
           lambda r: r[:5] == b"+PONG", "+PONG")

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
