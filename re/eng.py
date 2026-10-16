"""Proper function-aware xref engine using .pdata RUNTIME_FUNCTION table."""
import pefile, capstone, re, sys, json
from collections import defaultdict

P = r"D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Plugins\IndRun\Binaries\Win64\IndRun.dll"
pe = pefile.PE(P, fast_load=True)
pe.parse_data_directories()
IB = pe.OPTIONAL_HEADER.ImageBase
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True
DATA = pe.__data__
SECT = {s.Name.rstrip(b"\x00").decode(): (s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData) for s in pe.sections}

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

def cstr(rva, maxn=120):
    d = read(rva, maxn)
    if not d: return None
    z = d.find(b"\x00")
    if z <= 0: return None
    s = d[:z]
    if all(32 <= c < 127 or c in (9,10,13) for c in s):
        return s.decode()
    return None

# ---------- .pdata -> function ranges ----------
def pdata_funcs():
    va, vsz, praw, rsz = SECT[".pdata"]
    out = []
    for off in range(0, min(vsz, rsz), 12):
        e = DATA[praw+off:praw+off+12]
        if len(e) < 12: break
        st, en, ui = (int.from_bytes(e[0:4],'little'), int.from_bytes(e[4:8],'little'),
                      int.from_bytes(e[8:12],'little'))
        if st == 0: continue
        out.append((st, en, ui))
    out.sort()
    return out

FUNCS = pdata_funcs()
FUNC_RVA = sorted(set(f[0] for f in FUNCS))
print("[i] %d functions from .pdata, %d unique starts" % (len(FUNCS), len(FUNC_RVA)), file=sys.stderr)

def insns_in(rva, end):
    code = read(rva, end - rva)
    if not code: return []
    res = []
    for ins in md.disasm(code, IB + rva):
        res.append(ins)
    return res

def annotate(ins):
    notes = []
    for op in ins.operands:
        if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
            t = ins.address + ins.size + op.mem.disp - IB
            s = cstr(t)
            notes.append(("rip", t, s))
        elif op.type == capstone.x86.X86_OP_IMM:
            v = op.imm
            notes.append(("imm", v, None))
    return notes

def dis(rva, n, stop_ret=True, maxins=500, show_data=True):
    out = []
    code = read(rva, n)
    cnt = 0
    for ins in md.disasm(code, IB + rva):
        ann = ""
        for kind, t, s in annotate(ins):
            if kind == "rip":
                trva = t
                ann += "  ; ->%08x" % trva
                if s: ann += ' "%s"' % s[:70]
                else:
                    q = read(trva, 8)
                    if q: ann += " q=%016x" % int.from_bytes(q,'little')
            else:
                tv = t - IB
                if 0x1000 <= tv < 0x8a000:
                    ann += "  ; sub_%08x" % tv
        out.append("  %08x  %-9s %s%s" % (ins.address, ins.mnemonic, ins.op_str, ann))
        cnt += 1
        if stop_ret and ins.mnemonic == "ret":
            break
        if cnt > maxins:
            out.append("  ..."); break
    return "\n".join(out)

# ---------- xref map built from function bodies ----------
_XR = None
def xrefs():
    global _XR
    if _XR is not None: return _XR
    xr = defaultdict(list)
    for st, en, _ in FUNCS:
        if en <= st or en - st > 0x20000: continue
        for ins in insns_in(st, en):
            r = ins.address - IB
            for op in ins.operands:
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                    xr[ins.address + ins.size + op.mem.disp - IB].append((r, ins.mnemonic, ins.op_str))
    _XR = xr
    return xr

if __name__ == "__main__":
    m = sys.argv[1]
    if m == "at":
        xr = xrefs()
        for t in sys.argv[2:]:
            t = int(t, 16)
            print("--- xrefs to %08x : %d ---" % (t, len(xr.get(t,[]))))
            for r, mn, o in sorted(xr.get(t, [])):
                print("   %08x  %s %s" % (r, mn, o))
    elif m == "func":
        rva = int(sys.argv[2], 16)
        n = int(sys.argv[3], 16) if len(sys.argv) > 3 else 400
        print(dis(rva, n, stop_ret=False))
    elif m == "callers":
        # find call rel32 targets matching given rvas
        targets = set(int(x,16) for x in sys.argv[2:])
        for st, en, _ in FUNCS:
            for ins in insns_in(st, en):
                if ins.mnemonic == "call" and ins.op_str.startswith("0x"):
                    t = int(ins.op_str, 16) - IB
                    if t in targets:
                        print("%08x -> %08x" % (ins.address - IB, t))
    elif m == "offsets":
        # find ALL reads/writes of  base_global + const
        base = int(sys.argv[2], 16)
        ptn = int(sys.argv[3], 16) if len(sys.argv) > 3 else None
        xr = xrefs()
        print("### direct refs to global %08x" % base)
        for r, mn, o in sorted(xr.get(base, [])):
            # peek next instruction
            code = read(r, 20)
            nxt = list(md.disasm(code, IB + r))[:3]
            print("   %08x  %s %s   ||  next: %s" % (r, mn, o,
                  " | ".join("%s %s" % (i.mnemonic, i.op_str) for i in nxt[1:3])))
