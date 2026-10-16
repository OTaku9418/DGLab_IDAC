"""Robust engine v2: byte-pattern call/jmp scan + pdata validation + full xref map."""
import pefile, capstone, sys, json
from collections import defaultdict

P = r"D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Plugins\IndRun\Binaries\Win64\IndRun.dll"
pe = pefile.PE(P, fast_load=True)
pe.parse_data_directories()
IB = pe.OPTIONAL_HEADER.ImageBase
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True
DATA = pe.__data__
SECT = {s.Name.rstrip(b"\x00").decode(): (s.VirtualAddress, s.Misc_VirtualSize,
        s.PointerToRawData, s.SizeOfRawData) for s in pe.sections}
TVA, TVSZ, TRAW, TRSZ = SECT[".text"]

def rva2off(rva):
    for s in pe.sections:
        va, sz = s.VirtualAddress, max(s.Misc_VirtualSize, s.SizeOfRawData)
        if va <= rva < va + sz:
            d = rva - va
            if d < s.SizeOfRawData: return s.PointerToRawData + d
    return None

def read(rva, n):
    o = rva2off(rva)
    return DATA[o:o+n] if o is not None else b""

def cstr(rva, maxn=200):
    d = read(rva, maxn)
    if not d: return None
    z = d.find(b"\x00")
    if z <= 0: return None
    s = d[:z]
    if all(32 <= c < 127 or c in (9,10,13) for c in s): return s.decode()
    return None

# ---- pdata validation ----
def pdata_raw():
    va, vsz, praw, rsz = SECT[".pdata"]
    ents = []
    for off in range(0, rsz - 11, 12):
        e = DATA[praw+off:praw+off+12]
        st, en, ui = (int.from_bytes(e[0:4],'little'), int.from_bytes(e[4:8],'little'),
                      int.from_bytes(e[8:12],'little'))
        ents.append((off//12, st, en, ui))
    return ents

def valid_pdata():
    ents = pdata_raw()
    good = []
    for i, st, en, ui in ents:
        if not (TVA <= st < TVA + TVSZ): break
        if not (st < en <= TVA + TVSZ): break
        good.append((st, en, ui))
    return good

PD = valid_pdata()

def insns(rva, n):
    code = read(rva, n)
    return list(md.disasm(code, IB + rva)) if code else []

# ---- call/jmp rel32 scan ----
def scan_calls():
    """Return list of (site_rva, kind, target_rva) scanning whole .text bytes."""
    out = []
    blob = DATA[TRAW:TRAW+TRSZ]
    for i in range(len(blob) - 4):
        op = blob[i]
        if op not in (0xE8, 0xE9):
            continue
        rel = int.from_bytes(blob[i+1:i+5], 'little', signed=True)
        tgt = TVA + i + 5 + rel
        if TVA <= tgt < TVA + TVSZ:
            out.append((TVA + i, "call" if op == 0xE8 else "jmp", tgt))
    return out

# ---- rip-relative xref scan: decode every pdata function ----
_XR = None
def xrefs():
    global _XR
    if _XR is not None: return _XR
    xr = defaultdict(list)
    for st, en, _ in PD:
        if en <= st or en - st > 0x10000: continue
        for ins in insns(st, en - st):
            for op in ins.operands:
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                    xr[ins.address + ins.size + op.mem.disp - IB].append(
                        (ins.address - IB, ins.mnemonic, ins.op_str))
    _XR = xr
    return xr

def dis(rva, n, stop_ret=False, ann=True):
    out = []
    for ins in md.disasm(read(rva, n), IB + rva):
        a = ""
        if ann:
            for op in ins.operands:
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                    t = ins.address + ins.size + op.mem.disp - IB
                    s = cstr(t)
                    a += "  ; [%08x]" % t
                    if s: a += ' "%s"' % s[:80]
                elif op.type == capstone.x86.X86_OP_IMM:
                    tv = op.imm - IB
                    if TVA <= tv < TVA + TVSZ: a += "  ; sub_%08x" % tv
        out.append("  %08x  %-9s %s%s" % (ins.address, ins.mnemonic, ins.op_str, a))
        if stop_ret and ins.mnemonic == "ret": break
    return "\n".join(out)

if __name__ == "__main__":
    m = sys.argv[1]
    if m == "info":
        print("valid pdata funcs:", len(PD))
        print("first:", ["%x-%x" % (a, b) for a, b, _ in PD[:5]])
        print("last :", ["%x-%x" % (a, b) for a, b, _ in PD[-5:]])
        c = scan_calls()
        print("calls:", sum(1 for x in c if x[1] == "call"), "jumps:", sum(1 for x in c if x[1] == "jmp"))
    elif m == "callers":
        tg = set(int(x, 16) for x in sys.argv[2:])
        for s, k, t in scan_calls():
            if t in tg: print("%08x %s -> %08x" % (s, k, t))
    elif m == "refs":
        base = int(sys.argv[2], 16)
        xr = xrefs()
        for r, mn, o in sorted(set(xr.get(base, []))):
            nxt = [i for i in insns(r, 24)][1:3]
            print("  %08x  %s %-28s | %s" % (r, mn, o,
                  " ; ".join("%s %s" % (i.mnemonic, i.op_str) for i in nxt)))
    elif m == "at":
        xr = xrefs()
        for t in sys.argv[2:]:
            t = int(t, 16)
            print("--- xrefs to %08x : %d ---" % (t, len(xr.get(t, []))))
            for r, mn, o in sorted(set(xr.get(t, []))):
                print("   %08x  %s %s" % (r, mn, o))
    elif m == "func":
        print(dis(int(sys.argv[2], 16), int(sys.argv[3], 16) if len(sys.argv) > 3 else 400))
