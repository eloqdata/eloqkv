"""A command is parked on a page fetch, and its transaction is ABORTED
(docs/08 §6/§7).

Real aborts of a parked command come from paths that know nothing about page
fetches — a deadlock victim, tx recovery, a term change. They call
AbortCcRequest, which Free()s the request back to its pool. If the entry's
FetchHub still holds that pointer as a parked waiter, the fetch completes,
resolves the waiter and enqueues a recycled request: the use-after-free class
that produced the original crash in this work.

Reaching the state needs the Debug-only injectors:

  shed_all_pages       - leaves the object metadata-resident with no pages, so
                         the next access is guaranteed to page-fault.
  force_shard_clean_now- runs the LRU clean pass immediately; without it the
                         pass only runs under genuine memory pressure.
  stall_page_fetch     - holds the completion in the shard queue ~3s.
  abort_parked_waiters - aborts whatever is parked on the fetch, without
                         deregistering it from the hub.

The assertion is not about the aborted client's reply, which may legitimately
be an error: it is that the SERVER survives the completion that follows, and
that the key stays usable afterwards.
"""
import socket
import sys
import threading
import time

sys.path.insert(0, "tests/unit_cc")
from swap_with_inflight_fetch import (build_paged, cmd, conn,  # noqa: E402
                                      make_pressure)


def run(write_command):
    """Park `write_command` on a page fault, then abort it mid-flight."""
    a, b = conn(), conn()
    key = write_command[1]
    # Pressure first, target second: a target built before the fillers is the
    # coldest entry and gets evicted WHOLE, which turns the later access into a
    # record fetch instead of a page fault.
    make_pressure(b)
    build_paged(a, key)
    time.sleep(9)                       # let the checkpoint clean the pages

    cmd(b, "fault_inject", "shed_all_pages", -1)
    cmd(b, "fault_inject", "force_shard_clean_now", -1)
    time.sleep(3)
    cmd(b, "fault_inject", "shed_all_pages", -1, "remove")

    cmd(b, "fault_inject", "stall_page_fetch", -1)
    cmd(b, "fault_inject", "abort_parked_waiters", -1)

    out = {}

    def worker():
        try:
            out["reply"] = cmd(a, *write_command)
        except OSError as exc:
            out["reply"] = ("<ERR %s>" % exc).encode()

    t = threading.Thread(target=worker, daemon=True)
    t.start()
    t.join(timeout=45)
    out["returned"] = not t.is_alive()

    cmd(b, "fault_inject", "abort_parked_waiters", -1, "remove")
    cmd(b, "fault_inject", "stall_page_fetch", -1, "remove")
    time.sleep(1.0)

    # The server must still be answering, and the key still usable.
    out["ping"] = cmd(b, "PING")
    out["reuse"] = cmd(b, "HSET", key, "after", "ok")
    out["read_back"] = cmd(b, "HGET", key, "after")
    if out["returned"]:
        a.close()
    b.close()
    return out


def main():
    ok = True
    for label, command in [
        ("read", ("HGET", "abrt:hget", "f00880")),
        ("write", ("HSET", "abrt:hset", "f00880", "v")),
    ]:
        r = run(command)
        good = (r["returned"]
                and r["ping"].strip() == b"+PONG"
                and r["reuse"][:1] in (b":", b"$")
                and r["read_back"].startswith(b"$2\r\nok"))
        ok = ok and good
        print("%-6s returned=%s reply=%r ping=%r reuse=%r read_back=%r -> %s"
              % (label, r["returned"], r["reply"][:28], r["ping"].strip(),
                 r["reuse"][:8], r["read_back"][:12],
                 "PASS" if good else "FAIL"), flush=True)

    s = conn()
    print("server responsive:", cmd(s, "PING").strip(), flush=True)
    print("RESULT:", "PASS" if ok else "FAIL", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
