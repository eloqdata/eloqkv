"""Crash/restart coherence matrix for atomic paged-object mutations.

No-WAL lanes may recover either side of an acknowledged post-checkpoint
mutation, but never a torn metadata/page mixture. WAL lanes must recover the
acknowledged side and prove replay's paged drain page-faulted. Each mutation is
one Redis command so its old/new images are unambiguous.
"""

from __future__ import annotations

import argparse
import glob
import os
import subprocess
import time

from paged_hash_matrix import pairs_to_map
from paged_testlib import RespClient, marker_count, require_equal, wait_until


def kill_run(run_dir):
    killed = []
    for proc in glob.glob("/proc/[0-9]*"):
        try:
            pid = int(os.path.basename(proc))
            with open(os.path.join(proc, "cmdline"), "rb") as stream:
                command = stream.read().replace(b"\0", b" ")
            exe = os.readlink(os.path.join(proc, "exe"))
        except (OSError, ValueError):
            continue
        if (run_dir.encode() in command and
                (exe.endswith("/eloqkv") or exe.endswith("/host_manager"))):
            try:
                os.kill(pid, 9)
                killed.append(pid)
            except OSError:
                pass
    if not killed:
        raise AssertionError("no run-scoped EloqKV process found to SIGKILL")
    return killed


def port_ready(port):
    try:
        with RespClient(port=port, timeout=2) as client:
            return client.command(b"PING") == b"PONG"
    except OSError:
        return False


def restart(start_script, run_dir, port, label):
    pids = kill_run(run_dir)
    wait_until(lambda: not port_ready(port), 20, 0.1,
               label + " port closure")
    output = os.path.join(run_dir, "persistence-%s.stdout" % label)
    with open(output, "wb") as stream:
        subprocess.Popen(["bash", start_script], start_new_session=True,
                         stdout=stream, stderr=subprocess.STDOUT)
    wait_until(lambda: port_ready(port), 180, 0.5,
               label + " restart serving")
    print("%s: killed pids %r and restarted" % (label, pids), flush=True)


def read_hash(client, key):
    reply = client.command(b"HGETALL", key)
    if not isinstance(reply, list):
        raise AssertionError("HGETALL returned %r" % reply)
    return pairs_to_map(reply)


def require_old_or_new(got, old, new, wal, label):
    allowed = [new] if wal else [old, new]
    if got not in allowed:
        raise AssertionError("%s recovered torn/unknown state: got %r, "
                             "allowed %r" % (label, got, allowed))
    selected = "new" if got == new else "old checkpoint"
    print("%s: coherent %s image" % (label, selected), flush=True)


def checkpoint_wait(seconds):
    # The harness uses a five-second interval. Waiting across two intervals is
    # deliberate: the public protocol has no checkpoint-complete command.
    time.sleep(seconds)


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
    key = b"paged:persistence:atomic"

    try:
        with RespClient(port=args.port) as client:
            client.command(b"DEL", key)
            baseline = {}
            command = [b"HSET", key]
            for i in range(args.fields):
                field = b"f%05d" % i
                value = b"base:%05d:" % i + bytes([i & 0xff]) * 160
                command.extend((field, value))
                baseline[field] = value
            require_equal(client.command(*command), args.fields,
                          "baseline population")
            scan = client.command(b"HSCAN", key, b"0", b"COUNT", b"1",
                                  b"NOVALUES")
            if not isinstance(scan, list) or scan[0] == b"0":
                raise AssertionError("baseline did not convert to paged")
        checkpoint_wait(args.checkpoint_wait)

        stalls_before = marker_count(args.log_dir, "FAULTLOG drain_page_stall")
        updated = dict(baseline)
        with RespClient(port=args.port) as client:
            command = [b"HSET", key]
            inserted = 0
            for i in range(0, args.fields, 7):
                field = b"f%05d" % i
                value = b"updated:%05d:" % i + b"u" * 240
                command.extend((field, value))
                updated[field] = value
            for i in range(24):
                field = b"new%05d" % i
                value = b"new-value:" + bytes([i]) * 190
                command.extend((field, value))
                updated[field] = value
                inserted += 1
            require_equal(client.command(*command), inserted,
                          "atomic multi-page update")
        restart(args.start_script, args.run_dir, args.port, "update")
        with RespClient(port=args.port) as client:
            recovered = read_hash(client, key)
        require_old_or_new(recovered, baseline, updated, wal, "update crash")

        # Establish a known durable paged baseline independent of which side a
        # no-WAL recovery selected, then crash after one delete-all command.
        with RespClient(port=args.port) as client:
            client.command(b"DEL", key)
            command = [b"HSET", key]
            for field, value in updated.items():
                command.extend((field, value))
            require_equal(client.command(*command), len(updated),
                          "delete baseline")
        checkpoint_wait(args.checkpoint_wait)
        with RespClient(port=args.port) as client:
            require_equal(client.command(b"HDEL", key, *updated.keys()),
                          len(updated), "atomic delete all")
        restart(args.start_script, args.run_dir, args.port, "delete-all")
        with RespClient(port=args.port) as client:
            recovered = read_hash(client, key)
        require_old_or_new(recovered, updated, {}, wal, "delete-all crash")

        # Replacement is the sweeper-dependent case: after restart the key is
        # either the complete old hash or the complete new string, never a
        # visible derived page row or a hybrid type.
        with RespClient(port=args.port) as client:
            client.command(b"DEL", key)
            command = [b"HSET", key]
            for field, value in baseline.items():
                command.extend((field, value))
            require_equal(client.command(*command), len(baseline),
                          "replacement baseline")
        checkpoint_wait(args.checkpoint_wait)
        with RespClient(port=args.port) as client:
            require_equal(client.command(b"SET", key, b"replacement"), b"OK",
                          "atomic type replacement")
        restart(args.start_script, args.run_dir, args.port, "replacement")
        with RespClient(port=args.port) as client:
            key_type = client.command(b"TYPE", key)
            if key_type == b"hash":
                replacement_state = (b"hash", read_hash(client, key))
            elif key_type == b"string":
                replacement_state = (b"string", client.command(b"GET", key))
            else:
                replacement_state = (key_type, None)
        require_old_or_new(replacement_state,
                           (b"hash", baseline),
                           (b"string", b"replacement"),
                           wal,
                           "replacement crash")

        if wal:
            wait_until(lambda: marker_count(args.log_dir,
                                            "FAULTLOG drain_page_stall") >
                       stalls_before,
                       15, 0.2, "WAL replay page-fault marker")
        with RespClient(port=args.port) as client:
            require_equal(client.command(b"PING"), b"PONG", "final health")
    except (AssertionError, OSError, TimeoutError) as exc:
        print("RESULT: FAIL: %s" % exc, flush=True)
        return 1

    print("RESULT: PASS (wal=%s; every crash recovered a coherent image)" %
          args.wal, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
