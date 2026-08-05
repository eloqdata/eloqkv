"""Print the one candidate EloqKV port that currently accepts writes."""

import sys
import time

from paged_testlib import RespClient, RespError


def main() -> int:
    ports = [int(value) for value in sys.argv[1:]] or [7401, 7501]
    deadline = time.monotonic() + 180
    while time.monotonic() < deadline:
        for port in ports:
            try:
                with RespClient(port=port, timeout=3) as client:
                    reply = client.command(b"SET", b"paged:leader-probe", b"1")
                if reply == b"OK":
                    print(port)
                    return 0
            except OSError:
                pass
        time.sleep(2)
    print("no writable EloqKV node among %r" % ports, file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
