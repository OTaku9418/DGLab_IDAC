"""Pass 4: predecessors of the torque/viscosity block, r13d candidate writers, field 0x1678 refs."""
import sys
sys.path.insert(0, r"D:\INDAC\DGLab_IDAC\re")
from eng2 import pe, IB, md, DATA, SECT, read, PD, dis, insns, scan_calls

print("=" * 100)
print("A) every jmp/jcc whose TARGET is inside the FFI block 0x2B600..0x2C200 (predecessors)")
calls = scan_calls()
for s, k, t in sorted(calls):
    if 0x2B600 <= t <= 0x2C200 and k == "jmp":
        print("   jmp   site 0x%06X -> 0x%06X" % (s, t))
ccs = []
for st, en, ui in PD:
    if en <= st or en - st > 0x40000: continue
    if not (st <= 0x2B600 < en or st <= 0x1E900 < en or st == 0x148A0): continue
    for i in insns(st, en - st):
        r = i.address - IB
        if i.mnemonic.startswith("j") and i.mnemonic != "jmp" and i.op_str.startswith("0x"):
            t = int(i.op_str, 16) - IB
            if 0x2B600 <= t <= 0x2C200:
                ccs.append((r, i.mnemonic, t))
for r, m, t in sorted(ccs):
    print("   %-6s site 0x%06X -> 0x%06X" % (m, r, t))
print()

print("=" * 100)
print("B) who jumps to 0x2C965 / 0x2C96D (the r13d = byte[rdi+0x1678] load)?")
for s, k, t in sorted(calls):
    if t in (0x2C965, 0x2C96D):
        print("   %s site 0x%06X -> 0x%06X" % (k, s, t))
for st, en, ui in PD:
    if not (st <= 0x2C965 < en): continue
    for i in insns(st, en - st):
        if i.mnemonic.startswith("j") and i.mnemonic != "jmp" and i.op_str.startswith("0x"):
            t = int(i.op_str, 16) - IB
            if t in (0x2C965, 0x2C96D):
                print("   %-6s site 0x%06X -> 0x%06X" % (i.mnemonic, i.address - IB, t))
print()

print("=" * 100)
print("C) context around the three `mov r13d, 1` sites in func 0x148A0")
for a in (0x1B531, 0x1E5D0, 0x1E998):
    print("   --- around 0x%X ---" % a)
    for i in insns(a - 0x20, 0x48):
        mk = "   <<<" if i.address - IB == a else ""
        print("   %08x  %-26s %-9s %s%s" % (i.address, i.bytes.hex(' '), i.mnemonic, i.op_str, mk))
print()

print("=" * 100)
print("D) every reference to displacement 0x1678 anywhere in decoded functions (whole image)")
n = 0
for st, en, ui in PD:
    if en <= st or en - st > 0x40000: continue
    for i in insns(st, en - st):
        if "0x1678" in i.op_str:
            print("   func 0x%06X  %08x  %-26s %-9s %s" % (st, i.address, i.bytes.hex(' '), i.mnemonic, i.op_str))
            n += 1
            if n > 60: break
    if n > 60: break
print("   (%d refs)" % n)
print()

print("=" * 100)
print("E) the gate byte [obj+0x5812] and [obj+0x164d] / [obj+0x4cc] / [obj+0x4e8] refs count")
for off in ("0x5812", "0x164d", "0x4cc", "0x4e8", "0x1640", "0x163c", "0x1644"):
    n = 0; sites = []
    for st, en, ui in PD:
        if en <= st or en - st > 0x40000: continue
        for i in insns(st, en - st):
            if off in i.op_str:
                n += 1
                if len(sites) < 6: sites.append("%08x" % (i.address - IB))
    print("   disp %-7s : %4d refs   e.g. %s" % (off, n, ", ".join(sites)))
print("DONE")
