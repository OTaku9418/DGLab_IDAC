"""Pass 7: CORRECTED listings via eng2.insns (validated rva2off)."""
import sys
sys.path.insert(0, r"D:\INDAC\DGLab_IDAC\re")
from eng2 import IB, insns

def dump(fn, ln, lo, hi, title):
    print("--- %s   (func 0x%X, window 0x%X..0x%X) ---" % (title, fn, lo, hi))
    for i in insns(fn, ln):
        r = i.address - IB
        if r < lo: continue
        if r > hi: break
        print("   %08x  %-24s %-10s %s" % (i.address, i.bytes.hex(' '), i.mnemonic, i.op_str))
    print()

print("=" * 100); print("TRAMPOLINES + UnBindAllFB (aligned at pdata function entry)"); print("=" * 100)
for a, nm in ((0x2160, "SERVO-ON  voider"), (0x21A0, "SERVO-OFF"), (0x21C0, "TORQUE"), (0x2200, "VISCOSITY"), (0x2240, "VIBRATION"), (0x2020, "UnBindAllFB")):
    dump(a, 0x60, a, a + 0x5C, nm)
print("=" * 100); print("PATH INTO 0x2C965 (r13d = byte[rXX+0x1678])"); print("=" * 100)
dump(0x148A0, 0x18040, 0x2C3C0, 0x2C42A, "func 0x148A0")
print("=" * 100); print("[obj+0x1678] WRITER at 0x7739"); print("=" * 100)
dump(0x6E70, 0xC80, 0x76D0, 0x7750, "func 0x6E70")
