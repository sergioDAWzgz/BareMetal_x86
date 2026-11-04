#!/usr/bin/env python3
"""Injects keystrokes into a running QEMU through its monitor socket.

QEMU's `sendkey` feeds the emulated 8042 controller, so the guest receives a
real IRQ1 and walks the same path a physical PS/2 keyboard would. test.sh uses
this to prove the keyboard driver end to end instead of only testing the
translation table.

The caller owns QEMU; with --quit this script asks the monitor to shut it down.
"""

import argparse
import socket
import time


def connect(path, timeout):
    deadline = time.time() + timeout
    last_error = None

    while time.time() < deadline:
        try:
            connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            connection.connect(path)
            return connection
        except OSError as error:
            last_error = error
            time.sleep(0.1)

    raise SystemExit("send-keys: cannot connect to %s: %s" % (path, last_error))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("socket", help="QEMU monitor unix socket")
    parser.add_argument("keys", nargs="+", help="QEMU key names, for example h ret")
    parser.add_argument("--settle", type=float, default=5.0,
                        help="seconds to wait for the guest to reach its input loop")
    parser.add_argument("--gap", type=float, default=0.15,
                        help="seconds between keystrokes")
    parser.add_argument("--quit", action="store_true",
                        help="ask the monitor to quit after the last keystroke")
    parser.add_argument("--screendump", metavar="PATH",
                        help="write a QEMU screendump of the display before quitting")
    args = parser.parse_args()

    connection = connect(args.socket, timeout=20.0)
    time.sleep(args.settle)

    for key in args.keys:
        connection.sendall(("sendkey %s\n" % key).encode("ascii"))
        time.sleep(args.gap)

    if args.screendump:
        time.sleep(0.5)
        connection.sendall(("screendump %s\n" % args.screendump).encode("ascii"))
        time.sleep(0.5)

    if args.quit:
        time.sleep(0.5)
        connection.sendall(b"quit\n")

    connection.close()


if __name__ == "__main__":
    main()
