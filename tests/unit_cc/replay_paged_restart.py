"""Crash-restart WAL replay of a paged object (docs/08 §10, §16).

Replay is the one path where CommitOn genuinely has no pinned pages. Per issue
#509 the replay/standby/migration paths run Deserialize + CommitOn and never
ExecuteOn, and Deserialize restores metadata only -- so every page a replayed
command touches is non-resident. That is what the buffered-command drain's
page-fault handling exists for: the drain stops on the faulting command,
records how far it got, fetches under the reserved kDrainTxnNumber, and runs
again when the pages land.

The test kills the server with SIGKILL so there is no clean shutdown and the
WAL must actually be replayed, then asserts the hash reads back exactly what
was written.

PRECONDITION, asserted like every other test in this directory: the restarted
server must log at least one `FAULTLOG drain_page_stall`. Without it the replay
may have completed with everything resident, which exercises none of the new
code and would make a green result meaningless. Requires a Debug build (the
marker is a DLOG).

PREREQUISITE: use the S1 harness in `paged_single_node.sh --wal on`, which
starts a standalone external log service. `enable_wal = on` with only the
built-in single-node log service is not sufficient: killing EloqKV kills that
service too and turns this into a data-loss/harness test rather than WAL replay.
The asserted `drain_page_stall` marker below prevents either configuration from
being mistaken for successful paged replay.

Usage:
  python3 tests/unit_cc/replay_paged_restart.py <start_cmd_file> <log_dir>

<start_cmd_file> holds the server command line to (re)start, one shell line.
"""
import glob
import os
import socket
import subprocess
import sys
import time

sys.path.insert(0, "tests/unit_cc")
from swap_with_inflight_fetch import HOST, PORT, cmd, conn  # noqa: E402

NFIELDS = 900
POST_CKPT_WRITES = 300


def marker_count(log_dir, token):
    n = 0
    for path in glob.glob(os.path.join(log_dir, "eloqdb.log.INFO*")):
        try:
            with open(path, errors="ignore") as fh:
                n += sum(1 for line in fh if token in line)
        except OSError:
            pass
    return n


def wait_up(timeout=180):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.create_connection((HOST, PORT), timeout=2)
            s.close()
            return True
        except OSError:
            time.sleep(2)
    return False


def kill_server():
    """SIGKILL every eloqkv/host_manager process, by exe symlink.

    Matching on /proc/<pid>/exe rather than a pkill pattern: a pattern that
    appears in its own command line kills the calling shell too.
    """
    for d in glob.glob("/proc/[0-9]*"):
        try:
            exe = os.readlink(os.path.join(d, "exe"))
        except OSError:
            continue
        if exe.endswith("/eloqkv") or exe.endswith("/host_manager"):
            try:
                os.kill(int(os.path.basename(d)), 9)
            except OSError:
                pass


def build_and_write(key):
    """Write a paged hash, checkpoint it, then write MORE so the tail of the
    WAL contains commands whose pages are on disk and not in memory."""
    c = conn()
    cmd(c, "DEL", key)
    for base in range(0, NFIELDS, 150):
        out = b""
        for i in range(base, base + 150):
            args = ("HSET", key, "f%05d" % i, "x" * 220 + "%05d" % i)
            out += ("*%d\r\n" % len(args)).encode()
            for a in args:
                b = str(a).encode()
                out += b"$%d\r\n%s\r\n" % (len(b), b)
        c.sendall(out)
        # Drain all 150 replies, not "whatever one recv returns": an undrained
        # reply stream leaves bytes that corrupt any later accounting on this
        # socket.
        want = 4 * 150
        got = 0
        while got < want:
            chunk = c.recv(65536)
            if not chunk:
                raise OSError("connection closed mid-batch")
            got += len(chunk)

    # Let the checkpoint push the pages to the store: replayed commands must
    # land on an object whose pages are durable but NOT resident.
    time.sleep(12)

    # Post-checkpoint writes: these are the ones replay has to re-apply, so
    # every one of them MUST be durably acknowledged before the SIGKILL.
    #
    # Strictly one command per round trip, on a FRESH connection. Counting raw
    # reply bytes on the shared connection was wrong and produced a fake bug:
    # the batched phase above is not guaranteed to drain all of its replies, so
    # leftover bytes in the socket satisfied the byte count early, the kill
    # landed before most writes had committed, and the restart legitimately
    # replayed a single command. That looked exactly like the engine losing 299
    # acknowledged writes. Read each reply, assert it, then send the next.
    c.close()
    c = conn()
    for i in range(POST_CKPT_WRITES):
        reply = cmd(c, "HSET", key, "p%05d" % i, "y" * 220 + "%05d" % i)
        if not reply.startswith(b":"):
            raise OSError("HSET p%05d not acknowledged: %r" % (i, reply[:40]))
    c.close()


def verify(key):
    c = conn()
    ok = True
    hlen = cmd(c, "HLEN", key)
    expected_len = b":%d\r\n" % (NFIELDS + POST_CKPT_WRITES)
    if hlen != expected_len:
        print("  HLEN %r, expected %r" % (hlen, expected_len))
        ok = False
    # Spot-check both generations: pre-checkpoint fields and the replayed tail.
    for field, prefix in [("f00000", b"x"), ("f00899", b"x"),
                          ("p00000", b"y"),
                          ("p%05d" % (POST_CKPT_WRITES - 1), b"y")]:
        got = cmd(c, "HGET", key, field)
        if not got.startswith(b"$225\r\n" + prefix):
            print("  HGET %s -> %r" % (field, got[:32]))
            ok = False
    c.close()
    return ok


def main():
    start_cmd = open(sys.argv[1]).read().strip()
    log_dir = sys.argv[2]
    key = "replay:paged"

    if not wait_up():
        print("server not up at start", flush=True)
        return 1

    build_and_write(key)
    print("wrote %d + %d fields" % (NFIELDS, POST_CKPT_WRITES), flush=True)

    stalls_before = marker_count(log_dir, "FAULTLOG drain_page_stall")
    kill_server()
    time.sleep(4)

    # Capture the restart's stdout/stderr. Discarding it cost a whole run:
    # the server failed to start, wrote no glog at all, and there was nothing
    # to look at but "did not come back".
    restart_out = os.path.join(log_dir, "restart_stdout.log")
    with open(restart_out, "wb") as fh:
        subprocess.Popen(start_cmd, shell=True, start_new_session=True,
                         stdout=fh, stderr=subprocess.STDOUT)
    if not wait_up():
        print("server did not come back after SIGKILL; its output:", flush=True)
        try:
            with open(restart_out, errors="ignore") as fh:
                tail = fh.read()[-2000:]
            print(tail, flush=True)
        except OSError:
            pass
        return 1
    time.sleep(8)                        # let replay finish

    content_ok = verify(key)
    stalled = marker_count(log_dir, "FAULTLOG drain_page_stall") > stalls_before

    if not stalled:
        # Not a pass with a caveat: the replay may have run entirely on
        # resident pages, in which case this run says nothing about the drain's
        # page-fault path.
        print("RESULT: FAIL(vacuous: replay never page-faulted)", flush=True)
        return 1
    print("content_ok=%s drain_stalled=%s" % (content_ok, stalled), flush=True)
    print("RESULT:", "PASS" if content_ok else "FAIL", flush=True)
    return 0 if content_ok else 1


if __name__ == "__main__":
    sys.exit(main())
