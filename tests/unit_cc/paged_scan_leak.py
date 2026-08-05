"""Internal page rows must never surface through SCAN / KEYS / DBSIZE.

Page rows of a paged object live in the ordinary object table's keyspace
(docs/08-paged-objects.md §5), so any store scan sees them. They are NOT
Redis keys: their value's first byte is a page-layout version, not an object
type tag. Three concrete hazards if a scanner does not discard them:

  * `KEYS *` / `SCAN` return binary `\\x00EKVPAGE...` keys a client can feed
    back into other commands;
  * `SCAN TYPE list` returns hash PAGES, because page layout version 1 has
    the same byte value as the serialized List type tag;
  * they consume scan work and reply memory.

The leak only appears once an object has been CHECKPOINTED — before that the
pages exist in memory only and no store scan sees them. So the test writes a
paged object, waits out a checkpoint, and then scans. It also drops the
object's pages from memory first (a restart would do; here a shed is enough
to prove the rows are being read from the store).

Run against a server with --paged_hash_convert_threshold=1 and a short
--checkpointer_interval. Usage:
    python3 tests/unit_cc/paged_scan_leak.py [port] [ckpt_wait_s]
"""
import sys
import time

sys.path.insert(0, "tests/unit_cc")
from standby_paged_apply import conn, read_reply, send  # noqa: E402

MAGIC = b"\x00EKVPAGE"


def read_array(sock):
    """Read one reply; return (header, [elements]) with nesting flattened."""
    buf = b""

    def more():
        nonlocal buf
        chunk = sock.recv(65536)
        if not chunk:
            raise OSError("connection closed")
        buf += chunk

    def line():
        nonlocal buf
        while b"\r\n" not in buf:
            more()
        ln, buf = buf.split(b"\r\n", 1)
        return ln

    def bulk(n):
        nonlocal buf
        while len(buf) < n + 2:
            more()
        s, buf = buf[:n], buf[n + 2:]
        return s

    head = line()
    out = []
    if not head.startswith(b"*"):
        return head, out
    for _ in range(max(0, int(head[1:]))):
        el = line()
        if el.startswith(b"$"):
            n = int(el[1:])
            out.append(bulk(n) if n >= 0 else None)
        elif el.startswith(b"*"):
            for _ in range(max(0, int(el[1:]))):
                el2 = line()
                if el2.startswith(b"$"):
                    n2 = int(el2[1:])
                    out.append(bulk(n2) if n2 >= 0 else None)
        else:
            out.append(el)
    return head, out


def scan_all(sock, extra=()):
    """Full SCAN loop; returns every key returned across all cursors."""
    keys = []
    cursor = b"0"
    for _ in range(10000):
        send(sock, b"SCAN", cursor, b"COUNT", b"100", *extra)
        head, flat = read_array(sock)
        if not flat:
            break
        cursor = flat[0]
        keys.extend(flat[1:])
        if cursor == b"0":
            break
    return keys


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 7399
    ckpt_wait = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    s = conn(port)

    send(s, b"FLUSHALL")
    read_reply(s)
    # A paged hash with enough fields to occupy many pages, plus a couple of
    # ordinary keys the scan MUST still return.
    for i in range(400):
        send(s, b"HSET", b"bighash", b"f%04d" % i, b"v" * 200)
        if read_reply(s)[:1] != b":":
            raise OSError("HSET not acknowledged")
    send(s, b"SET", b"plainkey", b"1")
    read_reply(s)
    send(s, b"HSET", b"smallhash", b"f", b"v")
    read_reply(s)

    print("waiting %ds for a checkpoint to write page rows" % ckpt_wait,
          flush=True)
    time.sleep(ckpt_wait)
    # Drop pages from memory so scans must read the store.
    send(s, b"fault_inject", b"shed_all_pages", b"-1")
    read_reply(s)
    send(s, b"fault_inject", b"force_shard_clean_now", b"-1")
    read_reply(s)
    time.sleep(2)

    bad = []

    # --- KEYS * ---
    send(s, b"KEYS", b"*")
    _, keys = read_array(s)
    leaked = [k for k in keys if k and k.startswith(MAGIC)]
    print("KEYS *: %d keys, %d leaked page rows" % (len(keys), len(leaked)),
          flush=True)
    if leaked:
        bad.append(("KEYS *", leaked[:2]))
    for want in (b"bighash", b"plainkey", b"smallhash"):
        if want not in keys:
            bad.append(("KEYS * missing", want))

    # --- SCAN, full cursor loop ---
    skeys = scan_all(s)
    sleaked = [k for k in skeys if k and k.startswith(MAGIC)]
    print("SCAN: %d keys, %d leaked page rows" % (len(skeys), len(sleaked)),
          flush=True)
    if sleaked:
        bad.append(("SCAN", sleaked[:2]))
    for want in (b"bighash", b"plainkey", b"smallhash"):
        if want not in skeys:
            bad.append(("SCAN missing", want))

    # --- SCAN TYPE list: page layout version 1 == the List type tag ---
    lkeys = scan_all(s, extra=(b"TYPE", b"list"))
    print("SCAN TYPE list: %d keys returned (expect 0)" % len(lkeys),
          flush=True)
    if lkeys:
        bad.append(("SCAN TYPE list", lkeys[:2]))

    # --- SCAN TYPE hash still finds the real hashes ---
    hkeys = scan_all(s, extra=(b"TYPE", b"hash"))
    hleaked = [k for k in hkeys if k and k.startswith(MAGIC)]
    print("SCAN TYPE hash: %d keys, %d leaked" % (len(hkeys), len(hleaked)),
          flush=True)
    if hleaked:
        bad.append(("SCAN TYPE hash", hleaked[:2]))
    for want in (b"bighash", b"smallhash"):
        if want not in hkeys:
            bad.append(("SCAN TYPE hash missing", want))

    # --- the object is still intact and readable ---
    send(s, b"HLEN", b"bighash")
    hlen = read_reply(s)
    print("HLEN bighash -> %r (expect :400)" % hlen, flush=True)
    if hlen != b":400":
        bad.append(("HLEN", hlen))

    if bad:
        for label, detail in bad:
            print("  FAIL: %s -> %r" % (label, detail), flush=True)
        print("RESULT: FAIL", flush=True)
        return 1
    print("RESULT: PASS", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
