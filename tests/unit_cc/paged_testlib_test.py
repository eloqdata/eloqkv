#!/usr/bin/env python3
"""Focused tests for the shared RESP reader's failure semantics."""

import socket
import time
import unittest

from paged_testlib import RespClient, RespError


class ScriptedSocket:
    """Minimal socket interface with deterministic delayed recv chunks."""

    def __init__(self, chunks):
        self.chunks = list(chunks)
        self.timeout = None

    def gettimeout(self):
        return self.timeout

    def settimeout(self, timeout):
        self.timeout = timeout

    def recv(self, _size):
        if not self.chunks:
            return b""
        delay, chunk = self.chunks.pop(0)
        if self.timeout is not None and delay > self.timeout:
            time.sleep(self.timeout)
            raise socket.timeout("scripted recv deadline")
        time.sleep(delay)
        return chunk

    def close(self):
        pass


def scripted_client(*chunks):
    """Construct a RespClient without opening a real socket."""
    client = object.__new__(RespClient)
    client.sock = ScriptedSocket(chunks)
    client._buf = bytearray()
    return client


class RespClientTest(unittest.TestCase):
    def test_complete_recursive_reply(self):
        client = scripted_client(
            (0, b"*3\r\n$3\r\nfoo\r\n:7\r\n-ERR bad\r\n"))
        self.assertEqual(client.read_deadline(1),
                         [b"foo", 7, RespError(b"ERR bad")])

    def test_eof_is_an_error_not_a_busy_loop(self):
        client = scripted_client((0, b"*1\r\n$5\r\nab"), (0, b""))
        with self.assertRaisesRegex(OSError, "connection closed"):
            client.read_deadline(1)

    def test_deadline_is_for_the_whole_reply(self):
        client = scripted_client(*[(0.03, chunk)
                                   for chunk in (b"+", b"O", b"K",
                                                 b"\r", b"\n")])
        started = time.monotonic()
        with self.assertRaises((socket.timeout, TimeoutError)):
            client.read_deadline(0.07)
        self.assertLess(time.monotonic() - started, 0.2)


if __name__ == "__main__":
    unittest.main()
