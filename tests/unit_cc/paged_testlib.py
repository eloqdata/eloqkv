"""Dependency-free RESP and paged-object integration-test helpers.

The older paged tests each grew a partial RESP reader.  Several of those
readers return only an aggregate header, which makes them unsuitable as a
semantic oracle and has previously allowed undrained replies to corrupt the
next assertion.  New tests use this module so every reply is recursively
decoded and every fault scenario can prove that its injector actually fired.
"""

from __future__ import annotations

import glob
import os
import socket
import time
from dataclasses import dataclass
from typing import Callable, Iterable, Sequence


@dataclass(frozen=True)
class RespError:
    """A server error reply, retained as data instead of raised implicitly."""

    message: bytes

    def startswith(self, prefix: bytes) -> bool:
        return self.message.startswith(prefix)


class RespClient:
    """A binary-safe RESP2 client with complete recursive reply draining."""

    def __init__(self, host: str = "127.0.0.1", port: int = 7399,
                 timeout: float = 90.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._buf = bytearray()

    def close(self) -> None:
        self.sock.close()

    def __enter__(self) -> "RespClient":
        return self

    def __exit__(self, *_args) -> None:
        self.close()

    @staticmethod
    def encode(args: Sequence[object]) -> bytes:
        out = [b"*%d\r\n" % len(args)]
        for arg in args:
            if isinstance(arg, bytes):
                raw = arg
            elif isinstance(arg, bytearray):
                raw = bytes(arg)
            else:
                raw = str(arg).encode()
            out.extend((b"$%d\r\n" % len(raw), raw, b"\r\n"))
        return b"".join(out)

    def send(self, *args: object) -> None:
        self.sock.sendall(self.encode(args))

    @staticmethod
    def _remaining(deadline: float | None) -> float | None:
        if deadline is None:
            return None
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("RESP command exceeded its wall-clock deadline")
        return remaining

    def _fill(self, n: int = 1, deadline: float | None = None) -> None:
        while len(self._buf) < n:
            remaining = self._remaining(deadline)
            if remaining is not None:
                # socket timeouts are per recv(). Resetting to the remaining
                # wall-clock budget prevents a peer that trickles bytes from
                # extending one command indefinitely.
                self.sock.settimeout(remaining)
            chunk = self.sock.recv(65536)
            if not chunk:
                raise OSError("connection closed while reading RESP reply")
            self._buf.extend(chunk)

    def _line(self, deadline: float | None = None) -> bytes:
        while True:
            at = self._buf.find(b"\r\n")
            if at >= 0:
                line = bytes(self._buf[:at])
                del self._buf[:at + 2]
                return line
            self._fill(len(self._buf) + 1, deadline)

    def _bytes(self, n: int, deadline: float | None = None) -> bytes:
        self._fill(n + 2, deadline)
        if self._buf[n:n + 2] != b"\r\n":
            raise AssertionError("malformed bulk reply terminator")
        value = bytes(self._buf[:n])
        del self._buf[:n + 2]
        return value

    def _read(self, deadline: float | None = None):
        self._fill(deadline=deadline)
        prefix = bytes(self._buf[:1])
        del self._buf[:1]
        if prefix == b"+":
            return self._line(deadline)
        if prefix == b"-":
            return RespError(self._line(deadline))
        if prefix == b":":
            return int(self._line(deadline))
        if prefix == b"$":
            n = int(self._line(deadline))
            return None if n < 0 else self._bytes(n, deadline)
        if prefix in (b"*", b"~"):
            n = int(self._line(deadline))
            return None if n < 0 else [self._read(deadline) for _ in range(n)]
        if prefix == b"%":
            n = int(self._line(deadline))
            return {self._read(deadline): self._read(deadline)
                    for _ in range(n)}
        if prefix == b"_":
            if self._line(deadline):
                raise AssertionError("malformed RESP3 null")
            return None
        if prefix == b"#":
            value = self._line(deadline)
            if value not in (b"t", b"f"):
                raise AssertionError("malformed RESP3 boolean")
            return value == b"t"
        if prefix in (b",", b"("):
            return self._line(deadline)
        raise AssertionError("unknown RESP prefix %r" % prefix)

    def read(self):
        return self._read()

    def read_deadline(self, timeout: float):
        """Read one complete reply within one wall-clock timeout.

        A timeout leaves an indeterminate partial reply buffered, so callers
        must close this client rather than issue another command on it.
        """
        old_timeout = self.sock.gettimeout()
        try:
            return self._read(time.monotonic() + timeout)
        finally:
            self.sock.settimeout(old_timeout)

    def command(self, *args: object):
        self.send(*args)
        return self.read()

    def command_deadline(self, timeout: float, *args: object):
        self.send(*args)
        return self.read_deadline(timeout)

    def pipeline(self, commands: Iterable[Sequence[object]]):
        commands = list(commands)
        self.sock.sendall(b"".join(self.encode(c) for c in commands))
        return [self.read() for _ in commands]


def require_equal(got, want, label: str) -> None:
    if got != want:
        raise AssertionError("%s: got %r, want %r" % (label, got, want))


def require_error(reply, label: str, contains: bytes | None = None) -> None:
    if not isinstance(reply, RespError):
        raise AssertionError("%s: got %r, want an error reply" % (label, reply))
    if contains is not None and contains.lower() not in reply.message.lower():
        raise AssertionError("%s: error %r lacks %r" %
                             (label, reply.message, contains))


def wait_until(predicate: Callable[[], object], timeout: float,
               interval: float = 0.1, label: str = "condition"):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = predicate()
        if last:
            return last
        time.sleep(interval)
    raise TimeoutError("timed out waiting for %s (last=%r)" % (label, last))


def marker_count(log_dir: str, marker: str) -> int:
    """Count a marker without double-counting glog's current-file symlinks."""
    seen = set()
    count = 0
    for path in glob.glob(os.path.join(log_dir, "eloqdb.log.*")):
        try:
            real = os.path.realpath(path)
            if real in seen or not os.path.isfile(real):
                continue
            seen.add(real)
            with open(real, "r", errors="replace") as stream:
                count += sum(marker in line for line in stream)
        except OSError:
            pass
    return count


def arm_fault(client: RespClient, name: str) -> None:
    reply = client.command("fault_inject", name, -1)
    if isinstance(reply, RespError):
        raise AssertionError("failed to arm %s: %r" % (name, reply.message))


def disarm_fault(client: RespClient, name: str) -> None:
    reply = client.command("fault_inject", name, -1, "remove")
    if isinstance(reply, RespError):
        raise AssertionError("failed to disarm %s: %r" % (name, reply.message))


def shed_all_pages(client: RespClient, log_dir: str, timeout: float = 15.0) -> int:
    """Force a clean pass and prove that at least one page was shed."""
    marker = "FAULTLOG shed_all_pages"
    before = marker_count(log_dir, marker)
    arm_fault(client, "shed_all_pages")
    arm_fault(client, "force_shard_clean_now")
    try:
        return wait_until(lambda: marker_count(log_dir, marker) - before,
                          timeout, 0.2, "a page-shed marker")
    finally:
        disarm_fault(client, "force_shard_clean_now")
        disarm_fault(client, "shed_all_pages")


def hscan_all(client: RespClient, key: bytes, count: int = 7,
              novalues: bool = False):
    cursor = b"0"
    out = []
    for _ in range(100000):
        args = [b"HSCAN", key, cursor, b"COUNT", str(count).encode()]
        if novalues:
            args.append(b"NOVALUES")
        reply = client.command(*args)
        if (not isinstance(reply, list) or len(reply) != 2 or
                not isinstance(reply[1], list)):
            raise AssertionError("malformed HSCAN reply: %r" % (reply,))
        cursor = reply[0]
        out.extend(reply[1])
        if cursor == b"0":
            return out
    raise AssertionError("HSCAN did not terminate")


def scan_all(client: RespClient, *options: bytes):
    cursor = b"0"
    keys = []
    for _ in range(100000):
        reply = client.command(b"SCAN", cursor, b"COUNT", b"100", *options)
        if (not isinstance(reply, list) or len(reply) != 2 or
                not isinstance(reply[1], list)):
            raise AssertionError("malformed SCAN reply: %r" % (reply,))
        cursor = reply[0]
        keys.extend(reply[1])
        if cursor == b"0":
            return keys
    raise AssertionError("SCAN did not terminate")
