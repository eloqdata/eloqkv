"""Checkpoint/restart/delete/recreate lifecycle for one paged hash.

This is the release-blocking A->M/PD->PC->PM->PX->R path from
docs/08-paged-objects-test-plan.md section 3.1.  It deliberately reuses the
same logical key so stale derived rows or unsafe page-id reuse cannot hide
behind a fresh key:

* checkpoint a multi-page hash, prove its clean pages can be shed, then delete
  every field in one HDEL;
* checkpoint and restart from EloqStore, requiring the key to remain absent;
* repeat last-field deletion through several HDELs and through MULTI/EXEC;
* replace a recreated paged hash with a string, then recreate it as a hash;
* checkpoint, shed, restart again, and compare the complete final model.

In a cluster lane only the replica is restarted.  EloqStore is shared and only
the leader checkpoints, so the restarted replica's cold read is still a direct
durable-store oracle while avoiding an unsupported abrupt promotion in the
one-voter test topology.
"""

from __future__ import annotations

import argparse
import os
import signal
import subprocess
import time

from paged_testlib import (RespClient, require_equal, shed_all_pages,
                           wait_until)


def pairs_to_map(items) -> dict[bytes, bytes]:
    if not isinstance(items, list) or len(items) % 2:
        raise AssertionError("malformed HGETALL reply: %r" % (items,))
    return dict(zip(items[0::2], items[1::2]))


def assert_absent(client: RespClient, key: bytes, label: str) -> None:
    require_equal(client.command(b"EXISTS", key), 0, label + " EXISTS")
    require_equal(client.command(b"TYPE", key), b"none", label + " TYPE")
    require_equal(client.command(b"HLEN", key), 0, label + " HLEN")
    require_equal(client.command(b"HGET", key, b"missing"), None,
                  label + " HGET")


def assert_model(client: RespClient, key: bytes,
                 model: dict[bytes, bytes], label: str) -> None:
    require_equal(client.command(b"TYPE", key), b"hash", label + " TYPE")
    require_equal(client.command(b"HLEN", key), len(model), label + " HLEN")
    require_equal(pairs_to_map(client.command(b"HGETALL", key)), model,
                  label + " HGETALL")


def is_paged(client: RespClient, key: bytes) -> bool:
    reply = client.command(b"HSCAN", key, b"0", b"COUNT", b"1",
                           b"NOVALUES")
    if (not isinstance(reply, list) or len(reply) != 2 or
            not isinstance(reply[0], bytes) or not isinstance(reply[1], list)):
        raise AssertionError("malformed HSCAN representation probe: %r" % reply)
    return reply[0] != b"0"


def populate(client: RespClient, key: bytes, count: int,
             generation: bytes) -> dict[bytes, bytes]:
    model = {}
    commands = []
    for i in range(count):
        field = generation + b":f%04d" % i
        value = generation + b":v%04d:" % i + bytes([i & 0xff]) * 180
        model[field] = value
        commands.append((b"HSET", key, field, value))
    replies = client.pipeline(commands)
    if any(reply != 1 for reply in replies):
        raise AssertionError("%s population returned %r" %
                             (generation.decode(errors="replace"), replies[:8]))
    return model


def wait_absent(port: int, key: bytes, label: str) -> None:
    def absent():
        try:
            with RespClient(port=port, timeout=5) as client:
                return client.command(b"EXISTS", key) == 0
        except (OSError, TimeoutError):
            return False

    wait_until(absent, 40, 0.2, label + " absence")


def wait_model(port: int, key: bytes, model: dict[bytes, bytes],
               label: str) -> None:
    def matches():
        try:
            with RespClient(port=port, timeout=10) as client:
                if client.command(b"HLEN", key) != len(model):
                    return False
                return pairs_to_map(client.command(b"HGETALL", key)) == model
        except (AssertionError, OSError, TimeoutError):
            return False

    wait_until(matches, 60, 0.25, label + " model convergence")


def script_markers(start_script: str) -> tuple[str, ...]:
    """Extract markers shared by the server and its host-manager child."""
    text = open(start_script, encoding="utf-8").read()
    markers = []
    for token in text.split():
        if token.startswith("--config="):
            markers.append(os.path.basename(token.split("=", 1)[1]))
        elif token.startswith(("--eloq_data_path=", "--log_dir=")):
            markers.append(token.split("=", 1)[1])
    if not markers:
        raise AssertionError("could not identify process markers in %s" %
                             start_script)
    return tuple(markers)


def instance_pids(start_script: str) -> list[int]:
    markers = script_markers(start_script)
    result = []
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        root = os.path.join("/proc", name)
        try:
            executable = os.path.basename(os.readlink(os.path.join(root, "exe")))
            if executable not in ("eloqkv", "host_manager"):
                continue
            with open(os.path.join(root, "cmdline"), "rb") as stream:
                command = stream.read().replace(b"\0", b" ").decode(
                    errors="replace")
        except OSError:
            continue
        if any(marker in command for marker in markers):
            result.append(int(name))
    return result


def restart_instance(start_script: str, port: int) -> None:
    pids = instance_pids(start_script)
    if not pids:
        raise AssertionError("restart precondition: no process matched %s" %
                             start_script)
    for pid in pids:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass

    def endpoint_down():
        try:
            with RespClient(port=port, timeout=1):
                return False
        except OSError:
            return True

    wait_until(endpoint_down, 15, 0.1, "port %d to stop" % port)
    subprocess.Popen(["bash", start_script], start_new_session=True,
                     stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

    def endpoint_up():
        try:
            with RespClient(port=port, timeout=3) as client:
                return client.command(b"PING") == b"PONG"
        except (OSError, TimeoutError):
            return False

    wait_until(endpoint_up, 180, 0.5, "port %d restart" % port)


def wait_writable(port: int) -> None:
    probe = b"paged:lifecycle:writable-probe"

    def writable():
        try:
            with RespClient(port=port, timeout=5) as client:
                return client.command(b"SET", probe, b"1") == b"OK"
        except (OSError, TimeoutError):
            return False

    wait_until(writable, 180, 0.5, "leader %d writable" % port)


def delete_in_chunks(client: RespClient, key: bytes,
                     model: dict[bytes, bytes]) -> None:
    fields = list(model)
    chunks = [fields[i:i + 23] for i in range(0, len(fields), 23)]
    if len(chunks) < 2:
        raise AssertionError("chunked-delete precondition needs multiple chunks")
    for index, chunk in enumerate(chunks):
        require_equal(client.command(b"HDEL", key, *chunk), len(chunk),
                      "chunked HDEL %d" % index)


def delete_in_transaction(client: RespClient, key: bytes,
                          model: dict[bytes, bytes]) -> None:
    fields = list(model)
    cut1 = len(fields) // 3
    cut2 = 2 * len(fields) // 3
    chunks = (fields[:cut1], fields[cut1:cut2], fields[cut2:])
    require_equal(client.command(b"MULTI"), b"OK", "lifecycle MULTI")
    for index, chunk in enumerate(chunks):
        require_equal(client.command(b"HDEL", key, *chunk), b"QUEUED",
                      "queued lifecycle HDEL %d" % index)
    require_equal(client.command(b"EXEC"), [len(chunk) for chunk in chunks],
                  "lifecycle delete-all EXEC")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("single", "cluster"), required=True)
    parser.add_argument("--port", type=int, required=True,
                        help="single node or current leader")
    parser.add_argument("--log-dir", required=True)
    parser.add_argument("--start-script", required=True,
                        help="single-node start script; unused in cluster mode")
    parser.add_argument("--replica-port", type=int)
    parser.add_argument("--replica-start-script")
    parser.add_argument("--fields", type=int, default=120)
    parser.add_argument("--checkpoint-wait", type=float, default=15.0)
    args = parser.parse_args()
    if args.fields < 48:
        parser.error("--fields must be at least 48")
    if args.mode == "cluster" and (args.replica_port is None or
                                    not args.replica_start_script):
        parser.error("cluster mode requires replica port and start script")

    key = b"paged:lifecycle:reused-key"
    target_port = args.port if args.mode == "single" else args.replica_port
    target_script = (args.start_script if args.mode == "single" else
                     args.replica_start_script)
    assert target_port is not None and target_script is not None

    control = RespClient(port=args.port)
    try:
        control.command(b"DEL", key)
        initial = populate(control, key, args.fields, b"initial")
        assert_model(control, key, initial, "initial resident")
        if not is_paged(control, key):
            raise AssertionError("initial representation is not paged")
        if args.mode == "cluster":
            wait_model(target_port, key, initial, "initial replica")

        # Successful shedding proves both that a checkpoint completed and that
        # the following HDEL genuinely begins from absent page buffers.
        time.sleep(args.checkpoint_wait)
        shed = shed_all_pages(control, args.log_dir, timeout=30)
        print("phase initial: shed %d clean page marker(s)" % shed, flush=True)

        fields = list(initial)
        require_equal(control.command(b"HDEL", key, b"missing", *fields,
                                      fields[0], b"missing"), len(initial),
                      "one-command delete-all")
        assert_absent(control, key, "after one-command delete-all")
        if args.mode == "cluster":
            wait_absent(target_port, key, "replica after delete-all")

        # In no-WAL lanes the delete must reach EloqStore before the crash. Its
        # absence after restart is the durable oracle; a stale metadata row or
        # incomplete delete fan-out resurrects data and fails below.
        time.sleep(args.checkpoint_wait)
        control.close()
        restart_instance(target_script, target_port)
        if args.mode == "single":
            control = RespClient(port=args.port)
        else:
            wait_writable(args.port)
            control = RespClient(port=args.port)
        wait_absent(target_port, key, "restarted target after delete-all")
        with RespClient(port=target_port) as restarted:
            assert_absent(restarted, key, "after first restart")
        assert_absent(control, key, "control after first restart")
        print("phase delete/checkpoint/restart: key stayed absent", flush=True)

        # A short incarnation exercises last-field removal before rebuilding a
        # multi-page incarnation under the same logical key.
        require_equal(control.command(b"HSET", key, b"small", b"v"), 1,
                      "small recreation")
        require_equal(control.command(b"HDEL", key, b"small"), 1,
                      "small last-field deletion")
        assert_absent(control, key, "after small recreation")

        chunked = populate(control, key, args.fields // 2, b"chunked")
        delete_in_chunks(control, key, chunked)
        assert_absent(control, key, "after chunked delete-all")

        transactional = populate(control, key, 48, b"txn")
        delete_in_transaction(control, key, transactional)
        assert_absent(control, key, "after transactional delete-all")
        print("phase repeated deletion: single/chunked/transactional passed",
              flush=True)

        replacement = populate(control, key, args.fields, b"replacement")
        assert_model(control, key, replacement, "before type overwrite")
        require_equal(control.command(b"SET", key, b"string-successor"), b"OK",
                      "type overwrite SET")
        require_equal(control.command(b"TYPE", key), b"string",
                      "type after overwrite")
        require_equal(control.command(b"GET", key), b"string-successor",
                      "string successor value")
        if args.mode == "cluster":
            def replica_string():
                try:
                    with RespClient(port=target_port, timeout=5) as replica:
                        return (replica.command(b"TYPE", key) == b"string" and
                                replica.command(b"GET", key) ==
                                b"string-successor")
                except OSError:
                    return False
            wait_until(replica_string, 40, 0.2, "replica type overwrite")

        require_equal(control.command(b"DEL", key), 1, "delete string successor")
        final = populate(control, key, args.fields + 17, b"final")
        assert_model(control, key, final, "final resident")
        if not is_paged(control, key):
            raise AssertionError("final recreated representation is not paged")
        if args.mode == "cluster":
            wait_model(target_port, key, final, "final replica")

        time.sleep(args.checkpoint_wait)
        shed = shed_all_pages(control, args.log_dir, timeout=30)
        print("phase final: shed %d clean page marker(s)" % shed, flush=True)
        assert_model(control, key, final, "final refault before restart")

        control.close()
        restart_instance(target_script, target_port)
        if args.mode == "single":
            control = RespClient(port=args.port)
        else:
            wait_writable(args.port)
            control = RespClient(port=args.port)
        wait_model(target_port, key, final, "restarted target final")
        with RespClient(port=target_port) as restarted:
            assert_model(restarted, key, final, "final after restart")
        assert_model(control, key, final, "final control after restart")
        if args.mode == "cluster":
            wait_model(target_port, key, final, "final post-restart replica")
        require_equal(control.command(b"PING"), b"PONG", "final health")
        print("phase overwrite/recreate/checkpoint/restart: model exact",
              flush=True)
    except (AssertionError, OSError, TimeoutError) as exc:
        print("RESULT: FAIL: %s" % exc, flush=True)
        return 1
    finally:
        control.close()

    print("RESULT: PASS", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
