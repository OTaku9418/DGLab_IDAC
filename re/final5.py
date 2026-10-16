"""Pass 5: exact call-site listings (re-verify), [obj+0x1678] writer context, path into 0x2C965."""
import sys
sys.path.insert(0, r"D:\INDAC\DGLab_IDAC\re")
from eng2 import IB, md, dis, insns

def win(tag, start, n, mark=()):
    print("--- %s @0x%X ---" % (tag, start))
    for i in insns(start, 0x40):
        if i.address - IB > start + 0x60: break
        mk = "   <<<" if (i.address - IB) in mark else ""
        print("   %08x  %-24s %-9s %s%s" % (i.address, i.bytes.hex(' '), i.mnemonic, i.op_str, mk))
    print()

print("=" * 100); print("TORQUE / VISCOSITY / VIBRATION / SERVO call sites, re-verified byte-exact"); print("=" * 100)
win("torque call 0x2BCDB", 0x2BCA0, 0, (0x2BCB3, 0x2BCDB))
win("servo-on 0x2B7B8 + vibration 0x2B7C1 + viscosity 0x2B7E3 + vibration 0x2B7EC", 0x2B780, 0,
    (0x2B7B8, 0x2B7C1, 0x2B7E3, 0x2B7EC))
win("vibration 0x2BE91", 0x2BE60, 0, (0x2BE91,))
win("viscosity 0x2BFC1", 0x2BF90, 0, (0x2BFC1,))
win("servo-off 0xC053 (func 0x6E70)", 0xC020, 0, (0xC053,))

print("=" * 100); print("[obj+0x1678] writer at 0x7739 (func 0x6E70) - what is edx?"); print("=" * 100)
win("0x7739", 0x76E0, 0, (0x7739,))

print("=" * 100); print("path into 0x2C965 (je from 0x2C406)"); print("=" * 100)
win("0x2C406 -> 0x2C965", 0x2C3C0, 0, (0x2C406, 0x2C965))
