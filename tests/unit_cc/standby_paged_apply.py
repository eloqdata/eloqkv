"""Does a standby apply paged commands correctly when it must read them back?

Phase 6b of docs/08-paged-objects.md. A standby node never runs `ExecuteOn` --
it only `Deserialize`s and replays `CommitOn` (the #509 invariant), so nothing
on a standby pins pages. A `CommitOn` arriving for a page that is not resident
must therefore fault, and the standby must do what the replay drain does: leave
the object untouched, buffer the command, and issue the fetch. Dropping the
fault would let the standby diverge silently from the primary, and a divergent
standby that gets promoted is data loss, not a cache miss.

  Phase A  build a paged hash on the leader, confirm the replica matches
  Phase B  arm shed_all_pages on the replica, write a tail, compare again
  Phase C  kill the replica, write a tail it misses, bring it back, and KEEP
           writing while it catches up

Phase C is the one that reaches the divert, and the "keep writing" half is the
part that matters. Two ways of arriving at a paged object on a standby differ:

  built    the standby applied every command itself, so it created each page
           and all of them are resident. Nothing can fault. (Phase A/B.)
  fetched  the standby restarted, and a forwarded command found no entry, so
           it read the record back from the store. A stored paged object is
           metadata only -- its pages live under separate keys -- so it arrives
           with ZERO resident pages, and the next CommitOn faults. (Phase C.)

shed_all_pages cannot substitute for this. It only sheds pages whose `flushed_`
is set, and `flushed_` is set by the checkpointer, which a standby never runs;
`EvictablePageCount()` on a standby is therefore always zero. Phase B is kept
because it is the natural thing to try and it costs nothing, but on its own it
proves only that replication works -- the first version of this test did just
that and reported PASS without the standby ever faulting.

Which node is the leader is NOT fixed by the config: both nodes are in the same
node group and the role falls out of the ng leader election, so a run can come
up with either port serving writes. The test discovers the roles instead of
assuming them, and prints which log directory belongs to the replica so the
FAULTLOG check afterwards reads the right file.

Requires two nodes with --paged_hash_convert_threshold=1 so every hash pages,
and a Debug build for the fault injectors.

Usage: python3 tests/unit_cc/standby_paged_apply.py <start_a.sh> <start_b.sh>
                                                    [nfields] [tail]
"""
import glob
import os
import signal
import socket
import subprocess
import sys
import time

KEY = "sb:paged"

# port -> how to recognise and restart that node. Filled in by main() with the
# two start scripts; the log directory is read out of the script itself so the
# marker check afterwards cannot be pointed at the wrong instance.
NODES = {}


def conn(port, timeout=90):
    """Open a connection.

    The timeout is generous because a read on a freshly restarted replica is a
    chain of page fetches from object storage, and a cold HGET there has taken
    well over 20 s. A short timeout turns that into a spurious failure.
    """
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def send(sock, *args):
    out = ("*%d\r\n" % len(args)).encode()
    for a in args:
        b = a if isinstance(a, bytes) else str(a).encode()
        out += b"$%d\r\n%s\r\n" % (len(b), b)
    sock.sendall(out)


def read_reply(sock):
    """Read one reply in full, arrays included.

    @return the reply's first line (its header for aggregate types). Arrays are
    drained element by element so the socket stays usable -- a half-read reply
    is what corrupted an earlier test's accounting.
    """
    buf = b""

    def line():
        nonlocal buf
        while b"\r\n" not in buf:
            chunk = sock.recv(65536)
            if not chunk:
                raise OSError("connection closed")
            buf += chunk
        ln, buf = buf.split(b"\r\n", 1)
        return ln

    def bulk(n):
        nonlocal buf
        if n < 0:
            return
        while len(buf) < n + 2:
            buf += sock.recv(65536)
        buf = buf[n + 2:]

    head = line()
    if head[:1] == b"$":
        bulk(int(head[1:]))
    elif head[:1] == b"*":
        for _ in range(max(0, int(head[1:]))):
            el = line()
            if el[:1] == b"$":
                bulk(int(el[1:]))
    return head


def cmd(sock, *args):
    send(sock, *args)
    return read_reply(sock)


def hlen(sock):
    r = cmd(sock, "HLEN", KEY)
    return int(r[1:]) if r[:1] == b":" else None


def value(sock, field):
    """HGET one field, returning the payload rather than the header."""
    send(sock, "HGET", KEY, field)
    buf = b""
    while b"\r\n" not in buf:
        buf += sock.recv(65536)
    head, rest = buf.split(b"\r\n", 1)
    if head[:1] != b"$" or int(head[1:]) < 0:
        return None
    n = int(head[1:])
    while len(rest) < n + 2:
        rest += sock.recv(65536)
    return rest[:n]


def node_pids(port):
    """PIDs belonging to one node, the other left alone.

    Both nodes run the same binary from the same build, so the executable name
    cannot tell them apart -- each is identified by its own config and data
    paths appearing in the command line. /proc is read directly rather than
    shelling out to pgrep, whose own command line would match the pattern it
    was given.
    """
    marks = NODES[port]["marks"]
    out = []
    for entry in glob.glob("/proc/[0-9]*"):
        try:
            exe = os.path.basename(os.readlink(entry + "/exe"))
            if exe not in ("eloqkv", "host_manager"):
                continue
            with open(entry + "/cmdline", "rb") as fh:
                argv = fh.read().decode(errors="ignore")
        except OSError:
            continue
        if any(m in argv for m in marks):
            out.append(int(os.path.basename(entry)))
    return out


def kill_node(port):
    """SIGKILL one node. @return the pids it killed."""
    pids = node_pids(port)
    for pid in pids:
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass
    return pids


def start_node(port):
    subprocess.Popen(["bash", NODES[port]["script"]], start_new_session=True,
                     stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


def discover_roles(ports, limit=180):
    """Find which node serves writes.

    Role is decided by the node-group leader election, not by the config, so a
    run can come up with either port leading.

    @return (leader_port, replica_port), or (None, None) if no node accepted a
    write within `limit` seconds.
    """
    deadline = time.time() + limit
    while time.time() < deadline:
        for port in ports:
            try:
                c = conn(port, timeout=5)
            except OSError:
                continue
            try:
                if cmd(c, "SET", "probe:role", "1")[:1] == b"+":
                    other = [q for q in ports if q != port][0]
                    return port, other
            except OSError:
                pass
            finally:
                c.close()
        time.sleep(2)
    return None, None


def wait_writable(port, limit=180):
    """Block until `port` accepts a write again.

    Losing a node briefly costs the survivor its ability to start transactions
    ("Failed to initialize the transaction") while the topology settles -- a
    cluster-membership effect, nothing to do with paged objects.

    @return seconds waited, or None if it never recovered.
    """
    t0 = time.time()
    while time.time() - t0 < limit:
        try:
            c = conn(port, timeout=5)
        except OSError:
            time.sleep(1)
            continue
        try:
            if cmd(c, "SET", "probe:writable", "1")[:1] == b"+":
                return round(time.time() - t0, 1)
        except OSError:
            pass
        finally:
            c.close()
        time.sleep(1)
    return None


def compare(p, s, fields, label):
    """Compare HLEN and a sample of fields across the two nodes.

    @return the number of mismatches; the first few are printed.
    """
    pl, sl = hlen(p), hlen(s)
    bad = 0
    if pl != sl:
        print("  %s: HLEN leader=%r replica=%r  MISMATCH" % (label, pl, sl),
              flush=True)
        bad += 1
    else:
        print("  %s: HLEN both %r" % (label, pl), flush=True)
    for f in fields:
        pv, sv = value(p, f), value(s, f)
        if pv != sv:
            bad += 1
            if bad <= 5:
                print("  %s: field %s leader=%r replica=%r" %
                      (label, f, (pv or b"")[:16], (sv or b"")[:16]),
                      flush=True)
    return bad


def wait_replicated(p, s, tries=40):
    """Wait until the replica's HLEN catches up with the leader's.

    Standby apply is asynchronous, so a mismatch read immediately after a write
    is lag, not divergence -- only a value that never converges is a bug.

    @return True if it converged.
    """
    for _ in range(tries):
        try:
            if hlen(p) == hlen(s):
                return True
        except OSError:
            pass
        time.sleep(0.5)
    return False


def write_fields(p, prefix, n, retries=60):
    """Write n acknowledged fields, one command per round trip.

    A node joining or leaving the cluster costs the leader its ability to start
    transactions for a few seconds ("Failed to initialize the transaction"), so
    a write straddling a membership change is retried rather than treated as a
    failure. Every field is still acknowledged before the next is sent -- the
    retry waits for an acknowledgement, it does not skip one.
    """
    for i in range(n):
        name = "%s%05d" % (prefix, i)
        for attempt in range(retries):
            r = cmd(p, "HSET", KEY, name, name * 40)
            if r[:1] == b":":
                break
            if not r.startswith(b"-Failed to initialize"):
                raise OSError("HSET %s not acknowledged: %r" % (name, r[:40]))
            time.sleep(1)
        else:
            raise OSError("HSET %s never acknowledged after %ds"
                          % (name, retries))


def drain_stalls(log_dir):
    """How many page faults the RESTARTED replica raised on its apply path.

    Reads only the newest INFO file. Scanning the directory would mix the
    pre-restart and post-restart instances -- glog also leaves a symlink beside
    each timestamped file, double-counting every line -- and a pre-restart hit
    counted here would be exactly the false positive this check exists to rule
    out.

    @return the number of FAULTLOG drain_page_stall lines, 0 if none.
    """
    infos = sorted(glob.glob(os.path.join(log_dir, "eloqdb.log.INFO.*")),
                   key=os.path.getmtime)
    if not infos:
        return 0
    n = 0
    with open(infos[-1], errors="ignore") as fh:
        for line in fh:
            if "FAULTLOG drain_page_stall" in line:
                n += 1
    return n


def log_dir_of(script):
    """The --log_dir the start script passes, so the caller can read the right
    instance's glog files."""
    text = open(script).read()
    for tok in text.split():
        if tok.startswith("--log_dir="):
            return tok.split("=", 1)[1]
    return None


def main():
    script_a, script_b = sys.argv[1], sys.argv[2]
    nfields = int(sys.argv[3]) if len(sys.argv) > 3 else 400
    tail = int(sys.argv[4]) if len(sys.argv) > 4 else 300
    # Long enough for at least one --checkpointer_interval=5 round to land.
    ckpt_wait = 15

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
    print("replica log dir: %s" % NODES[rep_port]["log_dir"], flush=True)

    p = conn(lead_port)
    s = conn(rep_port)
    cmd(p, "DEL", KEY)

    # ---- Phase A: a paged object, replicated ----------------------------
    write_fields(p, "f", nfields)
    sample = ["f%05d" % i for i in (0, nfields // 2, nfields - 1)]
    conv = wait_replicated(p, s)
    print("phase A (%d fields, converged=%s)" % (nfields, conv), flush=True)
    bad_a = compare(p, s, sample, "A")

    # ---- Phase B: try to evict pages on the replica ----------------------
    armed = cmd(s, "fault_inject", "shed_all_pages", -1)
    print("phase B: arm shed_all_pages on replica -> %r" % armed[:40],
          flush=True)
    if armed[:1] == b"-":
        print("RESULT: SKIP (fault injection unavailable)", flush=True)
        return 2
    for i in range(tail):
        r = cmd(p, "HSET", KEY, "t%05d" % i, ("b%05d" % i) * 40)
        if r[:1] != b":":
            raise OSError("HSET t%05d not acknowledged: %r" % (i, r[:40]))
        if i % 25 == 0:
            cmd(s, "fault_inject", "force_shard_clean_now", -1)
    cmd(s, "fault_inject", "shed_all_pages", -1, "remove")
    cmd(s, "fault_inject", "force_shard_clean_now", -1, "remove")
    conv_b = wait_replicated(p, s)
    sample_b = sample + ["t%05d" % i for i in (0, tail // 2, tail - 1)]
    print("phase B (%d more fields, converged=%s)" % (tail, conv_b), flush=True)
    bad_b = compare(p, s, sample_b, "B")

    # ---- Phase C: replica restarts and catches up while writes continue --
    # Wait for a checkpoint first: the replica can only read back what the
    # leader made durable, and the point of the phase is that it reads back a
    # PAGED object rather than replaying everything.
    time.sleep(ckpt_wait)
    s.close()
    print("phase C: killed replica pids %r" % kill_node(rep_port), flush=True)

    waited = wait_writable(lead_port)
    print("phase C: leader writable again after %rs" % waited, flush=True)
    if waited is None:
        print("RESULT: FAIL (leader never recovered from losing the replica)",
              flush=True)
        return 1
    p = conn(lead_port)
    write_fields(p, "c", tail)
    start_node(rep_port)

    s = None
    for _ in range(150):
        try:
            s = conn(rep_port, timeout=5)
            if cmd(s, "PING")[:1] == b"+":
                break
            s = None
        except OSError:
            s = None
        time.sleep(1)
    if s is None:
        print("phase C: replica never came back", flush=True)
        print("RESULT: FAIL", flush=True)
        return 1

    # Put the replica into the state the divert exists for, rather than hoping
    # it lands there. HLEN answers from the metadata block alone, so serving it
    # pulls the RECORD back from the store and leaves the object exactly as a
    # fetch delivers it: metadata resident, every page non-resident. Writing
    # only after that makes the next CommitOn fault by construction.
    #
    # Without this the outcome is a coin flip. If the leader has already
    # checkpointed past the arriving commands the replica discards them
    # (commit_ts < NativeNodeGroupCkptTs), never builds the entry, and serves
    # later reads from the store instead -- a correct path that faults nothing
    # on the apply side. Runs alternated between 87 faults and 0 before this.
    # Retry until it actually answers: a replica that has just opened its port
    # can still refuse reads for a few seconds, and an HLEN that errored primed
    # nothing.
    primed = None
    for _ in range(60):
        primed = hlen(s)
        if primed is not None:
            break
        time.sleep(1)
    print("phase C: primed the replica with HLEN -> %r" % primed, flush=True)
    if primed is None:
        print("RESULT: FAIL (replica never served a read after restart)",
              flush=True)
        return 1

    # The writes that matter are the ones AFTER the replica is back: they land
    # on an object it had to fetch, whose pages are all non-resident.
    write_fields(p, "d", tail)

    conv_c = wait_replicated(p, s, tries=150)
    sample_c = (sample_b + ["c%05d" % i for i in (0, tail // 2, tail - 1)] +
                ["d%05d" % i for i in (0, tail // 2, tail - 1)])
    print("phase C (replica restarted, converged=%s)" % conv_c, flush=True)
    bad_c = compare(p, s, sample_c, "C")

    print("expected final HLEN: %r" % hlen(p), flush=True)

    # Matching content is necessary but not sufficient: a replica that never
    # faulted agrees trivially and says nothing about the divert. Earlier
    # versions of this test passed that way twice.
    stalls = drain_stalls(NODES[rep_port]["log_dir"])
    print("page faults on the restarted replica: %d" % stalls, flush=True)

    ok = (bad_a == 0 and bad_b == 0 and bad_c == 0 and conv and conv_b
          and conv_c)
    if not ok:
        print("RESULT: FAIL", flush=True)
        return 1
    if stalls == 0:
        print("RESULT: INCONCLUSIVE (replica never faulted; the divert was "
              "not exercised)", flush=True)
        return 2
    print("RESULT: PASS", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
