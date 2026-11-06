#!/usr/bin/env python3
"""Build main/chat_payload.h from the shared CivSwitch/core.js.

The chat payload is core.js + the UDP installer channel (tools/udp_loader.js),
compressed with PicoSwitch/compress.js at MAX=1000. Regenerating picks up
upstream core.js changes automatically.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                 # CivSwitch/esp32-ldn
CIV = os.path.dirname(ROOT)                  # CivSwitch
DEFAULT_CORE = os.path.join(CIV, "core.js")
SNIPPET = os.path.join(HERE, "udp_loader.js")
COMPRESS = os.path.join(CIV, "PicoSwitch", "compress.js")
NODE_MODULES = os.path.join(CIV, "PicoSwitch", "node_modules")
OUT = os.path.join(ROOT, "main", "chat_payload.h")

ENTRY = "        remote_js_loader();"


def c_escape(s: str) -> str:
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "?":
            out.append("\\?")
        else:
            out.append(ch)
    return "".join(out)


def main() -> int:
    core_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CORE
    core = open(core_path, encoding="utf-8").read()
    if ENTRY not in core:
        print("E: entry point %r not found in %s" % (ENTRY, core_path), file=sys.stderr)
        return 1
    snippet = open(SNIPPET, encoding="utf-8").read()
    combined = core.replace(ENTRY, snippet)

    tmp = tempfile.mkdtemp(prefix="civ7payload_")
    try:
        pico = os.path.join(tmp, "PicoSwitch")
        os.makedirs(pico)
        with open(os.path.join(tmp, "core.js"), "w", encoding="utf-8") as f:
            f.write(combined)
        script = os.path.join(pico, "compress.js")
        text = open(COMPRESS, encoding="utf-8").read().replace("const MAX = 254;", "const MAX = 1000;")
        with open(script, "w", encoding="utf-8") as f:
            f.write(text)
        if os.path.isdir(NODE_MODULES):
            os.symlink(NODE_MODULES, os.path.join(pico, "node_modules"))
        r = subprocess.run(["node", "compress.js"], cwd=pico, capture_output=True, text=True)
        print(r.stdout.strip())
        if r.returncode != 0:
            print(r.stderr, file=sys.stderr)
            return 2
        lines = [l for l in open(os.path.join(pico, "payload.txt"), encoding="utf-8").read().splitlines() if l.strip()]
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    with open(OUT, "w", encoding="utf-8") as f:
        f.write("/* Generated from core.js + udp_loader.js - DO NOT COMMIT. */\n")
        f.write("#ifndef CHAT_PAYLOAD_H\n#define CHAT_PAYLOAD_H\n\n")
        f.write("#define CHAT_LINE_COUNT %d\n\n" % len(lines))
        f.write("static const char *const CHAT_LINES[CHAT_LINE_COUNT] = {\n")
        for line in lines:
            f.write('    "%s",\n' % c_escape(line))
        f.write("};\n\n#endif\n")
    print("wrote %s (%d chat lines, max %d chars)" % (OUT, len(lines), max(len(l) for l in lines)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
