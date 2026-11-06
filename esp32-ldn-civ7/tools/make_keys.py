#!/usr/bin/env python3
"""Generate main/keys.h from a prod.keys file. The output is gitignored."""
from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "main", "keys.h")

NEEDED = [
    "aes_kek_generation_source",
    "aes_key_generation_source",
    "master_key_00",
    "master_key_12",
]


def load(path: str) -> dict[str, bytes]:
    keys: dict[str, bytes] = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            name, value = line.split("=", 1)
            name = name.strip()
            value = value.strip().replace(" ", "")
            try:
                keys[name] = bytes.fromhex(value)
            except ValueError:
                continue
    return keys


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "prod.keys")
    keys = load(path)
    missing = [k for k in NEEDED if k not in keys]
    if missing:
        print("E: %s is missing: %s" % (path, ", ".join(missing)), file=sys.stderr)
        return 1
    for k in NEEDED:
        if len(keys[k]) != 16:
            print("E: %s must be 16 bytes" % k, file=sys.stderr)
            return 1

    with open(OUT, "w", encoding="utf-8") as f:
        f.write("/* Generated from prod.keys by tools/make_keys.py - DO NOT COMMIT. */\n")
        f.write("#ifndef KEYS_H\n#define KEYS_H\n\n#include <stdint.h>\n\n")
        names = {
            "aes_kek_generation_source": "KEK_GEN",
            "aes_key_generation_source": "KEY_GEN",
            "master_key_00": "MASTER_00",
            "master_key_12": "MASTER_12",
        }
        for src, dst in names.items():
            b = keys[src]
            f.write("static const uint8_t %s[16] = {%s};\n" % (
                dst, ",".join("0x%02x" % x for x in b)))
        f.write("\n#endif\n")
    print("wrote", OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
