"""Matrix over the three dimensions that page-fault handling actually has
(docs/08 §4/§6/§7).

  D1  fetch outcome        delayed | errored
  D2  transaction shape    single command | command in BOTH the committed and
                           the dirty object (a multi-command tx that reads,
                           then writes, then reads again)
  D3  concurrent change    none | deleted | overwritten by a NON-paged value |
                           overwritten by a NEW PAGED object | tx aborted

Only combinations that probe distinct machinery are run — the full product is
mostly redundant. Expiry is omitted because it takes the same retire path as
deletion.

EVERY scenario asserts its own PRECONDITION, not just its outcome. Three
earlier tests in this area passed while exercising nothing: the object was
evicted whole instead of shed, or the injector sat after the wake. A scenario
that cannot prove its fault fired is reported FAIL(vacuous), which is the only
way a green run here means anything.

Usage:  python3 tests/unit_cc/paged_fault_matrix.py <server_log_dir>
Requires a Debug server (fault injectors) with paged conversion enabled, a
small --node_memory_limit_mb, and --logbufsecs=0 so the injector log lines the
precondition checks read are not sitting in glog's buffer.
"""
import argparse
import glob
import os
import socket
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(__file__))
import swap_with_inflight_fetch as swap  # noqa: E402
from swap_with_inflight_fetch import (build_paged, cmd, conn, drain_lines,  # noqa: E402
                                      make_pressure)

LOG_DIR = "logs_matrix"
NFIELDS = 900


def fault_count(token):
    """How many times an injector reported firing, across the server's logs."""
    n = 0
    for path in glob.glob(os.path.join(LOG_DIR, "eloqdb.log.INFO*")):
        try:
            with open(path, errors="ignore") as fh:
                n += sum(1 for line in fh if token in line)
        except OSError:
            pass
    return n


def arm(c, name):
    cmd(c, "fault_inject", name, -1)


def disarm(c, name):
    cmd(c, "fault_inject", name, -1, "remove")


def strip_pages(c, key):
    """Strip resident pages so the next access faults.

    Counts shed events globally rather than per key: CcEntry::KeyString()
    reports "no lock, no key" for most entries, so a per-key match reports
    "nothing shed" even when shedding worked. A global delta is coarser but
    honest.

    Returns False if nothing was shed at all, which makes any later "no fault
    happened" result meaningless rather than passing.
    """
    before = fault_count("FAULTLOG shed_all_pages")
    arm(c, "shed_all_pages")
    arm(c, "force_shard_clean_now")
    time.sleep(3)
    disarm(c, "shed_all_pages")
    return fault_count("FAULTLOG shed_all_pages") > before


def run_scenario(name, d1, d2, d3):
    ctl, worker_conn = conn(), conn()
    key = "mx:" + name
    build_paged(worker_conn, key)
    time.sleep(9)                        # checkpoint, so pages become shed-able

    if d2 == "both":
        # First command of the tx touches the COMMITTED object, the later ones
        # create and use the dirty object, so the tx holds contexts in both.
        cmd(worker_conn, "MULTI")
        cmd(worker_conn, "HGET", key, "f00010")
        cmd(worker_conn, "HSET", key, "f00010", "rewritten")
        cmd(worker_conn, "EXEC")
        time.sleep(6)

    shed_ok = strip_pages(ctl, key)

    stalls_before = fault_count("FAULTLOG stall_page_fetch")
    fails_before = fault_count("FAULTLOG fail_page_fetch")
    aborts_before = fault_count("FAULTLOG abort_parked_waiters")

    arm(ctl, "stall_page_fetch")
    if d1 == "errored":
        arm(ctl, "fail_page_fetch")
    if d3 == "abort":
        arm(ctl, "abort_parked_waiters")

    out = {}

    def victim():
        try:
            if d2 == "both":
                cmd(worker_conn, "MULTI")
                cmd(worker_conn, "HGET", key, "f00880")
                cmd(worker_conn, "HSET", key, "f00881", "v")
                out["reply"] = cmd(worker_conn, "EXEC")
            else:
                out["reply"] = cmd(worker_conn, "HGET", key, "f00880")
        except OSError as exc:
            out["reply"] = ("<ERR %s>" % exc).encode()

    t = threading.Thread(target=victim, daemon=True)
    t.start()
    time.sleep(0.8)                      # the fetch is issued and stalled

    if d3 == "delete":
        out["change"] = cmd(ctl, "DEL", key)
    elif d3 == "nonpaged":
        out["change"] = cmd(ctl, "SET", key, "now-a-plain-string")
    elif d3 == "newpaged":
        # A NEW paged incarnation: delete, then rebuild a paged hash under the
        # same key while the old incarnation's fetch is still in flight. This
        # is the case orphaned_ exists for — installing a stale page here
        # would corrupt data silently rather than hang.
        cmd(ctl, "DEL", key)
        for i in range(0, 450, 150):
            pipeline = b""
            for j in range(i, i + 150):
                args = ("HSET", key, "n%05d" % j, "y" * 220)
                pipeline += ("*%d\r\n" % len(args)).encode()
                for a in args:
                    b = str(a).encode()
                    pipeline += b"$%d\r\n%s\r\n" % (len(b), b)
            ctl.sendall(pipeline)
            # A single recv() is not a pipeline drain.  WAL/replication can
            # fragment these 150 integer replies, leaving stale `:1` replies
            # to be mistaken for the later PING/HSET/HGET health checks.
            drain_lines(ctl, 150)
        out["change"] = b"+REBUILT"
    else:
        out["change"] = b"+NONE"

    t.join(timeout=60)
    out["returned"] = not t.is_alive()

    for fault in ("abort_parked_waiters", "fail_page_fetch",
                  "stall_page_fetch"):
        disarm(ctl, fault)
    time.sleep(1.0)

    out["stalled"] = fault_count("FAULTLOG stall_page_fetch") > stalls_before
    out["failed"] = fault_count("FAULTLOG fail_page_fetch") > fails_before
    out["aborted"] = fault_count("FAULTLOG abort_parked_waiters") > aborts_before
    out["shed"] = shed_ok

    # The server must still work, and the key must still be usable.
    out["ping"] = cmd(ctl, "PING")
    out["reuse"] = cmd(ctl, "HSET", key, "post", "ok")
    out["readback"] = cmd(ctl, "HGET", key, "post")
    if out["returned"]:
        worker_conn.close()
    ctl.close()
    return out


SCENARIOS = [
    # name              D1         D2        D3
    ("delay_none",      "delayed", "single", "none"),
    ("delay_del",       "delayed", "single", "delete"),
    ("delay_nonpaged",  "delayed", "single", "nonpaged"),
    ("delay_newpaged",  "delayed", "single", "newpaged"),
    ("err_none",        "errored", "single", "none"),
    ("err_del",         "errored", "single", "delete"),
    ("both_delay",      "delayed", "both",   "none"),
    ("both_newpaged",   "delayed", "both",   "newpaged"),
    ("both_err",        "errored", "both",   "none"),
    ("both_abort",      "delayed", "both",   "abort"),
]


def main():
    global LOG_DIR
    parser = argparse.ArgumentParser()
    parser.add_argument("log_dir", nargs="?", default="logs_matrix")
    parser.add_argument("port", nargs="?", type=int, default=7399)
    args = parser.parse_args()
    LOG_DIR = args.log_dir
    # conn/build_paged/make_pressure are functions imported from this module;
    # their globals remain the module namespace, so set its endpoint once.
    swap.PORT = args.port

    ctl = conn()
    make_pressure(ctl)                   # so the clean pass has work to do
    ctl.close()

    all_ok = True
    for name, d1, d2, d3 in SCENARIOS:
        r = run_scenario(name, d1, d2, d3)

        # Preconditions: did this scenario exercise what it claims?
        vacuous = []
        if not r["shed"]:
            vacuous.append("no-shed")
        if not r["stalled"]:
            vacuous.append("no-stall")
        # The delete disruptor ORPHANS the in-flight fetch at the DEL
        # (docs/08 §7: deletion runs the payload-swap rule), and an orphaned
        # completion is discarded before the fail_page_fetch injector runs —
        # so in the errored+delete row the error marker CANNOT fire and its
        # absence is the designed outcome, not a vacuous run: the reader is
        # woken at the DEL and re-runs to nil instead of ever seeing the
        # store error.
        if d1 == "errored" and d3 != "delete" and not r["failed"]:
            vacuous.append("no-fetch-error")
        if d3 == "abort" and not r["aborted"]:
            vacuous.append("no-abort")

        # A <TIMEOUT> reply means the command never answered: the client
        # thread finished only because its socket timed out. That is a hang,
        # not a pass.
        answered = r["returned"] and b"<TIMEOUT>" not in r["reply"]
        if d3 == "nonpaged":
            # The key is a plain string now, so HSET/HGET on it must be
            # WRONGTYPE. Demanding the hash read back would fail the scenario
            # for behaving correctly.
            reusable = r["readback"].startswith(b"-WRONGTYPE")
        else:
            reusable = r["readback"].startswith(b"$2\r\nok")
        # The REPLY must be right, not merely present. A scenario that
        # answers with the wrong thing is a behaviour bug, and an earlier
        # version of this matrix passed while a deleted key reported a storage
        # error instead of nil.
        if d1 == "errored" and d3 == "delete":
            # DEL lands at 0.8s, well inside the 3s stall, and deletion
            # ORPHANS the in-flight fetch and wakes the parked reader
            # (docs/08 §7) — so the reader re-runs against the deleted key
            # and answers nil. The store error dies with the orphaned
            # completion and must NOT surface: this row once asserted the
            # error reply, which was the pre-orphan behaviour this same
            # comment block calls a bug for the plain delete row.
            expected = r["reply"].startswith(b"$-1")
        elif d1 == "errored":
            # A store error surfaces as an error reply, or — inside MULTI — as
            # the aborted-transaction nil array.
            expected = (r["reply"].startswith(b"-")
                        or r["reply"].startswith(b"*-1"))
        elif d3 in ("delete", "newpaged"):
            expected = (r["reply"].startswith(b"$-1")   # gone -> nil
                        or r["reply"].startswith(b"*"))  # MULTI array form
        elif d3 == "nonpaged":
            expected = r["reply"].startswith(b"-WRONGTYPE")
        elif d3 == "abort":
            expected = r["reply"].startswith(b"*-1") or r["reply"].startswith(b"-")
        else:
            expected = r["reply"].startswith(b"$") or r["reply"].startswith(b"*")
        healthy = (answered and r["ping"].strip() == b"+PONG" and reusable
                   and expected)
        ok = healthy and not vacuous
        all_ok = all_ok and ok
        why = ",".join(vacuous) if vacuous else (
            "hang" if not answered else
            ("dead" if r["ping"].strip() != b"+PONG" else
             ("unusable-key" if not reusable else "wrong-reply")))
        status = "PASS" if ok else "FAIL(%s)" % why
        print("%-15s %-8s %-7s %-9s returned=%-5s reply=%-26r %s"
              % (name, d1, d2, d3, r["returned"], r["reply"][:24], status),
              flush=True)

    print("RESULT:", "PASS" if all_ok else "FAIL", flush=True)
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
