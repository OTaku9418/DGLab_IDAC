"""Hook mechanics pass 4: unwind info, aligned call-site context, r13d/ecx dataflow,
address-taken check for trampolines."""
import sys, struct, re
sys.path.insert(0, r"D:\INDAC\DGLab_IDAC\re")
from eng2 import pe, IB, md, DATA, SECT, read, cstr, TVA, TVSZ, TRAW, TRSZ, PD, dis, insns, scan_calls

def A(r):
    return IB + r

def raw_pdata_entries():
    va, vsz, praw, rsz = SECT[".pdata"]
    out = []
    for off in range(0, rsz - 11, 12):
        e = DATA[praw+off:praw+off+12]
        out.append((off // 12, int.from_bytes(e[0:4], 'little'),
                    int.from_bytes(e[4:8], 'little'), int.from_bytes(e[8:12], 'little')))
    return out

print("=== RAW .pdata ENTRIES for the trampoline functions ===")
for idx, st, en, ui in raw_pdata_entries():
    if 0x1FE0 <= st <= 0x2290:
        print("  idx=%-5d begin=0x%08X end=0x%08X unwindinfo=0x%08X" % (idx, st, en, ui))
print()

def decode_unwind(rva):
    b = read(rva, 64)
    ver = b[0] & 0x7
    flags = b[0] >> 3
    prolog = b[1]
    n = b[2]
    frame = b[3]
    codes = []
    for i in range(n):
        off, op = b[4 + 2*i], b[5 + 2*i]
        code = op & 0xF
        info = op >> 4
        names = {0: "PUSH_NONVOL", 1: "ALLOC_LARGE", 2: "ALLOC_SMALL", 3: "SET_FPREG",
                 4: "SAVE_NONVOL", 5: "SAVE_NONVOL_FAR", 6: "EPILOG", 8: "SAVE_XMM128",
                 9: "SAVE_XMM128_FAR", 10: "PUSH_MACHFRAME"}
        txt = names.get(code, "?")
        if code == 0:
            regs = ["rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi"]
            txt += " " + regs[info]
        elif code == 2:
            txt += " size=0x%X" % (info*8+8)
        elif code == 1:
            txt += " size=0x%X" % (info*8+16)
        codes.append("off %d: %s" % (off, txt))
    return ("ver=%d flags=%d prologsize=%d ncodes=%d framereg/off=0x%02X | " % (ver, flags, prolog, n, frame)
            + " ; ".join(codes))

print("=== DECODED UNWIND INFO (only the 7 real trampoline/slot funcs) ===")
REAL = [(s, e, u) for i, s, e, u in raw_pdata_entries() if i <= 35 and 0x1FE0 <= s <= 0x2290]
for st, en, ui in REAL:
    print("  func 0x%08X-0x%08X  UI@0x%08X  prologsize=%d ncodes=%d raw=%s"
          % (st, en, ui, read(ui, 64)[1], read(ui, 64)[2], read(ui, 12).hex(' ')))
print()

# ---- containing function for each call site ----
def containing(rva):
    best = None
    for s, e, u in PD:
        if s <= rva < e and (best is None or s > best[0]):
            best = (s, e, u)
    return best

SITES = [
    ("servo?on  0x2160", 0x2B7B8),
    ("vibration 0x2240", 0x2B7C1),
    ("viscosity 0x2200", 0x2B7E3),
    ("vibration 0x2240", 0x2B7EC),
    ("vibration 0x2240", 0x2BE91),
    ("torque    0x21C0", 0x2BCDB),
    ("viscosity 0x2200", 0x2BFC1),
    ("servo?off 0x21A0", 0xC053),
]
print("=== CONTAINING FUNCTIONS OF CALL SITES ===")
for lbl, s in SITES:
    c = containing(s)
    print("  %s @0x%08X -> func %s" % (lbl, s, ("0x%08X-0x%08X" % (c[0], c[1])) if c else "NONE"))
print()

for lbl, s in SITES:
    c = containing(s)
    st = c[0] if c else s - 0x100
    print("=== %s  call@0x%08X  (abs 0x%016X)   func 0x%08X-0x%08X ===" % (lbl, s, A(s), st, c[1]))
    # aligned disasm from function start up to site+0x10
    ln = min(s + 0x10 - st, 0x4000)
    txt = []
    for ins in md.disasm(read(st, ln), IB + st):
        rva = ins.address - IB
        if s - 0x60 <= rva <= s + 0x10:
            mark = " <<<" if rva == s else ""
            txt.append("   %08x  %-10s %s%s" % (rva, ins.mnemonic, ins.op_str, mark))
    print("\n".join(txt))
    print()

print("=== ADDRESS-TAKEN CHECK: search .rdata/.data for trampoline addrs ===")
for name, rva in [("servo?on", 0x2160), ("servo?off", 0x21A0), ("torque", 0x21C0),
                  ("viscosity", 0x2200), ("vibration", 0x2240), ("UnBindAll", 0x2020)]:
    absva = struct.pack("<Q", A(rva))
    hits = 0
    for sname, (va, vsz, praw, rsz) in SECT.items():
        blob = DATA[praw:praw+rsz]
        i = blob.find(absva)
        while i >= 0:
            hits += 1
            print("   %s VA 0x%016X found in %s @rva 0x%08X" % (name, A(rva), sname, va + i))
            i = blob.find(absva, i + 1)
    if hits == 0:
        print("   %-10s 0x%08X : no absolute pointer found (not address-taken)" % (name, rva))
