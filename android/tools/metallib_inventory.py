#!/usr/bin/env python3
"""What macOS's GPU work needs translated: an inventory of a shader capture (hengevm-android --gpu capture).

For each captured .metallib: its functions (name, vertex/fragment/kernel), their AIR bitcode, and how
often the pipelines that use them are bound (from index.tsv). The Vulkan backend translates shaders by
this table, most-bound first: the few that draw the desktop (WindowServer, Core Animation) matter most.

usage: metallib_inventory.py SHADER_DIR [--extract OUT]   (OUT gets one .bc per function, for llvm-dis)
"""
import glob
import os
import struct
import sys

TYPES = {0: "vertex", 1: "fragment", 2: "kernel", 3: "unqualified", 4: "visible", 5: "extern", 6: "intersection",
         7: "mesh", 8: "object"}
BC_WRAPPER, BC_RAW = b"\xde\xc0\x17\x0b", b"BC\xc0\xde"


def functions(data):
    """[(name, type, bitcode bytes)] from a metallib, or by scanning for bitcode if the layout is unknown."""
    out = []
    if data[:4] == b"MTLB" and len(data) >= 88:
        try:
            fl_off, = struct.unpack_from("<Q", data, 24)
            bc_off, bc_size = struct.unpack_from("<QQ", data, 72)
            count, = struct.unpack_from("<I", data, fl_off)
            at = fl_off + 4
            for _ in range(count):
                group, = struct.unpack_from("<I", data, at)
                end, t = at + group, at + 4
                name, kind, offt, size = "?", None, None, None
                while t + 4 <= end:
                    tag = data[t:t + 4]
                    if tag == b"ENDT":
                        break
                    n, = struct.unpack_from("<H", data, t + 4)
                    v = data[t + 6:t + 6 + n]
                    if tag == b"NAME":
                        name = v.split(b"\0")[0].decode(errors="replace")
                    elif tag == b"TYPE" and v:
                        kind = TYPES.get(v[0], str(v[0]))
                    elif tag == b"OFFT" and n >= 24:
                        offt = struct.unpack_from("<QQQ", v)[2]
                    elif tag == b"MDSZ" and n >= 8:
                        size, = struct.unpack_from("<Q", v)
                    t += 6 + n
                bc = data[bc_off + offt:bc_off + offt + size] if offt is not None and size else b""
                out.append((name, kind or "?", bc))
                at = end
            if out:
                return out
        except struct.error:
            pass
    i = 0
    while True:  # unknown layout: every bitcode module in it
        i = min([j for j in (data.find(BC_WRAPPER, i), data.find(BC_RAW, i)) if j >= 0], default=-1)
        if i < 0:
            return out
        if data[i:i + 4] == BC_WRAPPER and i + 20 <= len(data):
            off, size = struct.unpack_from("<II", data, i + 8)
            out.append(("?", "?", data[i:i + off + size]))
            i += max(off + size, 4)
        else:
            out.append(("?", "?", data[i:]))
            return out


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    root = sys.argv[1]
    extract = sys.argv[sys.argv.index("--extract") + 1] if "--extract" in sys.argv else None
    if extract:
        os.makedirs(extract, exist_ok=True)
    binds = {}
    if os.path.exists(os.path.join(root, "index.tsv")):
        for line in open(os.path.join(root, "index.tsv")):
            if line.startswith("pipe-"):
                k, n = line.split()
                binds[k] = int(n)
    libs = sorted(glob.glob(os.path.join(root, "lib-*.metallib")))
    pipes = glob.glob(os.path.join(root, "pipe-*"))
    names = {}
    for lib in libs:
        data = open(lib, "rb").read()
        for name, kind, bc in functions(data):
            names.setdefault((name, kind), []).append((os.path.basename(lib), len(bc)))
            if extract and bc:
                safe = "".join(c if c.isalnum() or c in "_-" else "_" for c in name)
                open(os.path.join(extract, f"{safe}-{os.path.basename(lib)[4:20]}.bc"), "wb").write(bc)
    # A pipeline names its functions as strings in its serialized data: count binds per function name.
    used = {}
    for p in pipes:
        blob, key = open(p, "rb").read(), os.path.basename(p).split(".")[0]
        for (name, _) in names:
            if name != "?" and name.encode() in blob:
                used[name] = used.get(name, 0) + binds.get(key, 0)
    print(f"{len(libs)} libraries, {len(names)} distinct functions, {len(pipes)} pipelines, "
          f"{sum(binds.values())} pipeline binds")
    print(f"{'binds':>8}  {'kind':<9} {'bitcode':>8}  function")
    for (name, kind), where in sorted(names.items(), key=lambda kv: (-used.get(kv[0][0], 0), kv[0][0])):
        print(f"{used.get(name, 0):>8}  {kind:<9} {max(s for _, s in where):>8}  {name}")


if __name__ == "__main__":
    main()
