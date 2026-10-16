import pefile, capstone
from capstone.x86 import *

DLL = r"D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Plugins\IndRun\Binaries\Win64\IndRun.dll"
pe = pefile.PE(DLL, fast_load=True)
IB = pe.OPTIONAL_HEADER.ImageBase
SECT = {s.Name.rstrip(b"\x00").decode(): (s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData) for s in pe.sections}
TEXT_VA, TEXT_VS, TEXT_PR, TEXT_RS = SECT[".text"]
TEXT = pe.__data__[TEXT_PR:TEXT_PR+TEXT_RS]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True

def read(rva, n):
    if TEXT_VA <= rva < TEXT_VA + TEXT_VS:
        o = rva - TEXT_VA
        if o + n <= len(TEXT): return TEXT[o:o+n]
    for nm,(va,vs,pr,rs) in SECT.items():
        if va <= rva < va + max(vs, rs):
            o = pr + (rva - va)
            if o + n <= len(pe.__data__): return pe.__data__[o:o+n]
    return b""

def dis(rva, ln):
    return list(md.disasm(read(rva, ln), IB + rva))

def show(title, rva, ln):
    print("### %s  RVA 0x%X  abs 0x%X  (%d bytes)" % (title, rva, IB + rva, ln))
    for i in dis(rva, ln):
        print("  %016X  %-30s %s %s" % (i.address, i.bytes.hex(' '), i.mnemonic, i.op_str))
    print()

TARGETS = {0x2160:"servo-ON", 0x21A0:"servo-OFF", 0x21C0:"torque", 0x2200:"viscosity", 0x2240:"vibration", 0x2020:"UnBindAllFB"}

print("ImageBase = 0x%X" % IB)
print("sections:", {k:("VA 0x%X"%v[0]) for k,v in SECT.items()})
G = 0x241890
print("singleton pointer var RVA 0x%X -> abs 0x%X\n" % (G, IB+G))

print("="*100)
for rva, nm in TARGETS.items():
    show(nm, rva, 0x60 if rva != 0x2020 else 0x140)

print("="*100)
print("SINGLETON ACCESS / LAZY ALLOC  0x5E0A0-0x5E0E0, and every site touching 0x241890")
print()
for i in dis(0x5E0A0, 0x50):
    print("  %016X  %-30s %s %s" % (i.address, i.bytes.hex(' '), i.mnemonic, i.op_str))
print()
# every rip-relative reference to 0x241890 in .text
print("--- all RIP-relative sites referencing RVA 0x%X in .text ---" % G)
for i in md.disasm(TEXT, IB + TEXT_VA):
    if i.mnemonic in ("lea","mov","cmp","movzx") and "rip" in i.op_str and i.disp:
        if (i.address + i.size + i.disp) == IB + G:
            print("  RVA 0x%06X  %-26s %s %s" % (i.address - IB, i.bytes.hex(' '), i.mnemonic, i.op_str))
print()

print("="*100)
print("CALL SITES + CONTEXT")
print()
sites = []
for fn, nm in TARGETS.items():
    for i in md.disasm(TEXT, IB + TEXT_VA):
        if i.mnemonic in ("call","jmp","jne","je","jle","jg","jbe","ja","jl","jge") and i.disp and (i.address + i.size + i.disp) == IB + fn:
            sites.append((i.address, fn, nm, i.mnemonic))
seen = set()
for a, fn, nm, mn in sorted(sites):
    key = (a, fn)
    if key in seen: continue
    seen.add(key)
    r = a - IB
    print("--- %s 0x%X  %s at RVA 0x%X (abs 0x%X)" % (nm, fn, mn, r, a))
    for i in dis(r - 0x40, 0x60):
        mark = "  <<<<" if i.address == a else ""
        print("      %016X  %-24s %s %s%s" % (i.address, i.bytes.hex(' '), i.mnemonic, i.op_str, mark))
    print()

print("="*100)
print("BIND*/UNBIND EXPORTS + their slot offsets")
print()
exps = [e for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name and b"FB" in e.name]
for e in sorted(exps, key=lambda x: x.address):
    nm = e.name.decode()
    rva = e.address
    print("EXPORT %-70s RVA 0x%X  abs 0x%X" % (nm, rva, IB + rva))
    for i in dis(rva, 0x70):
        print("      %016X  %-24s %s %s" % (i.address, i.bytes.hex(' '), i.mnemonic, i.op_str))
    print()
