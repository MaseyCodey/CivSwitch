#!/usr/bin/env python3
"""serial_log.py - append the ESP32 console to a file, forever.

  python3 tools/serial_log.py /dev/cu.usbmodem2101 /tmp/netpad.log

Reconnects when the port disappears (USB-JTAG re-enumerates on reset), so it can
be left running in the background while the board reboots.
"""
from __future__ import annotations

import glob
import os
import select
import sys
import termios
import time


def pick(port: str) -> str | None:
    if port and os.path.exists(port):
        return port
    cands = sorted(glob.glob("/dev/cu.usbmodem*"))
    return cands[0] if cands else None


def read_forever(port: str, out: str, baud: int) -> None:
    f = open(out, "ab", buffering=0)
    current = None
    fd = -1
    while True:
        if current is None:
            current = pick(port)
            if current is None:
                time.sleep(0.3)
                continue
            try:
                fd = os.open(current, os.O_RDONLY | os.O_NONBLOCK)
            except OSError:
                current = None
                time.sleep(0.3)
                continue
            a = termios.tcgetattr(fd)
            a[0] = 0
            a[1] = 0
            a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
            a[3] = 0
            a[4] = baud
            a[5] = baud
            termios.tcsetattr(fd, termios.TCSANOW, a)
            f.write(("\n--- attached %s %s ---\n" % (current, time.strftime("%H:%M:%S"))).encode())
        r, _, _ = select.select([fd], [], [], 0.5)
        if not r:
            if not os.path.exists(current):
                try:
                    os.close(fd)
                except OSError:
                    pass
                fd = -1
                current = None
            continue
        try:
            data = os.read(fd, 8192)
        except OSError:
            data = b""
        if not data:
            try:
                os.close(fd)
            except OSError:
                pass
            fd = -1
            current = None
            continue
        f.write(data)


def main() -> int:
    port = sys.argv[1] if len(sys.argv) > 1 else ""
    out = sys.argv[2] if len(sys.argv) > 2 else "/tmp/netpad.log"
    baud = int(sys.argv[3]) if len(sys.argv) > 3 else termios.B115200
    read_forever(port, out, baud)
    return 0


if __name__ == "__main__":
    sys.exit(main())
