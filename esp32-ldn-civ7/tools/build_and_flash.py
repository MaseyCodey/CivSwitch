#!/usr/bin/env python3
"""One-click build (and optional flash) for esp32-ldn-civ7.

  python3 tools/build_and_flash.py --keys prod.keys --port /dev/cu.usbmodemXXXX --flash

Steps:
  1. main/keys.h        from prod.keys        (make_keys.py)
  2. main/chat_payload.h from ../core.js       (make_payload.py)
  3. blobs/*.bin        from installer-creator (make_install_blobs.py)
  4. idf.py build
  5. esptool write-flash (bootloader, partition table, app, template, apply JS)

Requires the ESP-IDF environment to be sourced (idf.py on PATH) and node.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CIV = os.path.dirname(ROOT)
BUILD = os.path.join(ROOT, "build")
BLOBS = os.path.join(ROOT, "blobs")

APP_OFF = 0x10000
TPL_OFF = 0x110000
APPLY_OFF = 0x290000


def run(cmd, **kw):
    print("+", " ".join(cmd), flush=True)
    return subprocess.run(cmd, check=True, **kw)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--keys", default=os.path.join(ROOT, "prod.keys"),
                    help="path to prod.keys (required unless main/keys.h exists)")
    ap.add_argument("--core", default=os.path.join(CIV, "core.js"),
                    help="shared CivSwitch core.js")
    ap.add_argument("--port", default=os.environ.get("ESPPORT", ""),
                    help="serial port for flashing, e.g. /dev/cu.usbmodemXXXX")
    ap.add_argument("--flash", action="store_true", help="flash after building")
    ap.add_argument("--erase", action="store_true", help="erase the whole flash first")
    ap.add_argument("--no-build", action="store_true", help="skip idf.py build")
    args = ap.parse_args()

    py = sys.executable
    keys_h = os.path.join(ROOT, "main", "keys.h")
    if os.path.exists(args.keys):
        run([py, os.path.join(HERE, "make_keys.py"), args.keys])
    elif os.path.exists(keys_h):
        print("using existing main/keys.h")
    else:
        print("E: no prod.keys found (pass --keys)", file=sys.stderr)
        return 1

    run([py, os.path.join(HERE, "make_payload.py"), args.core])
    run([py, os.path.join(HERE, "make_install_blobs.py")])

    if not args.no_build:
        run(["idf.py", "build"], cwd=ROOT)

    if not args.flash:
        print("build only; pass --flash --port PORT to flash")
        return 0

    if not args.port:
        print("E: --port is required with --flash", file=sys.stderr)
        return 1

    esptool = [py, "-m", "esptool", "--chip", "esp32s3", "-p", args.port, "-b", "460800",
               "--before", "default-reset", "--after", "hard-reset"]
    if args.erase:
        run(esptool + ["erase-flash"])
    run(esptool + ["write-flash", "--flash-mode", "dio", "--flash-freq", "80m",
                   "--flash-size", "16MB",
                   "0x0", os.path.join(BUILD, "bootloader", "bootloader.bin"),
                   "0x8000", os.path.join(BUILD, "partition_table", "partition-table.bin"),
                   hex(APP_OFF), os.path.join(BUILD, "esp32-ldn-civ7.bin"),
                   hex(TPL_OFF), os.path.join(BLOBS, "blob_tpl.bin"),
                   hex(APPLY_OFF), os.path.join(BLOBS, "blob_apply.bin")])
    print("flashed. Press RESET on the board, then create the wireless lobby.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
