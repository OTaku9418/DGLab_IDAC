"""Hook mechanics pass 1: PE facts, trampoline disassembly, ABI trace."""
import sys
sys.path.insert(0, r"D:\INDAC\DGLab_IDAC\re")
from eng2 import pe, IB, md, DATA, SECT, read, cstr, TVA, TVSZ, TRAW, TRSZ, PD, dis, insns, scan_calls
import capstone, struct

print("=== PE FACTS ===")
print("ImageBase      : 0x%016X" % IB)
print("EntryPoint RVA : 0x%08X" % pe.OPTIONAL_HEADER.AddressOfEntryPoint)
print("SizeOfImage    : 0x%08X" % pe.OPTIONAL_HEADER.SizeOfImage)
print("Machine        : 0x%04X" % pe.FILE_HEADER.Machine)
print("Characteristics: 0x%04X" % pe.FILE_HEADER.Characteristics)
print("DllCharacteristics: 0x%04X" % pe.OPTIONAL_HEADER.DllCharacteristics)
print()
print("%-10s %-12s %-12s %-12s %-12s" % ("section", "VA", "VSize", "RawPtr", "RawSize"))
for n, (va, vsz, praw, rsz) in SECT.items():
    print("%-10s 0x%08X   0x%08X   0x%08X   0x%08X" % (n, va, vsz, praw, rsz))
print()

def A(rva):
    return IB + rva

TARGETS = [
    ("servo-on  invoke", 0x2160),
    ("servo-off invoke", 0x21A0),
    ("torque    invoke", 0x21C0),
    ("viscosity invoke", 0x2200),
    ("vibration invoke", 0x2240),
    ("UnBindAllFB      ", 0x2020),
]

print("=== TRAMPOLINE REGION RAW DISASM (0x2000 .. 0x2290) ===")
print(dis(0x2000, 0x290))
print()

print("=== PROLOGUE BYTES FOR EACH TARGET ===")
for name, rva in TARGETS:
    b = read(rva, 16)
    print("%s RVA 0x%08X abs 0x%016X : %s" % (name, rva, A(rva), b.hex(' ')))
print()

# Are the RVAs inside .pdata-described functions (i.e. real function starts)?
print("=== .pdata FUNCTION BOUNDARIES NEAR TARGETS ===")
for st, en, ui in PD:
    if 0x1F00 <= st <= 0x2300:
        print("  func 0x%08X - 0x%08X  (len 0x%X)  unwind@0x%X" % (st, en, en - st, ui))
print()

print("=== CALL/JMP SITES TARGETING EACH TRAMPOLINE ===")
allc = scan_calls()
for name, rva in TARGETS:
    hits = [(s, k) for s, k, t in allc if t == rva]
    print("%s (0x%08X): %d refs" % (name, rva, len(hits)))
    for s, k in sorted(hits):
        print("    0x%08X  %s" % (s, k))
print()
