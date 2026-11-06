#!/usr/bin/env python3
"""Build the two flash blobs the ESP32 needs for the one-shot installer.

  blob_tpl.bin    u32 LE length + base64(save_template.bin)
  blob_apply.bin  u32 LE length + the installer JS (STAGER + stage hex baked in)

Usage: python3 tools/make_install_blobs.py [core.js] [outdir]
"""
import base64
import importlib.util
import io
import json
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CIV = os.path.dirname(ROOT)
INST_DIR = os.path.join(CIV, "installer-creator")


def load_install_module():
    spec = importlib.util.spec_from_file_location("civinst", os.path.join(INST_DIR, "install.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    core_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(CIV, "core.js")
    outdir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "..", "blobs")
    os.makedirs(outdir, exist_ok=True)

    inst = load_install_module()

    core = io.open(core_path, encoding="utf-8").read().replace("\r\n", "\n")
    wrapped = ("(async function() {\ntry{\n" + core +
               "\n} catch (e) {\nawait log('EXCEPTION: ' + e.message);\nawait log(e.stack);\n}\n})();")

    tmp = tempfile.mkdtemp(prefix="civblob_")
    wrapped_path = os.path.join(tmp, "wrapped.js")
    io.open(wrapped_path, "w", encoding="utf-8", newline="\n").write(wrapped)
    hex_path = os.path.join(tmp, "stage.hex")
    r = subprocess.run(["node", os.path.join(INST_DIR, "compress_stage.js"), wrapped_path, hex_path],
                       capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode != 0:
        print(r.stderr, file=sys.stderr)
        return 2
    stage_hex = io.open(hex_path, encoding="utf-8").read().strip()

    tpl = open(os.path.join(INST_DIR, "save_template.bin"), "rb").read()
    if len(tpl) < 4096 or tpl[:4] != b"CIV7":
        print("E: bad save_template.bin", file=sys.stderr)
        return 3
    tpl_b64 = base64.b64encode(tpl)

    apply_js = (inst.BLOCK
                .replace("__STAGER__", json.dumps(inst.STAGER))
                .replace("__STAGE_HEX__", json.dumps(stage_hex))
                .replace("__TPL_B64_LEN__", str(len(tpl_b64)))).encode()

    with open(os.path.join(outdir, "blob_tpl.bin"), "wb") as f:
        f.write(struct.pack("<I", len(tpl_b64)))
        f.write(tpl_b64)
    apply_b64 = base64.b64encode(apply_js)
    with open(os.path.join(outdir, "blob_apply.bin"), "wb") as f:
        f.write(struct.pack("<I", len(apply_b64)))
        f.write(apply_b64)

    print("template %d bytes -> b64 %d" % (len(tpl), len(tpl_b64)))
    print("apply js %d bytes -> b64 %d, stage hex %d chars" % (len(apply_js), len(apply_b64), len(stage_hex)))
    print("wrote", outdir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
