import struct, sys
from unicorn import *
from unicorn.arm64_const import *
blob = bytearray(open('/tmp/fx/tb.bin','rb').read())
BASE, STK, CODE = 0x100000, 0x300000, 0x200000
RET = 0xd65f03c0
# the last two words: the vector's own first instruction, then b vector+4; make them a marker
blob[-8:] = struct.pack('<II', 0xd503201f, 0xd4200020)   # nop ; brk #1 = "declined"
eret_off = blob.find(struct.pack('<I', 0xd69f03e0))
fails = 0
def run(insn, regs, esr_ec=0, expect_decline=False):
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(BASE, 0x10000); uc.mem_map(STK, 0x10000); uc.mem_map(CODE, 0x10000)
    uc.mem_write(BASE, bytes(blob)); uc.mem_write(CODE, struct.pack('<I', insn))
    for i in range(31): uc.reg_write(UC_ARM64_REG_X0 + i if i < 29 else (UC_ARM64_REG_X29 if i == 29 else UC_ARM64_REG_X30), regs.get(i, 0x1000 + i))
    uc.reg_write(UC_ARM64_REG_SP, STK + 0x8000)
    # ESR_EL1: op0 3 op1 0 crn 5 crm 2 op2 0; ELR_EL1: 3,0,4,0,1
    uc.reg_write(UC_ARM64_REG_CP_REG, (5, 2, 3, 0, 0, esr_ec << 26))
    uc.reg_write(UC_ARM64_REG_CP_REG, (4, 0, 3, 0, 1, CODE))
    stop = {}
    def hook(uc, addr, size, ud):
        w = struct.unpack('<I', uc.mem_read(addr, 4))[0]
        if w == 0xd69f03e0: stop['how'] = 'eret'; uc.emu_stop()
        elif w == 0xd4200020: stop['how'] = 'decline'; uc.emu_stop()
        elif w in (0xd51be342, 0xd51be242, 0xd51be302, 0xd51be202): stop['msr'] = (w, uc.reg_read(UC_ARM64_REG_X2))
    uc.hook_add(UC_HOOK_CODE, hook)
    try: uc.emu_start(BASE, BASE + len(blob), count=2000)
    except UcError as e: stop['how'] = 'error %s at %#x' % (e, uc.reg_read(UC_ARM64_REG_PC))
    out = {i: uc.reg_read(UC_ARM64_REG_X0 + i if i < 29 else (UC_ARM64_REG_X29 if i == 29 else UC_ARM64_REG_X30)) for i in range(31)}
    elr = uc.reg_read(UC_ARM64_REG_CP_REG, (4, 0, 3, 0, 1))
    uc.msr = stop.get('msr', (0, 0))
    return stop.get('how', 'ran on'), out, elr, uc.reg_read(UC_ARM64_REG_SP), uc
def sysr(uc, crn, crm, op1, op2):
    # read it as code would: mrs x0, <reg> at CODE+0x100
    w = 0xd5380000 | op1 << 16 | crn << 12 | crm << 8 | op2 << 5
    uc.mem_write(CODE + 0x100, struct.pack('<II', w, 0xd503201f))
    uc.emu_start(CODE + 0x100, CODE + 0x104, count=1)
    return uc.reg_read(UC_ARM64_REG_X0)
def check(name, cond, extra=''):
    global fails
    if not cond: fails += 1
    print(('ok   ' if cond else 'FAIL ') + name + (' ' + extra if extra else ''))
base = {i: 0x1000 + i for i in range(31)}
# reads into every register
for rt in list(range(31)):
    how, out, elr, sp, uc = run(0x7a00 | 8 << 5 | rt, {})
    others = all(out[i] == base[i] for i in range(31) if i != rt)
    check('CNTFRQ read into x%d' % rt, how == 'eret' and out[rt] == 24000000 and others and elr == CODE + 4 and sp == STK + 0x8000, '%s %s' % (how, out[rt]))
for op, nm in [(0, 'CNTVCT'), (1, 'CNTPCT'), (4, 'CNTV_CVAL'), (5, 'CNTP_CVAL'), (6, 'CNTV_TVAL'), (7, 'CNTP_TVAL')]:
    for rt in (0, 3, 7, 16, 17, 20, 30):
        how, out, elr, sp, uc = run(0x7a00 | op << 5 | rt, {})
        others = all(out[i] == base[i] for i in range(31) if i != rt)
        check('%s read into x%d' % (nm, rt), how == 'eret' and others and elr == CODE + 4 and sp == STK + 0x8000, '%s value %#x' % (how, out[rt]))
# scaling of a known compare value: write then read
for op_w, op_r, crm, nm in [(9, 4, 3, 'CNTV_CVAL'), (10, 5, 2, 'CNTP_CVAL')]:
    for rt in (1, 9, 16, 17, 28):
        for v in (0, 4, 5, 24000000, 123456789012345, 0x7fffffffffffffff, 0xffffffffffffffff):
            how, out, elr, sp, uc = run(0x7a00 | op_w << 5 | rt, {rt: v})
            raw = uc.msr[1]; okreg = uc.msr[0] == (0xd51be342 if crm == 3 else 0xd51be242)
            sv = v - (1 << 64) if v >> 63 else v
            q = abs(sv) // 5 * (1 if sv >= 0 else -1)
            want = (sv - q) & ((1 << 64) - 1)
            same = all(out[i] == (v if i == rt else base[i]) for i in range(31))
            check('%s write of %#x from x%d' % (nm, v, rt), how == 'eret' and okreg and raw == want and same and elr == CODE + 4, '%s raw %#x want %#x' % (how, raw, want))
for op_w, crm, nm in [(11, 3, 'CNTV_TVAL'), (12, 2, 'CNTP_TVAL')]:
    for rt in (2, 16, 17):
        for v in (0, 5, 24000, 0x7fffffff, 0xdeadbeef00000064):
            how, out, elr, sp, uc = run(0x7a00 | op_w << 5 | rt, {rt: v})
            same = all(out[i] == (v if i == rt else base[i]) for i in range(31))
            raw = sysr(uc, 14, crm, 3, 0) & 0xffffffff
            sv = v & 0xffffffff; sv = sv - (1 << 32) if sv >> 31 else sv
            want = sv - (abs(sv) // 5 * (1 if sv >= 0 else -1))
            rawS = raw - (1 << 32) if raw >> 31 else raw
            check('%s write of %#x from x%d' % (nm, v, rt), how == 'eret' and same and elr == CODE + 4 and 0 <= want - rawS < 200000, '%s reads back %d, wanted just under %d' % (how, rawS, want))
# declines: another undefined word, op 13, a class that is neither
for insn, ec, nm in [(0x00000000, 0, 'udf #0'), (0x7a00 | 13 << 5 | 3, 0, 'op 13'), (0x7c00, 0, '0x7c00'), (0x7a00 | 3, 0x15, 'an SVC class'), (0xd4200000, 0x3c, 'a BRK')]:
    how, out, elr, sp, uc = run(insn, {}, esr_ec=ec)
    check('declined: %s' % nm, how == 'decline' and all(out[i] == base[i] for i in range(31)) and sp == STK + 0x8000 and elr == CODE, how)
print('FAILS', fails)
