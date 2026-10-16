"""Robust 4-byte RIP-displacement scan + targeted helpers."""
import pefile, capstone, sys, struct, re
from collections import defaultdict
from eng2 import pe, IB, md, DATA, SECT, read, cstr, TVA, TVSZ, TRAW, TRSZ, PD, dis, insns

def scan_disp(targets, text_only=True):
    """Find every instruction whose RIP-relative displacement field resolves to one of targets.
    Scans raw bytes for a disp32 at offset i where i+4+disp == target (target in RVA)."""
    res = defaultdict(list)
    tset = set(targets)
    blob = DATA[TRAW:TRAW+TRSZ]
    for i in range(len(blob)-4):
        disp = int.from_bytes(blob[i:i+4], 'little', signed=True)
        tgt = TVA + i + 4 + disp
        if tgt in tset:
            res[tgt].append(TVA + i)
    return res

def decode_at(site_disp_off, back=6, fwd=4):
    """Disassemble around a displacement field offset (rva of disp)."""
    start = site_disp_off - back
    return dis(start, back+fwd+8)

if __name__ == "__main__":
    mode = sys.argv[1]
    if mode == "scan":
        targets = [int(x,16) for x in sys.argv[2:]]
        r = scan_disp(targets)
        for t in targets:
            sites = sorted(set(r.get(t, [])))
            print("=== %08x : %d sites ===" % (t, len(sites)))
            for s in sites:
                ins = insns(s-7, 12)
                txt = " | ".join("%s %s" % (i.mnemonic, i.op_str) for i in ins[:3])
                print("   disp@%08x  %s" % (s, txt))
