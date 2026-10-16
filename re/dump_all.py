import sys
sys.path.insert(0, r'D:\INDAC\DGLab_IDAC\re')
from eng2 import IB, insns

def dump(lo, hi, title):
    print('--- %s ---' % title)
    for i in insns(lo, 0x200):
        r = i.address - IB
        if r < lo:
            continue
        if r > hi:
            break
        print('%08X  %-24s %-8s %s' % (i.address, i.bytes.hex(' '), i.mnemonic, i.op_str))
    print()

dump(0x2160, 0x2199, 'servo-on 0x2160')
dump(0x21A0, 0x21BF, 'servo-off 0x21A0')
dump(0x21C0, 0x21FA, 'torque 0x21C0')
dump(0x2200, 0x223A, 'viscosity 0x2200')
dump(0x2240, 0x227A, 'vibration 0x2240')
dump(0x2020, 0x2154, 'UnBindAllFB 0x2020')

SITES = [(0x2B7B8, 'servo-on 0x2160'), (0x2B7C1, 'vibration 0x2240'),
         (0x2B7E3, 'viscosity 0x2200'), (0x2B7EC, 'vibration 0x2240'),
         (0x2BE91, 'vibration 0x2240'), (0x2BCDB, 'torque 0x21C0'),
         (0x2BFC1, 'viscosity 0x2200'), (0xC053, 'servo-off 0x21A0')]
FUNCS = {0x2B7B8: (0x148A0, 0x2D274), 0x2B7C1: (0x148A0, 0x2D274), 0x2B7E3: (0x148A0, 0x2D274),
         0x2B7EC: (0x148A0, 0x2D274), 0x2BE91: (0x148A0, 0x2D274), 0x2BCDB: (0x148A0, 0x2D274),
         0x2BFC1: (0x148A0, 0x2D274), 0xC053: (0x6E70, 0xCC5C)}
for site, what in SITES:
    st = FUNCS[site][0]
    print('--- call site 0x%X -> %s (window 0x%X..0x%X) ---' % (site, what, site - 0x30, site + 0xC))
    items = []
    for i in insns(st, site - st + 0x40):
        r = i.address - IB
        if r < site - 0x30:
            continue
        if r > site + 0xC:
            break
        items.append(i)
    for i in items:
        mark = '  <== CALL' if (i.address - IB) == site else ''
        print('%08X  %-24s %-8s %s%s' % (i.address, i.bytes.hex(' '), i.mnemonic, i.op_str, mark))
    print()
