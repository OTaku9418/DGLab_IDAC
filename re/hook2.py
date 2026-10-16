"""Hook mechanics pass 2: rest of block, bind functions, singleleton lazy init, call sites."""
import sys
sys.path.insert(0, r"D:\INDAC\DGLab_IDAC\re")
from eng2 import pe, IB, md, DATA, SECT, read, cstr, TVA, TVSZ, TRAW, TRSZ, PD, dis, insns, scan_calls
import capstone

def A(rva): return IB + rva

print("=== VIBRATION + FOLLOWING (0x2236 .. 0x2330) ===")
print(dis(0x2236, 0x100))
print()

print("=== WHAT FUNCTION CONTAINS 0x2000? ===")
for st, en, ui in PD:
    if st <= 0x2000 < en:
        print("  containing func: 0x%08X - 0x%08X" % (st, en))
        print(dis(st, 0x30))
print()

print("=== pdata functions in 0x1F00..0x2400 (definitive boundaries) ===")
for st, en, ui in PD:
    if 0x1F00 <= st <= 0x2400:
        b = read(st, 12)
        print("  0x%08X - 0x%08X len=0x%-5X first12=%s" % (st, en, en - st, b.hex(' ')))
print()

BINDS = [
    ("BindServoOn",            0x5FA70),
    ("BindServoOff",           0x5FB10),
    ("BindCentering(void())",  0x5FBB0),
    ("BindCentering(int())",   0x5FC60),
    ("BindTorqueFunction",     0x5FD10),
    ("BindViscosityFunction",  0x5FD80),
    ("BindVibrationFunction",  0x5FE30),
]
for name, rva in BINDS:
    print("=== %s @ RVA 0x%08X (abs 0x%016X) ===" % (name, rva, A(rva)))
    print("    first16 = %s" % read(rva, 16).hex(' '))
    print(dis(rva, 0xB0))
    print()

print("=== UnBindAllFB exported @ 0x5FA60 ===")
print("    first16 = %s" % read(0x5FA60, 16).hex(' '))
print(dis(0x5FA60, 0x40))
print()

print("=== SINGLETON LAZY ALLOC REGION 0x5E080 .. 0x5E100 ===")
print(dis(0x5E080, 0x90))
print()

print("=== ALL RIP-REL REFS TO 0x241890 (raw disp32 scan) ===")
blob = DATA[TRAW:TRAW+TRSZ]
hits = []
for i in range(len(blob) - 4):
    disp = int.from_bytes(blob[i:i+4], 'little', signed=True)
    if TVA + i + 4 + disp == 0x241890:
        hits.append(TVA + i)
print("  count =", len(hits))
for h in hits:
    ins = list(md.disasm(read(h - 10, 20), IB + h - 10))
    txt = " | ".join("%s %s" % (x.mnemonic, x.op_str) for x in ins[:4])
    print("    disp@0x%08X (insn start ~0x%08X): %s" % (h, h - 7, txt))
print()

print("=== ALL CALL/JMP SITES TO TRAMPOLINES ===")
allc = scan_calls()
for name, rva in [("servo?on", 0x2160), ("servo?off", 0x21A0), ("torque", 0x21C0),
                  ("viscosity", 0x2200), ("vibration", 0x2240), ("UnBindAllFB", 0x2020)]:
    hits = sorted([(s, k) for s, k, t in allc if t == rva])
    print("%-12s 0x%08X : %d refs" % (name, rva, len(hits)))
    for s, k in hits:
        print("     0x%08X  %s" % (s, k))
print()
