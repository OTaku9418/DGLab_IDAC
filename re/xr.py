import pefile, capstone, json, re, sys
from collections import defaultdict

P = r"D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Plugins\IndRun\Binaries\Win64\IndRun.dll"
pe = pefile.PE(P, fast_load=True)
pe.parse_data_directories()
IB = pe.OPTIONAL_HEADER.ImageBase
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True

SECTIONS = {s.Name.rstrip(b"\x00").decode(): (s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData) for s in pe.sections}

def rva2off(rva):
    for s in pe.sections:
        va = s.VirtualAddress
        sz = max(s.Misc_VirtualSize, s.SizeOfRawData)
        if va <= rva < va + sz:
            d = rva - va
            if d < s.SizeOfRawData:
                return s.PointerToRawData + d
    return None

DATA = pe.__data__
def read(rva, n):
    o = rva2off(rva)
    return DATA[o:o+n] if o is not None else b""

def is_text(rva):
    va, vsz, _, _ = SECTIONS[".text"]
    return va <= rva < va + vsz

# ---------------- xref scan ----------------
def scan_xrefs(lo=None, hi=None):
    """Return dict target_rva -> list of (insn_rva, mnemonic, op_str, kind)"""
    xr = defaultdict(list)
    va, vsz, praw, rsz = SECTIONS[".text"]
    code = DATA[praw:praw+rsz]
    for ins in md.disasm(code, IB + va):
        try:
            for op in ins.operands:
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                    tgt = (ins.address + ins.size + op.mem.disp) - IB
                    xr[tgt].append((ins.address - IB, ins.mnemonic, ins.op_str))
                elif op.type == capstone.x86.X86_OP_IMM:
                    t = op.imm - IB
                    if 0 <= t < pe.OPTIONAL_HEADER.SizeOfImage:
                        xr[t].append((ins.address - IB, ins.mnemonic, ins.op_str))
        except Exception:
            pass
    return xr

if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else "xref"
    if what == "hex":
        a = int(sys.argv[2], 16); n = int(sys.argv[3], 16)
        d = read(a, n)
        for i in range(0, len(d), 16):
            row = d[i:i+16]
            print("%08x  %-48s %s" % (a+i, " ".join("%02x" % b for b in row),
                  "".join(chr(b) if 32 <= b < 127 else "." for b in row)))
    elif what == "xref":
        xr = scan_xrefs()
        for t in sys.argv[2:]:
            t = int(t, 16)
            print("--- xrefs to %08x (%d) ---" % (t, len(xr.get(t, []))))
            for r, m, o in xr.get(t, []):
                print("   from %08x  %s %s" % (r, m, o))
    elif what == "xrefall":
        xr = scan_xrefs()
        lo = int(sys.argv[2], 16); hi = int(sys.argv[3], 16)
        for t in sorted(xr):
            if lo <= t < hi:
                print("%08x : %s" % (t, ", ".join("%08x" % r for r, _, _ in xr[t])))
