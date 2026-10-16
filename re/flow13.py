"""Forward dataflow over the mega-function CFG to pin the constant value of r13d
at the torque call site (0x2bcd8) and the viscosity call site (0x2bfbe)."""
from eng2 import read, IB, md
import re, struct

A, B = 0x148a0, 0x2d274
code = read(A, B - A)
insns = list(md.disasm(code, IB + A))
addr2i = {}
for i, ins in enumerate(insns):
    addr2i[ins.address - IB] = i
RVAS = sorted(addr2i)

# ---- collect jump-table targets for indirect jmps -------------------------
def jump_table_targets(ins, rva):
    """Best-effort resolution of MSVC x64 switch jumps."""
    t = []
    m = re.search(r'jmp\s+qword ptr \[rip \+ (0x[0-9a-f]+)\]', ins.op_str)
    if m:
        base = rva + ins.size + int(m.group(1), 16)
        for k in range(64):
            b = read(base + k * 8, 8)
            if len(b) < 8: break
            v = struct.unpack("<Q", b)[0]
            if v == 0: continue
            t.append(v - IB)
    return t

succ = {}
unresolved = 0
for i, ins in enumerate(insns):
    rva = ins.address - IB
    s = []
    m, o = ins.mnemonic, ins.op_str
    if m == "ret" or m == "int3":
        pass
    elif m == "jmp":
        if o.startswith("0x"):
            s.append(int(o, 16) - IB)
        else:
            tt = jump_table_targets(ins, rva)
            if tt: s += tt
            else:
                unresolved += 1
                s += RVAS          # conservative: assume anything reachable
    elif m.startswith("j"):
        s.append(int(o, 16) - IB)
        if i + 1 < len(insns): s.append(insns[i + 1].address - IB)
    else:
        if i + 1 < len(insns): s.append(insns[i + 1].address - IB)
    succ[rva] = [x for x in s if x in addr2i]

print("[i] unresolved indirect jmps:", unresolved)

# ---- dataflow: r13d lattice (None = unknown, ('c',k) = const k) ----------
def effect(ins):
    """Return ('set', value_or_None) if this insn writes r13d, else None."""
    m, o = ins.mnemonic, ins.op_str
    if m in ("mov", "movzx", "movsxd", "lea", "add", "sub", "xor", "and", "or", "pop", "inc", "dec") \
            and o.split(",")[0].strip() in ("r13d", "r13"):
        dst, _, src = o.partition(",")
        dst, src = dst.strip(), src.strip()
        if m == "mov" and re.fullmatch(r'0x[0-9a-f]+', src):
            return ("c", int(src, 16) & 0xFFFFFFFF)
        if m == "xor" and dst == src == "r13d":
            return ("c", 0)
        return None            # unknown
    return None

IN = {RVAS[0]: ("c", None)}     # entry: r13d unknown
work = [RVAS[0]]
iters = 0
while work and iters < 400000:
    iters += 1
    rva = work.pop()
    st = IN[rva]
    eff = effect(insns[addr2i[rva]])
    out = eff if eff is not None else st
    for s in succ[rva]:
        old = IN.get(s, "unset")
        if old == "unset" or old != out:
            if old != "unset" and old is not None and out is None:
                pass       # widening: unknown absorbs
            IN[s] = out
            work.append(s)
print("[i] visited", len(IN), "of", len(RVAS), "insns; iters", iters)

for site, label in [(0x2bcd8, "direction arg for torque (mov ecx, r13d)"),
                    (0x2bfbe, "first arg for viscosity (mov ecx, r13d)"),
                    (0x2bcdb, "torque call"),
                    (0x2bfc1, "viscosity call")]:
    v = IN.get(site, "unreachable")
    print("  %08x  r13d state = %-14s  (%s)" % (site, v, label))
