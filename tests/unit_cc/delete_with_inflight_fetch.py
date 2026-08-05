"""DEL while a page fetch is IN FLIGHT, then an immediate recreate (§7).

The §7 hazard, as sharpened in review: deletion is a payload swap in every
sense that matters, but the in-place DEL commit never ran ApplyPayloadSwapRule
— so the deleted object's in-flight fetches stayed live in the entry's
FetchHub. A NEW incarnation restarts page ids at 0, so once the key is
recreated, an old fetch for page N can find page N LIVE in the successor and
install the previous incarnation's bytes into it. The stale install is only
masked while the entry still holds the retained DELETED block; the recreate
removes that mask.

Sequence:

  1. paged hash, durable, then `shed_all_pages` (metadata resident, pages
     shed — a page READ is now a page fetch);
  2. arm `stall_page_fetch` (~3 s) and start a reader: it parks with a
     fetch genuinely in flight;
  3. DEL the key — the fix orphans the fetch and wakes the reader HERE;
  4. immediately recreate the key with DIFFERENT values (page ids restart
     at 0: the collision the orphan flag exists for);
  5. after the stall window, assert the reader completed (old value or nil,
     never a hang) and that EVERY successor field reads back the NEW value —
     a stale install would surface as old bytes or a corrupt page.

Usage: python3 tests/unit_cc/delete_with_inflight_fetch.py <port> <log_dir>
"""
import sys
import threading
import time

sys.path.insert(0, "tests/unit_cc")
from paged_testlib import (RespClient, arm_fault, disarm_fault,  # noqa: E402
                           shed_all_pages)

KEY = b"paged:del-inflight"
OLD_FIELDS = 120
NEW_FIELDS = 90


def main():
    port = int(sys.argv[1])
    log_dir = sys.argv[2]
    bad = []

    c = RespClient(port=port)
    c.command(b"DEL", KEY)
    for i in range(OLD_FIELDS):
        c.command(b"HSET", KEY, b"f%04d" % i, b"OLD" * 60)
    time.sleep(8)  # durable, so the pages can be shed
    shed_all_pages(c, log_dir, timeout=30)

    arm_fault(c, "stall_page_fetch")
    reply = {}

    def reader():
        try:
            reply["r"] = RespClient(port=port).command(b"HGET", KEY, b"f0003")
        except Exception as exc:  # noqa: BLE001 - recorded, not raised
            reply["r"] = "ERR:%s" % exc

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    time.sleep(0.6)  # let the reader park with its fetch in flight

    # The disruptor pair: DEL while the fetch is stalled, then recreate
    # IMMEDIATELY so the successor's page 0 is live before the stall ends.
    c.command(b"DEL", KEY)
    for i in range(NEW_FIELDS):
        c.command(b"HSET", KEY, b"g%04d" % i, b"NEW" * 60)
    print("  DEL + %d-field recreate done while the fetch was in flight"
          % NEW_FIELDS, flush=True)

    t.join(timeout=60)
    disarm_fault(c, "stall_page_fetch")
    r = reply.get("r", "<hung>")
    # Old value (read serialized before the DEL) and nil (after) are both
    # legal; a hang or an error is not.
    ok_reader = r is None or (isinstance(r, bytes) and r.startswith(b"OLD"))
    print("  reader reply: %r -> %s" % (
        r if not isinstance(r, bytes) else r[:9],
        "ok" if ok_reader else "BAD"), flush=True)
    if not ok_reader:
        bad.append(("parked reader outcome", str(r).encode()[:40],
                    b"OLD value or nil, never a hang"))

    # The heart of the assertion: the successor must be EXACTLY the new
    # object. A stale install into a colliding page id shows up here as OLD
    # bytes, a missing field, or a wrong HLEN.
    n = c.command(b"HLEN", KEY)
    if n != NEW_FIELDS:
        bad.append(("successor HLEN", str(n).encode(),
                    str(NEW_FIELDS).encode()))
    wrong = 0
    for i in range(NEW_FIELDS):
        v = c.command(b"HGET", KEY, b"g%04d" % i)
        if v != b"NEW" * 60:
            wrong += 1
    if wrong:
        bad.append(("successor fields with stale/corrupt bytes",
                    str(wrong).encode(), b"0"))
    for i in range(0, OLD_FIELDS, 17):
        if c.command(b"HGET", KEY, b"f%04d" % i) is not None:
            bad.append(("old-incarnation field resurfaced",
                        b"f%04d" % i, b"nil"))
            break
    print("  successor: HLEN=%r, %d/%d fields exact, old fields absent"
          % (n, NEW_FIELDS - wrong, NEW_FIELDS), flush=True)
    if c.command(b"PING") != b"PONG":
        bad.append(("server unresponsive", b"", b"PONG"))

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
