import pefile, capstone, re, sys
from collections import defaultdict
from xr import pe, IB, md, DATA, SECTIONS, read, rva2off

TEXT_VA, TEXT_VSZ, TEXT_RAW, TEXT_RSZ = SECTIONS[".text"]
code = DATA[TEXT_RAW:TEXT_RAW+TEXT_RSZ]

# ---- find all strings (ascii + utf16) with their RVAs ----
def collect_strings(minlen=5):
    out = []
    for name, (va, vsz, praw, rsz) in SECTIONS.items():
        blob = DATA[praw:praw+rsz]
        for m in re.finditer(rb"[\x20-\x7e]{%d,}" % minlen, blob):
            out.append((va + m.start(), m.group().decode()))
    return out

def find_string(needle):
    res = []
    for name, (va, vsz, praw, rsz) in SECTIONS.items():
        blob = DATA[praw:praw+rsz]
        for pat, enc in ((needle.encode(), "a"), (needle.encode("utf-16-le"), "w")):
            st = 0
            while True:
                i = blob.find(pat, st)
                if i < 0: break
                res.append((va + i, enc))
                st = i + 1
    return res

# ---- full xref map (computed once) ----
_XR = None
def xrefs():
    global _XR
    if _XR is not None: return _XR
    xr = defaultdict(list)
    for ins in md.disasm(code, IB + TEXT_VA):
        try:
            for op in ins.operands:
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                    tgt = (ins.address + ins.size + op.mem.disp) - IB
                    xr[tgt].append((ins.address - IB, ins.mnemonic, ins.op_str))
        except Exception:
            pass
    _XR = xr
    return xr

if __name__ == "__main__":
    mode = sys.argv[1]
    if mode == "findstr":
        for needle in sys.argv[2:]:
            for rva, enc in find_string(needle):
                print("STR %08x (%s) %r" % (rva, enc, needle))
                for fr, m, o in xrefs().get(rva, []):
                    print("     xref from %08x  %s %s" % (fr, m, o))
    elif mode == "globals":
        # all rip-relative targets in .data / .rdata referenced from .text, counted
        cnt = defaultdict(int)
        for t, lst in xrefs().items():
            for fr, m, o in lst:
                cnt[t] += 1
        out = [(t, c) for t, c in cnt.items() if t >= 0x200000]
        out.sort(key=lambda x: -x[1])
        for t, c in out[:400]:
            print("%08x  %4d refs" % (t, c))
    elif mode == "at":
        t = int(sys.argv[2], 16)
        for fr, m, o in sorted(xrefs().get(t, [])):
            print("  from %08x  %s %s" % (fr, m, o))
