"""Acknowledged DEL -> crash BEFORE its checkpoint -> WAL replay -> raw rows.

The case the durable-row oracle structurally cannot see: the oracle always
lets the LIVE node checkpoint the deletion, so it exercises only the live
commit path. A crash-replayed deletion goes through the buffered-command
drain, and on a fresh restart the prior object is NOT resident: the DEL is
an overwrite command, so replay applies it without fetching the old paged
metadata, and there is no page-id inventory in memory to fan out.

What this test asserts is the DESIGN's contract for that case, not full
fan-out (docs/08 §9): correctness first — the key is gone from Redis, the
METADATA row is gone from the store, and the key is cleanly recreatable —
while the page rows left behind are ACCEPTED sweeper debt, the same
category as replacement (SET/RESTORE over a paged key). They are reported
for visibility but are not a failure: reclaiming them is the §14 sweeper's
job, by design decision, because forcing a record fetch for every replayed
deletion just to fan out was judged not worth the mechanism. When the
prior IS resident at replay (e.g. standby apply of a hot key), the central
CommitOn paged-deletion rule retains the block and the fan-out happens.

Sequence:

  1. create a paged hash, wait for its checkpoint, verify metadata + page
     rows exist in raw EloqStore;
  2. DEL (reply 1), then SIGKILL the node immediately — before the deletion
     checkpoint;
  3. restart against the same WAL and EloqStore, wait for replay plus two
     checkpoint intervals;
  4. Redis says the key is gone; the metadata row is absent; recreate works.
     Leftover page rows are printed as sweeper debt.

Needs a WAL-enabled single-node harness (paged_single_node.sh up --wal on);
reads START_SCRIPT/PORT/LOG_DIR from bld-paged-matrix/single/current.env.

Usage:
    python3 tests/unit_cc/paged_replay_delete_rows.py \
        --port P --dss-port D --start-script S [--checkpoint-wait 12]
"""
from __future__ import annotations

import argparse
import os
import signal
import subprocess
import time

from eloqstore_paged_rows import active_table, exact_row, partition_of
from paged_store_rows_oracle import (hset_image, image, inspect_key,
                                     raw_page_rows)
from paged_testlib import RespClient, require_equal

KEY = b"paged:replay-delete-rows"


def eloqkv_pids():
    pids = []
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            exe = os.readlink("/proc/%s/exe" % entry)
        except OSError:
            continue
        if exe.endswith("/eloqkv"):
            pids.append(int(entry))
    return pids


def wait_ping(port, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if RespClient(port=port).command(b"PING") == b"PONG":
                return True
        except OSError:
            pass
        time.sleep(1.0)
    return False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--dss-port", type=int, required=True)
    parser.add_argument("--start-script", required=True)
    parser.add_argument("--shard", type=int, default=0)
    parser.add_argument("--page-size", type=int, default=4096)
    parser.add_argument("--fields", type=int, default=180)
    parser.add_argument("--checkpoint-wait", type=float, default=12.0)
    args = parser.parse_args()

    client = RespClient(port=args.port)
    client.command(b"DEL", KEY)
    require_equal(hset_image(client, KEY, image(args.fields, 0, 160)),
                  args.fields, "replay-delete baseline")
    time.sleep(args.checkpoint_wait)

    table = active_table(args.host, args.dss_port, 0, args.shard)
    baseline = inspect_key(args, table, KEY)
    if baseline["metadata"] is None or baseline["page_count"] == 0:
        print("RESULT: FAIL (baseline never became durable: %r)" % baseline,
              flush=True)
        return 1
    print("  baseline: metadata=True pages=%d (table=%s)"
          % (baseline["page_count"], table), flush=True)

    require_equal(client.command(b"DEL", KEY), 1, "acknowledged DEL")
    # SIGKILL immediately: the deletion is in the WAL, not the checkpoint.
    pids = eloqkv_pids()
    for pid in pids:
        os.kill(pid, signal.SIGKILL)
    print("  DEL acknowledged; SIGKILL %s before its checkpoint" % pids,
          flush=True)
    time.sleep(3)

    subprocess.Popen(["setsid", "bash", args.start_script],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                     start_new_session=True)
    if not wait_ping(args.port, 300):
        print("RESULT: FAIL (node did not come back)", flush=True)
        return 1
    print("  restart-up; waiting replay + 2 checkpoint intervals", flush=True)
    time.sleep(2 * args.checkpoint_wait)

    bad = []
    client = RespClient(port=args.port)
    exists = client.command(b"EXISTS", KEY)
    if exists != 0:
        bad.append("EXISTS after replay = %r, want 0" % exists)

    pages_after = raw_page_rows(args, table, KEY)
    meta_after = exact_row(args.host, args.dss_port, table,
                           partition_of(KEY), args.shard, KEY)
    print("  replay-delete-rows metadata=%r pages=%d"
          % (meta_after is not None, len(pages_after)), flush=True)
    if meta_after is not None:
        bad.append("metadata row survived the replayed deletion")
    if pages_after:
        # Accepted sweeper debt (docs/08 §9): the prior was not resident at
        # replay, so there was no inventory to fan out. Reported, not failed.
        print("  sweeper debt: %d orphaned page rows (accepted, §9)"
              % len(pages_after), flush=True)

    # The entry must also be reusable: recreate and read back.
    require_equal(hset_image(client, KEY, image(60, 1, 160)), 60,
                  "recreate after replayed deletion")
    v = client.command(b"HGET", KEY, b"field:%05d" % 3)
    if v is None or not v.startswith(b"generation:1:"):
        bad.append("recreate readback wrong: %r" % (v[:24] if v else v))

    if bad:
        for b in bad:
            print("  FAIL: %s" % b, flush=True)
        print("RESULT: FAIL", flush=True)
        return 1
    print("RESULT: PASS (replayed deletion: Redis-correct, metadata gone, "
          "recreatable; page-row debt goes to the sweeper)", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
