"""Hook mechanics pass 3: singleton init (aligned), call-site contexts, direction constants."""
import sys, struct, re
sys.path.insert(0, r"D:\INDAC\DGLab_IDAC\re")
from eng2 import pe, IB, md, DATA, SECT, read, cstr, TVA, TVSZ, TRAW, TRSZ, PD, dis, insns, scan_calls

def A(rva): return IB + rva

# ---- properly aligned: locate the function containing 0x5E0B7 ----
print("=== .pdata function containing 0x5E0B7 ===")
for st, en, ui in PD:
    if st <= 0x5E0B7 < en:
        print("  func 0x%08X - 0x%08X (len 0x%X)" % (st, en, en - st))
        print(dis(st, min(en - st, 0x260)))
print()

print("=== Unwind info for trampolines (leaf vs framed) ===")
for st, en, ui in PD:
    if 0x2000 <= st <= 0x2280:
        b = read(ui, 12)
        print("  func 0x%08X-0x%08X unwind@0x%08X bytes=%s" % (st, en, ui, b.hex(' ')))
print()

print("=== FIRST 5 BYTES decode of each trampoline (MinHook relocation) ===")
for name, rva, ln in [("servo?on", 0x2160, 0x3A), ("servo?off", 0x21A0, 0x1F),
                      ("torque", 0x21C0, 0x3B), ("viscosity", 0x2200, 0x3B),
                      ("vibration", 0x2240, 0x3B), ("UnBindAll", 0x2020, 0x135)]:
    code = read(rva, 16)
    total = 0
    print("  %s RVA 0x%08X : %s" % (name, rva, code.hex(' ')))
    for ins in md.disasm(code, IB + rva):
        total += ins.size
        print("      +%-2d  %-8s %s   (cum=%d)" % (ins.size, ins.mnemonic, ins.op_str, total))
        if total >= 5: break
    # does the function contain any rel jump in first 5 bytes?
print()

print("=== CALL SITE CONTEXTS ===")
SITES = [
    ("servo?on 0x2160", 0x2B7B8),
    ("vibration 0x2240", 0x2B7C1),
    ("viscosity 0x2200", 0x2B7E3),
    ("vibration 0x2240", 0x2B7EC),
    ("vibration 0x2240", 0x2BE91),
    ("torque 0x21C0", 0x2BCDB),
    ("viscosity 0x2200", 0x2BFC1),
    ("servo?off 0x21A0", 0xC053),
]
for label, site in SITES:
    print("--- %s   call at 0x%08X (abs 0x%016X) ---" % (label, site, A(site)))
    print(dis(site - 0x40, 0x60))
    print()
