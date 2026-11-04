#!/usr/bin/env python3
"""ota.py - push a firmware image to esp32-netpad over Wi-Fi.

  python3 tools/ota.py 10.2.0.240 build/esp32-netpad.bin

The board writes the image into the inactive OTA slot, then reboots. If the new
image does not boot (or never calls esp_ota_mark_app_valid_cancel_rollback), the
bootloader rolls back to the previous slot.
"""
from __future__ import annotations

import http.client
import os
import sys
import time

DEFAULT_HOST = os.environ.get("NETPAD_HOST", "192.168.4.1")
CHUNK = 16384


def upload(host: str, path: str, port: int = 80, timeout: float = 60.0) -> str:
    size = os.path.getsize(path)
    print("uploading %s (%d bytes) to %s:%d ..." % (path, size, host, port))
    conn = http.client.HTTPConnection(host, port, timeout=timeout)
    conn.putrequest("POST", "/ota")
    conn.putheader("Content-Type", "application/octet-stream")
    conn.putheader("Content-Length", str(size))
    conn.endheaders()
    sent = 0
    t0 = time.time()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(CHUNK)
            if not chunk:
                break
            conn.send(chunk)
            sent += len(chunk)
            pct = sent * 100 // size
            print("\r  %3d%%  %d/%d KB" % (pct, sent // 1024, size // 1024), end="", flush=True)
    print("\r  100%%  %d KB  (%.1fs)" % (size // 1024, time.time() - t0))
    resp = conn.getresponse()
    body = resp.read().decode("utf-8", "replace")
    conn.close()
    print("HTTP %d: %s" % (resp.status, body.strip()))
    return body


def wait_online(host: str, want_slot: str | None = None, timeout: float = 40.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            conn = http.client.HTTPConnection(host, 80, timeout=3)
            conn.request("GET", "/status")
            r = conn.getresponse()
            body = r.read().decode()
            conn.close()
            if r.status == 200:
                if want_slot and want_slot not in body:
                    print("online, waiting for slot %s: %s" % (want_slot, body))
                else:
                    print("online:", body)
                    return True
        except OSError:
            pass
        time.sleep(1.0)
    return False


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    host = DEFAULT_HOST
    path = sys.argv[1]
    if len(sys.argv) > 2:
        host, path = sys.argv[1], sys.argv[2]
    if not os.path.exists(path):
        print("no such file: %s" % path, file=sys.stderr)
        return 1
    body = ""
    for attempt in range(1, 3):
        try:
            body = upload(host, path)
            if "OK" in body:
                break
        except OSError as e:
            print("upload attempt %d failed: %s" % (attempt, e), file=sys.stderr)
        if attempt < 2:
            print("retrying in 2s ...")
            time.sleep(2)
    if "OK" not in body:
        return 1
    return 0 if wait_online(host) else 2


if __name__ == "__main__":
    sys.exit(main())
