"""Pass 6: authoritative aligned listings - trampolines, UnBindAllFB, 0x7739 writer, path into 0x2C965."""
import sys
sys.path.insert(0, r"D:\INDAC\DGLab_IDAC\re")
from eng2 import IB, md, insns

def dump(fn_start, fn_len, lo, hi, title):
    print("--- %s   (window 0x%X..0x%X inside func 0x%X) ---" % (title, lo, hi, fn_start))
    for i in md.disasm(open(r"D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Plugins\IndRun\Binaries\Win64\IndRun.dll", "rb").read()[0x400 + fn_start:0x400 + fn_start + fn_len], IB + fn_start):
        r = i.address - IB
        if r < lo: continue
        if r > hi: break
        print("   %08x  %-24s %-10s %s" % (i.address, i.bytes.hex(' '), i.mnemonic, i.op_str))
    print()

F = r"D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Plugins\IndRun\Binaries\Win64\IndRun.dll"
print("=" * 100); print("TRAMPOLINES + UnBindAllFB (aligned at function entry)"); print("=" * 100)
for a, nm in ((0x2160, "SERVO-ON"), (0x21A0, "SERVO-OFF"), (0x21C0, "TORQUE"), (0x2200, "VISCOSITY"), (0x2240, "VIBRATION"), (0x2020, "UnBindAllFB")):
    dump(a, 0x60, a, a + 0x5C, nm)
print("=" * 100); print("PATH INTO 0x2C965 (r13d = byte[rXX+0x1678])"); print("=" * 100)
dump(0x148A0, 0x18000, 0x2C3C0, 0x2C42A, "func 0x148A0")
print("=" * 100); print("[obj+0x1678] WRITER at 0x7739"); print("=" * 100)
dump(0x6E70, 0x5E00, 0x76D0, 0x7750, "func 0x6E70")
