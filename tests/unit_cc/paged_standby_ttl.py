"""Qualify TTL transitions for a paged hash on a leader and standby.

The standby applies serialized commands rather than executing their Redis
front ends.  This test therefore checks every representation boundary on both
nodes: PagedHash -> TTLPagedHash, a mutation while TTL is present,
TTLPagedHash -> PagedHash through PERSIST, and natural expiry.  Each phase is
reported separately so a process abort identifies the command that caused it.

Usage: python3 tests/unit_cc/paged_standby_ttl.py <leader-port> <replica-port>
"""

from __future__ import annotations

import argparse
import time

from paged_testlib import RespClient, RespError, require_equal, wait_until


def connect_when_ready(port: int, phase: str) -> RespClient:
    """Wait for a post-failover node process to begin accepting clients."""
    deadline = time.monotonic() + 180
    last = None
    while time.monotonic() < deadline:
        try:
            return RespClient(port=port, timeout=3)
        except OSError as exc:
            last = exc
            time.sleep(0.2)
    raise AssertionError("%s port %d never became ready: %s" %
                         (phase, port, last))


def safe_command(client: RespClient, phase: str, *args):
    try:
        reply = client.command(*args)
    except (OSError, TimeoutError) as exc:
        raise AssertionError("%s closed/stalled the node connection: %s" %
                             (phase, exc)) from exc
    if isinstance(reply, RespError):
        raise AssertionError("%s returned an error: %r" %
                             (phase, reply.message))
    return reply


def wait_hlen(client: RespClient, key: bytes, want: int, phase: str) -> None:
    def matches():
        try:
            return client.command(b"HLEN", key) == want
        except OSError:
            return False

    wait_until(matches, 30, 0.2, "%s replica HLEN=%d" % (phase, want))


def positive_pttl(client: RespClient, key: bytes, phase: str) -> int:
    ttl = safe_command(client, phase, b"PTTL", key)
    if not isinstance(ttl, int) or ttl <= 0:
        raise AssertionError("%s: got PTTL %r, want a positive TTL" %
                             (phase, ttl))
    return ttl


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("leader", type=int)
    parser.add_argument("replica", type=int)
    parser.add_argument("--fields", type=int, default=96)
    args = parser.parse_args()

    key = b"paged:standby:ttl"
    expiring = key + b":expires"
    leader = None
    replica = None
    try:
        leader = connect_when_ready(args.leader, "leader")
        replica = connect_when_ready(args.replica, "replica")
        with leader, replica:
            safe_command(leader, "cleanup", b"DEL", key, expiring)
            commands = []
            for i in range(args.fields):
                commands.append((b"HSET", key, b"f%04d" % i,
                                 (b"v%04d:" % i) + b"x" * 80))
            replies = leader.pipeline(commands)
            if any(reply != 1 for reply in replies):
                raise AssertionError("population returned non-insert replies")
            wait_hlen(replica, key, args.fields, "population")
            print("phase population: both nodes have %d fields" % args.fields,
                  flush=True)

            require_equal(safe_command(leader, "PEXPIRE", b"PEXPIRE", key,
                                       60000), 1, "leader PEXPIRE")
            def replica_has_ttl():
                try:
                    ttl = replica.command(b"PTTL", key)
                    return isinstance(ttl, int) and ttl > 0
                except OSError as exc:
                    raise AssertionError(
                        "PEXPIRE aborted/closed the replica: %s" % exc) from exc

            wait_until(replica_has_ttl, 30, 0.2,
                       "replica to observe PEXPIRE")
            print("phase PEXPIRE: replica retained TTLPagedHash", flush=True)

            require_equal(safe_command(leader, "HSET with TTL", b"HSET", key,
                                       b"after-ttl", b"kept"), 1,
                          "leader HSET with TTL")
            wait_hlen(replica, key, args.fields + 1, "mutation with TTL")
            positive_pttl(leader, key, "leader mutation with TTL")
            positive_pttl(replica, key, "replica mutation with TTL")
            print("phase mutation-with-TTL: content and TTL replicated",
                  flush=True)

            require_equal(safe_command(leader, "PERSIST", b"PERSIST", key),
                          1, "leader PERSIST")

            def persisted():
                try:
                    return replica.command(b"PTTL", key) == -1
                except OSError as exc:
                    raise AssertionError(
                        "PERSIST aborted/closed the replica: %s" % exc) from exc

            wait_until(persisted, 30, 0.2, "replica to apply PERSIST")
            require_equal(safe_command(leader, "leader after PERSIST", b"PTTL",
                                       key), -1, "leader PTTL after PERSIST")
            wait_hlen(replica, key, args.fields + 1, "PERSIST content")
            print("phase PERSIST: TTL removed without losing content",
                  flush=True)

            # Expiry is also checked independently of PERSIST. A short-lived
            # copy must disappear from both nodes and must not leave an empty
            # logical hash behind.
            require_equal(safe_command(leader, "expiry population", b"HSET",
                                       expiring, b"f", b"v"), 1,
                          "expiry HSET")
            for i in range(args.fields):
                safe_command(leader, "expiry population", b"HSET", expiring,
                             b"f%04d" % i, b"z" * 80)
            wait_hlen(replica, expiring, args.fields + 1, "expiry population")
            require_equal(safe_command(leader, "short PEXPIRE", b"PEXPIRE",
                                       expiring, 1500), 1, "short PEXPIRE")
            def replica_expired():
                try:
                    return replica.command(b"EXISTS", expiring) == 0
                except OSError as exc:
                    raise AssertionError(
                        "natural expiry closed the replica: %s" % exc) from exc

            wait_until(replica_expired, 20, 0.2, "replica natural expiry")
            require_equal(safe_command(leader, "leader natural expiry",
                                       b"EXISTS", expiring), 0,
                          "leader natural expiry")
            print("phase natural-expiry: key absent on both nodes", flush=True)
    except (AssertionError, OSError, TimeoutError) as exc:
        print("RESULT: FAIL: %s" % exc, flush=True)
        return 1

    print("RESULT: PASS", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
