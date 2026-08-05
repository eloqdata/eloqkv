"""Deterministic paged-flush crash boundaries against live EloqStore.

Each case arms one production fault point with PANIC, performs one atomic
multi-page HSET, proves that the named point fired, restarts the same durable
namespace, and accepts only the complete old or complete new logical image.
Nine concrete hooks cover the seven conceptual §11 points (the callback and
log-truncation points each have before/after hooks). The two log-truncation
hooks are WAL-only; no-WAL deployments do not execute that lifecycle. The
expected side is
narrower where the boundary is unambiguous: a no-WAL
crash before submission must retain the old checkpoint, while a crash after
EloqStore's successful completion must expose the new image.  With WAL, every
acknowledged HSET must recover to the new image at every boundary.
"""

from __future__ import annotations

import argparse
import glob
import os
import subprocess
import time

from paged_hash_matrix import pairs_to_map
from paged_testlib import RespClient, RespError, marker_count, require_equal
from paged_testlib import wait_until


# old/new policy for a no-WAL restart. "either" is the deliberately
# indeterminate in-flight store boundary.
CRASH_POINTS = (
    ("paged_flush_before_export", "old"),
    ("paged_flush_after_export", "old"),
    ("paged_flush_after_dss_expansion", "old"),
    ("paged_flush_store_inflight", "either"),
    ("paged_flush_after_store_commit", "new"),
    ("paged_flush_before_apply_callback", "new"),
    ("paged_flush_after_apply_callback", "new"),
    ("checkpoint_before_log_truncate_advance", "new"),
    ("checkpoint_after_log_truncate_advance", "new"),
)


def run_pids(run_dir, executable):
    result = set()
    for proc in glob.glob("/proc/[0-9]*"):
        try:
            pid = int(os.path.basename(proc))
            with open(os.path.join(proc, "cmdline"), "rb") as stream:
                command = stream.read().replace(b"\0", b" ")
            exe = os.readlink(os.path.join(proc, "exe"))
        except (OSError, ValueError):
            continue
        if run_dir.encode() in command and exe.endswith("/" + executable):
            result.add(pid)
    return result


def kill_run(run_dir):
    killed = []
    for executable in ("eloqkv", "host_manager"):
        for pid in run_pids(run_dir, executable):
            try:
                os.kill(pid, 9)
                killed.append(pid)
            except OSError:
                pass
    return killed


def port_ready(port):
    try:
        with RespClient(port=port, timeout=2) as client:
            return client.command(b"PING") == b"PONG"
    except OSError:
        return False


def restart(start_script, run_dir, port, label):
    kill_run(run_dir)
    wait_until(lambda: not port_ready(port), 20, 0.1,
               label + " port closure")
    output = os.path.join(run_dir, "flush-crash-%s.stdout" % label)
    with open(output, "wb") as stream:
        subprocess.Popen(["bash", start_script], start_new_session=True,
                         stdout=stream, stderr=subprocess.STDOUT)
    wait_until(lambda: port_ready(port), 180, 0.5,
               label + " restart serving")


def make_images(index, fields):
    baseline = {}
    updated = {}
    for field_index in range(fields):
        field = b"f%05d" % field_index
        baseline[field] = (b"base:%02d:%05d:" % (index, field_index) +
                           bytes([field_index & 0xff]) * 160)
        updated[field] = (b"next:%02d:%05d:" % (index, field_index) +
                          bytes([(field_index + 97) & 0xff]) * 220)
    return baseline, updated


def hset_image(client, key, image, label):
    command = [b"HSET", key]
    for field, value in image.items():
        command.extend((field, value))
    reply = client.command(*command)
    if isinstance(reply, RespError):
        raise AssertionError("%s returned error: %s" %
                             (label, reply.message))
    return reply


def read_hash(client, key):
    reply = client.command(b"HGETALL", key)
    if not isinstance(reply, list):
        raise AssertionError("HGETALL returned %r" % (reply,))
    return pairs_to_map(reply)


def require_recovery(got, old, new, wal, no_wal_policy, label):
    if wal or no_wal_policy == "new":
        allowed = (new,)
    elif no_wal_policy == "old":
        allowed = (old,)
    else:
        allowed = (old, new)
    if got not in allowed:
        raise AssertionError("%s recovered torn/incorrect image; got %d "
                             "fields, allowed sides=%s" %
                             (label, len(got), no_wal_policy))
    selected = "new" if got == new else "old"
    print("%s: coherent %s image" % (label, selected), flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=7399)
    parser.add_argument("--start-script", required=True)
    parser.add_argument("--run-dir", required=True)
    parser.add_argument("--log-dir", required=True)
    parser.add_argument("--wal", choices=("on", "off"), required=True)
    parser.add_argument("--checkpoint-wait", type=float, default=12.0)
    parser.add_argument("--fields", type=int, default=180)
    args = parser.parse_args()
    wal = args.wal == "on"

    cases = []
    try:
        # Build every old image before arming any hook, then cross two harness
        # checkpoint intervals once. Subsequent cases cannot dirty untouched
        # keys, so each crash point has one unambiguous candidate record.
        selected_points = [
            point for point in CRASH_POINTS
            if wal or not point[0].startswith("checkpoint_")
        ]
        if not wal:
            print("no-WAL: checkpoint log-truncation hooks are not applicable",
                  flush=True)
        with RespClient(port=args.port, timeout=30) as client:
            for index, (fault_name, policy) in enumerate(selected_points):
                key = b"paged:flush-crash:%02d" % index
                old, new = make_images(index, args.fields)
                client.command(b"DEL", key)
                require_equal(hset_image(client, key, old, fault_name),
                              args.fields, fault_name + " baseline")
                scan = client.command(b"HSCAN", key, b"0", b"COUNT", b"1",
                                      b"NOVALUES")
                if not isinstance(scan, list) or scan[0] == b"0":
                    raise AssertionError("%s baseline did not prove paging" %
                                         fault_name)
                cases.append((fault_name, policy, key, old, new))
        time.sleep(args.checkpoint_wait)

        for fault_name, policy, key, old, new in cases:
            trigger_marker = "FaultInject trigger name=" + fault_name
            hits_before = marker_count(args.log_dir, trigger_marker)
            original_pids = run_pids(args.run_dir, "eloqkv")
            if not original_pids:
                raise AssertionError("%s: no EloqKV pid before arm" %
                                     fault_name)

            with RespClient(port=args.port, timeout=30) as client:
                require_equal(client.command(b"fault_inject", fault_name, -1,
                                             b"action=PANIC"),
                              b"OK", fault_name + " arm")
                # The Redis mutation is acknowledged before checkpoint export
                # reaches any of these hooks.
                hset_image(client, key, new, fault_name + " mutation")

            wait_until(lambda: not (original_pids &
                                   run_pids(args.run_dir, "eloqkv")),
                       45, 0.1, fault_name + " PANIC")
            wait_until(lambda: marker_count(args.log_dir, trigger_marker) >
                       hits_before,
                       10, 0.1, fault_name + " non-vacuous hit marker")

            restart(args.start_script, args.run_dir, args.port, fault_name)
            with RespClient(port=args.port, timeout=60) as client:
                recovered = read_hash(client, key)
            require_recovery(recovered, old, new, wal, policy, fault_name)

        with RespClient(port=args.port) as client:
            require_equal(client.command(b"PING"), b"PONG", "final health")
    except (AssertionError, OSError, TimeoutError) as exc:
        print("RESULT: FAIL: %s" % exc, flush=True)
        return 1

    print("RESULT: PASS (wal=%s; %d deterministic crash points)" %
          (args.wal, len(selected_points)), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
