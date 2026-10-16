"""Pass 3: Bind export slot arithmetic, r13d origin, IAT resolution, address-taken scan."""
import sys, struct
sys.path.insert(0, r"D:\INDAC\DGLab_IDAC\re")
from eng2 import pe, IB, md, DATA, SECT, read, cstr, TVA, TVSZ, TRAW, TRSZ, PD, dis, insns, scan_calls

def A(r): return IB + r
pe.parse_data_directories()

print("=" * 100)
print("1) EXPORTS containing 'Bind' or 'UnBind' : slot arithmetic and the std::function slot they touch")
for e in sorted(pe.DIRECTORY_ENTRY_EXPORT.symbols, key=lambda x: x.address):
    if not e.name: continue
    nm = e.name.decode()
    if "Bind" not in nm: continue
    rva = e.address
    print("\n--- %s\n    export RVA 0x%X abs 0x%X" % (nm, rva, A(rva)))
    for i in insns(rva, 0x30):
        print("      %08x  %-26s %-8s %s" % (i.address, i.bytes.hex(' '), i.mnemonic, i.op_str))
        if i.mnemonic == "jmp":
            t = int(i.op_str, 16) - IB
            print("      -> body RVA 0x%X" % t)
            for j in insns(t, 0xA0):
                print("      %08x  %-26s %-8s %s" % (j.address, j.bytes.hex(' '), j.mnemonic, j.op_str))
            break
print()

print("=" * 100)
print("2) r13d ORIGIN: window around RVA 0x2C965 (the only r13d write in the torque/viscosity loop region)")
for i in insns(0x2C920, 0x80):
    print("   %08x  %-26s %-9s %s" % (i.address, i.bytes.hex(' '), i.mnemonic, i.op_str))
print()
print("   all references to disp 0x1678 in func 0x148A0-0x2D274:")
for i in insns(0x148A0, 0x2D274 - 0x148A0):
    if "0x1678" in i.op_str and ("[r" in i.op_str):
        print("   %08x  %-26s %-9s %s" % (i.address, i.bytes.hex(' '), i.mnemonic, i.op_str))
print()

print("=" * 100)
print("3) imports: resolve IAT slots referenced by the trampolines")
for i in insns(0x2160, 0x40):
    if i.mnemonic == "call" and "rip" in i.op_str:
        # compute target
        for op in i.operands:
            if op.type == 3 and op.mem.base == 41:  # RIP
                t = i.address + i.size + op.mem.disp - IB
                print("   %08x call qword ptr [rip+..] -> IAT RVA 0x%X (abs 0x%X)" % (i.address, t, IB + t))
                try:
                    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
                except Exception:
                    pass
                hit = None
                for ent in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
                    for imp in ent.imports:
                        if imp.address - IB == t:
                            hit = "%s!%s" % (ent.dll.decode(), imp.name.decode() if imp.name else "?")
                print("        => %s" % (hit or "(no import match; internal/C++ runtime thunk)"))
print()
print("   full import list (dll -> count):")
for ent in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
    print("     %-24s %d imports" % (ent.dll.decode(), len(ent.imports)))
print()

print("=" * 100)
print("4) ADDRESS-TAKEN scan: does any 8-byte absolute value equal a trampoline VA anywhere in the image?")
for t in (0x2020, 0x2160, 0x21A0, 0x21C0, 0x2200, 0x2240):
    pat = struct.pack("<Q", IB + t)
    hits = []
    st = 0
    while True:
        k = DATA.find(pat, st)
        if k < 0: break
        hits.append(k); st = k + 1
    # map file offset back to RVA
    rv = []
    for h in hits:
        for s in pe.sections:
            if s.PointerToRawData <= h < s.PointerToRawData + s.SizeOfRawData:
                rv.append("%s+0x%X" % (s.Name.rstrip(b'\x00').decode(), h - s.PointerToRawData))
    print("   0x%04X : %d absolute-pointer occurrence(s) %s" % (t, len(hits), rv))
print()

print("=" * 100)
print("5) .pdata / unwind summary for the 7 slots' trampolines + UnBindAllFB")
import collections
for st, en, ui in PD:
    if st in (0x2020, 0x2160, 0x21A0, 0x21C0, 0x2200, 0x2240, 0x2280):
        b = read(ui, 16)
        print("   func 0x%04X-0x%04X  len 0x%-4X  unwind@0x%X  UI bytes=%s  prologsize=%d ncodes=%d"
              % (st, en, en - st, ui, b[:8].hex(' '), b[1], b[2]))
print()
print("=" * 100)
print("6) first-16-byte instruction boundaries (MinHook >=5 byte requirement)")
for nm, t in [("servo-on", 0x2160), ("servo-off", 0x21A0), ("torque", 0x21C0), ("viscosity", 0x2200),
              ("vibration", 0x2240), ("UnBindAllFB", 0x2020)]:
    cum = 0; parts = []
    for i in insns(t, 0x14):
        parts.append("%s %s (%d)" % (i.mnemonic, i.op_str, i.size))
        cum += i.size
        if cum >= 5: break
    print("   0x%04X %-12s first >=5B boundary = %d bytes : %s" % (t, nm, cum, " | ".join(parts)))
print("DONE")
