"""After a standby is promoted, does its first checkpoint write a coherent
paged object?

The sibling test (standby_paged_apply.py) covers a standby that stays a
standby. This one covers the moment it stops being one. A promoted node owns
data it never executed a single command against -- per #509 it only ever ran
`Deserialize` + `CommitOn`, and for a paged object its pages may have arrived
by fetch rather than by construction. It is now responsible for checkpointing
that object, and the metadata row and the page rows it writes have to describe
the same object. If they do not, nothing notices until the next restart reads
them back, which is exactly what this test does.

  1. build a paged hash on the leader and let the replica converge
  2. FAILOVER TO the replica, which is promoted when it accepts a write
  3. check the promoted node serves the complete object
  4. write more fields on it -- paged writes on a node that was a standby
  5. wait out the checkpoint interval
  6. kill both nodes, restart them, and read the object back

Step 6 is the assertion: everything in memory is gone, so what comes back was
reconstructed from what the promoted node persisted.

The failover is the graceful `FAILOVER TO <host:port>` rather than killing the
leader. Killing it does NOT promote the survivor in this deployment and that is
not a paged-objects problem: with `node_group_replica_num = 1` there is no
second voter for the node group, and each node runs its own host manager, so
losing one leaves the host-manager raft group without quorum -- the survivor
sits logging WRITE_REQUEST_ON_SLAVE_NODE and never takes over. Automatic
failover needs 3 replicas. The graceful path exercises the same promotion
inside the engine.

Two outcomes are distinguished deliberately. Fields missing from the tail are a
DURABILITY shortfall, and how much is possible depends on whether the WAL is on
(with it off, anything the checkpoint had not reached is gone by design). A
field that comes back with the WRONG VALUE, or an HLEN that disagrees with the
number of fields actually present, is INCOHERENCE, and that is the real failure
this test looks for: it would mean the metadata row and the page rows disagree.

Usage: python3 tests/unit_cc/standby_failover_paged.py <start_a.sh>
                                                       <start_b.sh> [nfields]
                                                       [tail]
"""
import os
import socket
import sys
import time

sys.path.insert(0, "tests/unit_cc")
from paged_testlib import RespClient, RespError, marker_count  # noqa: E402
from standby_paged_apply import (KEY, NODES, cmd, conn,  # noqa: E402
                                 discover_roles, hlen, kill_node, log_dir_of,
                                 start_node, wait_replicated, wait_writable,
                                 write_fields)

# Long enough for several --checkpointer_interval=5 rounds.
CKPT_WAIT = 30
RECOVERY_READ_TIMEOUT = 60
DIAGNOSTIC_MARKERS = (
    "PAGELOG page_fetch_complete",
    "PAGELOG page_admission_refused",
    "CLEANLOG terminal",
    "heap full: 1",
)


def field_names(client, timeout):
    """Every field name currently in the object, read via HKEYS.

    The shared RESP parser rejects EOF, malformed/error replies, duplicate
    fields, and partial arrays. `timeout` is one wall-clock deadline for the
    complete response, not a per-recv timeout.
    """
    reply = client.command_deadline(timeout, "HKEYS", KEY)
    if isinstance(reply, RespError):
        raise AssertionError("HKEYS returned an error: %r" % reply.message)
    if not isinstance(reply, list) or not all(isinstance(v, bytes)
                                              for v in reply):
        raise AssertionError("malformed HKEYS reply: %r" % (reply,))
    out = set(reply)
    if len(out) != len(reply):
        raise AssertionError("HKEYS returned duplicate fields")
    return out


def expected_value(name):
    """The value write_fields() would have written for this field name."""
    return name * 40


def marker_snapshot(scripts):
    """Capture deduplicated marker counts for both node log directories."""
    return {
        log_dir_of(script): {
            token: marker_count(log_dir_of(script), token)
            for token in DIAGNOSTIC_MARKERS
        }
        for script in scripts
    }


def classify_recovery_timeout(lead_port, expected_names, scripts, before):
    """Gather evidence after a post-restart whole-object read times out.

    HLEN is answered from metadata while HKEYS may need every hash page, so a
    long storage/recovery tail and a liveness failure look identical at the
    original deadline. The original client is closed before these probes, but
    its server-side work may still finish and warm pages; a successful probe
    therefore proves progress after the timeout, not that the probe itself
    incurred a page miss.

    Marker deltas are evidence only: completions are not request-tagged and an
    optimized build may compile out PAGELOG DLOG sites. Marker absence never
    assigns or rules out an owning subsystem.
    """
    print("  -- classifying the recovery timeout --", flush=True)
    try:
        with RespClient(port=lead_port, timeout=15) as probe:
            print("     node: %r" % probe.command_deadline(15, "PING"),
                  flush=True)
            t0 = time.monotonic()
            probe_hlen = probe.command_deadline(15, "HLEN", KEY)
            print("     HLEN again: %r (%.3fs)"
                  % (probe_hlen, time.monotonic() - t0), flush=True)
            t0 = time.monotonic()
            field = b"f00000"
            one = probe.command_deadline(15, "HGET", KEY, field)
            print("     single-field HGET: completed=%r value_ok=%r "
                  "(%.3fs); page-miss status unknown"
                  % (not isinstance(one, RespError),
                     one == expected_value(field),
                     time.monotonic() - t0), flush=True)
    except (AssertionError, socket.timeout, TimeoutError, OSError) as exc:
        print("     probe connection failed: %s" % exc, flush=True)

    try:
        with RespClient(port=lead_port, timeout=300) as slow:
            t0 = time.monotonic()
            names = field_names(slow, 300)
        elapsed = time.monotonic() - t0
        if names == expected_names:
            print("     HKEYS retry: coherent %d-field reply in %.1fs; the "
                  "%ds deadline was insufficient for this occurrence, but "
                  "the cause remains unclassified"
                  % (len(names), elapsed, RECOVERY_READ_TIMEOUT), flush=True)
        else:
            print("     HKEYS retry: returned in %.1fs but was INCOHERENT "
                  "(%d present, %d expected, %d missing, %d extra)"
                  % (elapsed, len(names), len(expected_names),
                     len(expected_names - names),
                     len(names - expected_names)), flush=True)
    except (AssertionError, socket.timeout, TimeoutError, OSError) as exc:
        print("     HKEYS retry did not produce a complete coherent reply "
              "within 300s: %s" % exc, flush=True)

    for script in scripts:
        log_dir = log_dir_of(script)
        deltas = {
            token: marker_count(log_dir, token) - before[log_dir][token]
            for token in DIAGNOSTIC_MARKERS
        }
        print("     %s marker deltas since the original HKEYS: %s"
              % (os.path.basename(log_dir), deltas), flush=True)


def main():
    script_a, script_b = sys.argv[1], sys.argv[2]
    nfields = int(sys.argv[3]) if len(sys.argv) > 3 else 400
    tail = int(sys.argv[4]) if len(sys.argv) > 4 else 300

    for port, script, marks in ((7401, script_a,
                                 ("primary.ini", "/p_data", "/p_log")),
                                (7501, script_b,
                                 ("standby.ini", "/s_data", "/s_log"))):
        NODES[port] = {"script": script, "marks": marks,
                       "log_dir": log_dir_of(script)}

    lead_port, rep_port = discover_roles(list(NODES))
    if lead_port is None:
        print("no node accepted a write", flush=True)
        return 1
    print("roles: leader=%d replica=%d" % (lead_port, rep_port), flush=True)

    p = conn(lead_port)
    s = conn(rep_port)
    cmd(p, "DEL", KEY)
    write_fields(p, "f", nfields)
    if not wait_replicated(p, s):
        print("replica never converged before the failover", flush=True)
        return 1
    print("built %d fields, replica converged" % nfields, flush=True)
    s.close()
    p.close()

    # ---- failover ------------------------------------------------------
    f = conn(lead_port, timeout=180)
    r = cmd(f, "FAILOVER", "TO", "127.0.0.1:%d" % rep_port)
    f.close()
    print("FAILOVER TO 127.0.0.1:%d -> %r" % (rep_port, r[:60]), flush=True)
    if r[:1] != b"+":
        print("RESULT: FAIL (failover was refused)", flush=True)
        return 1
    waited = wait_writable(rep_port, limit=240)
    print("promoted node writable after %rs" % waited, flush=True)
    if waited is None:
        print("RESULT: FAIL (replica was never promoted)", flush=True)
        return 1

    n = conn(rep_port)
    after_promote = hlen(n)
    print("HLEN on the promoted node: %r (expected %d)"
          % (after_promote, nfields), flush=True)
    if after_promote != nfields:
        print("RESULT: FAIL (promoted node lost acknowledged fields)",
              flush=True)
        return 1

    # ---- paged writes on the node that used to be a standby -------------
    write_fields(n, "g", tail)
    hkeys_started = time.monotonic()
    with RespClient(port=rep_port) as live_reader:
        expected_names = field_names(live_reader, RECOVERY_READ_TIMEOUT)
    live_hkeys_elapsed = time.monotonic() - hkeys_started
    expected_hlen = hlen(n)
    print("after %d more writes: HLEN=%r fields=%d (HKEYS %.3fs)"
          % (tail, expected_hlen, len(expected_names), live_hkeys_elapsed),
          flush=True)
    if expected_hlen != len(expected_names):
        print("RESULT: FAIL (HLEN disagrees with HKEYS on the live object)",
              flush=True)
        return 1

    print("waiting %ds for the checkpoint" % CKPT_WAIT, flush=True)
    time.sleep(CKPT_WAIT)
    n.close()

    # ---- full restart: the store is the only source left ----------------
    kill_node(rep_port)
    kill_node(lead_port)
    time.sleep(5)
    start_node(lead_port)
    time.sleep(3)
    start_node(rep_port)

    new_lead, new_rep = discover_roles(list(NODES), limit=300)
    if new_lead is None:
        print("RESULT: FAIL (cluster did not come back)", flush=True)
        return 1
    print("after restart: leader=%d replica=%d" % (new_lead, new_rep),
          flush=True)

    with RespClient(port=new_lead, timeout=15) as metadata_reader:
        hlen_started = time.monotonic()
        final_hlen = metadata_reader.command_deadline(15, "HLEN", KEY)
    hlen_elapsed = time.monotonic() - hlen_started
    if not isinstance(final_hlen, int):
        print("RESULT: FAIL (post-restart HLEN returned %r)" % final_hlen,
              flush=True)
        return 1

    scripts = (script_a, script_b)
    markers_before = marker_snapshot(scripts)
    hkeys_started = time.monotonic()
    try:
        # A dedicated connection is intentionally discarded on timeout: its
        # RESP stream contains an unknown partial array and cannot be reused.
        with RespClient(port=new_lead,
                        timeout=RECOVERY_READ_TIMEOUT) as recovery_reader:
            final_names = field_names(recovery_reader,
                                      RECOVERY_READ_TIMEOUT)
    except (socket.timeout, TimeoutError, OSError):
        hkeys_elapsed = time.monotonic() - hkeys_started
        print("recovered: HLEN=%r (%.3fs); HKEYS did not finish in %.3fs"
              % (final_hlen, hlen_elapsed, hkeys_elapsed), flush=True)
        classify_recovery_timeout(new_lead, expected_names, scripts,
                                  markers_before)
        print("RESULT: FAIL (post-restart HKEYS exceeded the %ds recovery "
              "read timeout)" % RECOVERY_READ_TIMEOUT, flush=True)
        return 1
    except AssertionError as exc:
        print("RESULT: FAIL (post-restart HKEYS protocol/content error: %s)"
              % exc, flush=True)
        return 1
    hkeys_elapsed = time.monotonic() - hkeys_started
    print("recovered: HLEN=%r (%.3fs) fields=%d (HKEYS %.3fs); before the "
          "restart: %r / %d"
          % (final_hlen, hlen_elapsed, len(final_names), hkeys_elapsed,
             expected_hlen, len(expected_names)), flush=True)

    # Coherence, checked three ways.
    incoherent = []
    if final_hlen != len(final_names):
        incoherent.append("HLEN %r != %d fields present"
                          % (final_hlen, len(final_names)))
    extra = final_names - expected_names
    if extra:
        incoherent.append("%d fields present that were never written"
                          % len(extra))
    bad_values = 0
    with RespClient(port=new_lead, timeout=15) as value_reader:
        for name in sorted(final_names)[:: max(1, len(final_names) // 50)]:
            got = value_reader.command_deadline(15, "HGET", KEY, name)
            if got != expected_value(name):
                bad_values += 1
    if bad_values:
        incoherent.append("%d sampled fields hold the wrong value"
                          % bad_values)

    missing = len(expected_names - final_names)
    print("durability: %d of %d fields survived (%d missing)"
          % (len(final_names), len(expected_names), missing), flush=True)

    if incoherent:
        for m in incoherent:
            print("  INCOHERENT: %s" % m, flush=True)
        print("RESULT: FAIL (the promoted node's checkpoint wrote an "
              "incoherent object)", flush=True)
        return 1
    print("RESULT: PASS (object is coherent after failover + checkpoint + "
          "full restart)", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
