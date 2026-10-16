"""Definitive pass: aligned disassembly of trampolines, UnBindAllFB, Bind* thunks,
aligned call-site context, direction-constant dataflow.  Writes final2.out"""
import sys
sys.path.insert(0, r"D:\INDAC\DGLab_IDAC\re")
from eng2 import pe, IB, md, DATA, SECT, read, cstr, TVA, TVSZ, TRAW, TRSZ, PD, dis, insns, scan_calls

def A(r): return IB + r

def listing(rva, n, mark=None, annot=True):
    out = []
    for i in insns(rva, n):
        a = "   <<<<<<<<<< CALL TO TRAMPOLINE" if mark is not None and i.address == mark else ""
        out.append("  %08x  %-26s %-8s %s%s" % (i.address, i.bytes.hex(' '), i.mnemonic, i.op_str, a))
    return out

def containing(rva):
    for st, en, ui in PD:
        if st <= rva < en: return st, en
    return None

print("#" * 100)
print("TRAMPOLINE LISTINGS (bytes + mnemonic).  ImageBase = 0x%X" % IB)
for nm, rva, ln in [("servo-on  void()", 0x2160, 0x40), ("servo-off void()", 0x21A0, 0x40),
                    ("torque    void(int,int)", 0x21C0, 0x40), ("viscosity void(int,int)", 0x2200, 0x40),
                    ("vibration void(int,int)", 0x2240, 0x40)]:
    print("\n### %s   RVA 0x%X  abs 0x%X" % (nm, rva, A(rva)))
    print("\n".join(listing(rva, ln)))
print()
print("### UnBindAllFB  RVA 0x2020  abs 0x%X" % A(0x2020))
print("\n".join(listing(0x2020, 0x140)))
print()

print("#" * 100)
print("EXPORTS Bind*/UnBind* : slot arithmetic (find `add reg, imm` / `lea reg,[reg+imm]` after singleton load)")
pe.parse_data_directories()
for e in sorted(pe.DIRECTORY_ENTRY_EXPORT.symbols, key=lambda x: x.address):
    if not e.name: continue
    nm = e.name.decode()
    if "FB" not in nm: continue
    rva = e.address
    print("\n--- %s  (export RVA 0x%X, abs 0x%X)" % (nm, rva, A(rva)))
    snip = []
    for i in insns(rva, 0x90):
        snip.append("  %08x  %-26s %-8s %s" % (i.address, i.bytes.hex(' '), i.mnemonic, i.op_str))
        if i.mnemonic == "jmp" and i.op_str.startswith("0x"):  # thunk to real body
            tgt = int(i.op_str, 16) - IB
            snip.append("      -> body RVA 0x%X :" % tgt)
            for j in insns(tgt, 0x80):
                snip.append("  %08x  %-26s %-8s %s" % (j.address, j.bytes.hex(' '), j.mnemonic, j.op_str))
            break
    print("\n".join(snip[:60]))
print()

print("#" * 100)
print("CALL SITES: aligned context (disasm from containing pdata function start)")
SITES = [(0x2B7B8, 0x2160, "servo-ON"), (0x2B7C1, 0x2240, "vibration"), (0x2B7E3, 0x2200, "viscosity"),
         (0x2B7EC, 0x2240, "vibration"), (0x2BE91, 0x2240, "vibration"), (0x2BCDB, 0x21C0, "torque"),
         (0x2BFC1, 0x2200, "viscosity"), (0xC053, 0x21A0, "servo-OFF")]
for site, tgt, nm in SITES:
    c = containing(site)
    print("\n=== %s  trampoline 0x%X : call at RVA 0x%X (abs 0x%X) in func %s ===" %
          (nm, tgt, site, A(site), ("0x%X-0x%X" % c) if c else "?"))
    if not c:
        print("   (no pdata container)"); continue
    st, en = c
    lo = max(st, site - 0x120)
    print("   --- window 0x%X..0x%X (disasm from function entry 0x%X) ---" % (lo, site + 0x30, st))
    for i in insns(st, site + 0x30 - st):
        r = i.address - IB
        if r < lo: continue
        mark = "   <<<<<<<< CALL 0x%X" % tgt if i.address == A(site) else ""
        print("  %08x  %-24s %-9s %s%s" % (i.address, i.bytes.hex(' '), i.mnemonic, i.op_str, mark))
print()

print("#" * 100)
print("DIRECTION-REGISTER DATAFLOW: writes to ecx/rcx/r13/r13d/edx within +-0x200 of each int-taking call site")
INT_SITES = [(0x2BCDB, "torque"), (0x2BFC1, "viscosity"), (0x2BE91, "vibration")]
WR = ("mov","xor","lea","or","and","add","sub","pop","imul","shl","sar","shr","movzx","movsxd","cwde","cdq")
for site, nm in INT_SITES:
    st, en = containing(site)
    print("\n=== %s  site 0x%X  in func 0x%X-0x%X ===" % (nm, site, st, en))
    for i in insns(st, site - st + 0x10):
        r = i.address - IB
        if r < site - 0x200: continue
        if i.mnemonic not in WR: continue
        ops = i.op_str
        if any(k in ops.split(',')[0].strip() for k in ("ecx","rcx","r13","edx","rdx","rcx","eax","r8d","r8")):
            print("  %08x  %-24s %-9s %s" % (i.address, i.bytes.hex(' '), i.mnemonic, i.op_str))
print()

print("#" * 100)
print("ALL r13 / r13d WRITES in whole .text (aligned, per pdata func), with 4 insns of context")
cnt = 0
for st, en, ui in PD:
    if en <= st or en - st > 0x40000: continue
    prev = []
    for i in insns(st, en - st):
        d = i.op_str.split(",")[0].strip()
        if i.mnemonic in ("mov","xor","lea","or","and","add","sub","pop","movsxd","movzx","imul","not","neg") and d in ("r13d","r13"):
            if st <= 0x2B000 <= en or st <= 0x2C000 <= en or st <= 0xC000 <= en:
                print("  func 0x%X: %08x  %-26s %-9s %s" % (st, i.address, i.bytes.hex(' '), i.mnemonic, i.op_str))
                cnt += 1
                if cnt > 400: break
    if cnt > 400: break
print("  (r13 writes listed: %d)" % cnt)
print()

print("#" * 100)
print("IMMEDIATE CONSTANTS written to ecx/edx/r8d/r9d in window 0x2B700-0x2C100 and 0xC000-0xC100")
for st, en, ui in PD:
    if not (st <= 0x2B700 < en or st <= 0xC000 < en): continue
    for i in insns(st, en - st):
        r = i.address - IB
        if not (0x2B700 <= r <= 0x2C100 or 0xC000 <= r <= 0xC100): continue
        if i.mnemonic in ("mov","xor","or","and","add","sub","lea","movsxd","imul"):
            dst = i.op_str.split(",")[0].strip()
            if dst in ("ecx","rcx","edx","rdx","r8d","r8","r13d","r13","eax","r9d","r9"):
                print("  %08x  %-26s %-9s %s" % (i.address, i.bytes.hex(' '), i.mnemonic, i.op_str))
print()
print("DONE")
