#!/usr/bin/env python3
"""netpad.py - drive esp32-netpad over its TCP command port.

  python3 tools/netpad.py status
  python3 tools/netpad.py press a --hold 80 --count 2
  python3 tools/netpad.py press a+up
  python3 tools/netpad.py type "hello" --enter
  python3 tools/netpad.py macro
  python3 tools/netpad.py stop
  python3 tools/netpad.py raw "STATUS"

Host defaults to $NETPAD_HOST, then netpad.local, then 192.168.4.1 (setup AP).
"""
from __future__ import annotations

import argparse
import os
import socket
import sys

PORT = int(os.environ.get("NETPAD_PORT", "9060"))
TIMEOUT = float(os.environ.get("NETPAD_TIMEOUT", "10"))


def send(host: str, line: str) -> str:
    s = socket.create_connection((host, PORT), timeout=5)
    try:
        s.sendall((line + "\n").encode())
        s.settimeout(TIMEOUT)
        data = b""
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
            if data.rstrip().endswith(b"END"):
                break
    finally:
        s.close()
    return data.decode("utf-8", "replace")


def escape_text(text: str) -> str:
    return text.replace("\\", "\\\\").replace("\n", "\\n")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host",
                    default=os.environ.get("NETPAD_HOST", "192.168.4.1"))
    ap.add_argument("--quiet", action="store_true")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("status")
    sub.add_parser("stop")
    sub.add_parser("macro")

    p_press = sub.add_parser("press")
    p_press.add_argument("buttons", help="e.g. a, a+up, l+r, home")
    p_press.add_argument("--hold", type=int, default=80)
    p_press.add_argument("--count", type=int, default=1)

    p_type = sub.add_parser("type")
    p_type.add_argument("text")
    p_type.add_argument("--enter", action="store_true")

    p_raw = sub.add_parser("raw")
    p_raw.add_argument("line")

    a = ap.parse_args()

    if a.cmd == "status":
        line = "STATUS"
    elif a.cmd == "stop":
        line = "STOP"
    elif a.cmd == "macro":
        line = "BOOTSTRAP"
    elif a.cmd == "press":
        line = "PRESS %s %d %d" % (a.buttons, a.hold, a.count)
    elif a.cmd == "type":
        line = "TYPE %d %s" % (1 if a.enter else 0, escape_text(a.text))
    else:
        line = a.line

    try:
        out = send(a.host, line)
    except OSError as e:
        print("connect failed: %s" % e, file=sys.stderr)
        return 2

    if not a.quiet:
        print(out, end="" if out.endswith("\n") else "\n")
    return 0 if "OK" in out else 1


if __name__ == "__main__":
    sys.exit(main())
