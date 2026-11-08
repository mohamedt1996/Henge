#!/usr/bin/env python3
"""Make a static arm64 glibc binary return -ENOSYS for syscalls Android's app seccomp filter kills.

Android traps (SIGSYS) syscalls that bionic does not use. glibc makes some of them where no handler
can run: set_robust_list at startup and in every new thread with all signals blocked, rseq likewise,
clone3 in pthread_create. Each such call site is `mov x8/w8, #nr ... svc #0`; this rewrites the svc to
`mov x0, #-38` (ENOSYS), which glibc already handles as "kernel too old".
usage: nosyscall.py BINARY   (patched in place)
"""
import struct
import sys

BLOCKED = {99: "set_robust_list", 293: "rseq", 435: "clone3"}
SVC0 = 0xD4000001
MOVN_X0_ENOSYS = 0x92800000 | (37 << 5)  # x0 = ~37 = -38


def x8_imm(insn):
    """The immediate if insn is movz w8/x8, #imm (no shift); None if it writes x8 some other way; -1 if not x8."""
    if insn & 0x1F != 8:
        return -1
    if insn & 0x7FE00000 == 0x52800000:  # movz (32/64-bit), hw=0
        return (insn >> 5) & 0xFFFF
    # Any other data-processing or load into register 8 ends the search.
    return None


def main(path):
    data = bytearray(open(path, "rb").read())
    if data[:4] != b"\x7fELF" or data[4] != 2 or struct.unpack_from("<H", data, 18)[0] != 183:
        sys.exit("nosyscall: not a 64-bit arm64 ELF")
    phoff, = struct.unpack_from("<Q", data, 32)
    phentsize, phnum = struct.unpack_from("<HH", data, 54)
    patched = {}
    for i in range(phnum):
        p_type, p_flags, p_offset, _, _, p_filesz = struct.unpack_from("<IIQQQQ", data, phoff + i * phentsize)
        if p_type != 1 or not p_flags & 1:  # PT_LOAD, executable
            continue
        start, end = (p_offset + 3) & ~3, p_offset + p_filesz
        for off in range(start, end - 3, 4):
            if struct.unpack_from("<I", data, off)[0] != SVC0:
                continue
            for back in range(1, 9):
                if off - back * 4 < start:
                    break
                imm = x8_imm(struct.unpack_from("<I", data, off - back * 4)[0])
                if imm == -1:
                    continue
                if imm in BLOCKED:
                    struct.pack_into("<I", data, off, MOVN_X0_ENOSYS)
                    patched[BLOCKED[imm]] = patched.get(BLOCKED[imm], 0) + 1
                break
    open(path, "wb").write(data)
    print("nosyscall: " + (", ".join(f"{n} x{c}" for n, c in sorted(patched.items())) or "nothing to patch"))
    if "set_robust_list" not in patched:
        sys.exit("nosyscall: no set_robust_list call site found; glibc changed, check this script")


if __name__ == "__main__":
    main(sys.argv[1])
