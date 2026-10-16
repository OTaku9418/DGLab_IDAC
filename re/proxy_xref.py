"""Find, in the UE4SS proxy dwmapi.dll, how it resolves the UE4SS.dll path.

Targets the two wide strings that build the path:
  rva 0x9CC8  L"UE4SS.dll"
  rva 0x9BE8  L"--ue4ss-path"
and the failure dialog at rva 0x9CE0.
"""
import pefile, capstone, struct

P = r"D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\dwmapi.dll"
pe = pefile.PE(P, fast_load=True)
ib = pe.OPTIONAL_HEADER.ImageBase

TARGETS = {
    0x9CC8: 'L"UE4SS.dll"',
    0x9BE8: 'L"--ue4ss-path"',
    0x9BC0: 'L"--disable-ue4ss"',
    0x9B70: 'L"UE4SS Error"',
    0x9B88: 'L"Failed to load proxy DLL"',
    0x9CE0: 'L"Failed to load UE4SS.dll..."',
}
text = None
for s in pe.sections:
    if s.Name.rstrip(b"\0") == b".text":
        text = s
        break
code = text.get_data()
base_rva = text.VirtualAddress
print(f".text rva 0x{base_rva:X} size 0x{len(code):X}\n")

# --- find LEA r64, [rip+disp32] whose target is one of our strings ---
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True
disp_by_target = {}
for insn in md.disasm(code, ib + base_rva):
    if insn.mnemonic != "lea":
        continue
    for op in insn.operands:
        if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
            target = insn.address + insn.size + op.mem.disp
            rva = target - ib
            if rva in TARGETS:
                disp_by_target.setdefault(rva, []).append((insn.address - ib, insn.mnemonic + " " + insn.op_str))

for rva, label in TARGETS.items():
    hits = disp_by_target.get(rva, [])
    print(f"{label}  (rva 0x{rva:X}) -> {len(hits)} LEA reference(s)")
    for site, txt in hits:
        print(f"    rva 0x{site:06X}: {txt}")
    print()

# --- also: any immediate 0x9CC8-like value, and any 2-byte 'UNICODE' marker ---
print("=== search for the literal bytes 'UE4SS.dll' anywhere (wide & ascii) ===")
raw = open(P, "rb").read()
for pat, name in ((b"U\x00E\x004\x00S\x00S\x00", "wide"), (b"UE4SS", "ascii")):
    off = raw.find(pat)
    while off != -1:
        rva = None
        for s in pe.sections:
            if s.PointerToRawData <= off < s.PointerToRawData + s.SizeOfRawData:
                rva = s.VirtualAddress + (off - s.PointerToRawData)
        print(f"  {name} at file 0x{off:X} rva 0x{rva:X}" if rva else f"  {name} at file 0x{off:X}")
        off = raw.find(pat, off + 1)
