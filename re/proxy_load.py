"""Disassemble the proxy's UE4SS-loading code path."""
import pefile, capstone

P = r"D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\dwmapi.dll"
pe = pefile.PE(P, fast_load=True)
ib = pe.OPTIONAL_HEADER.ImageBase
for s in pe.sections:
    if s.Name.rstrip(b"\0") == b".text":
        text = s
        break
code = text.get_data()
base = ib + text.VirtualAddress

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True

def dis(rva_from, rva_to, label):
    print(f"\n{'='*78}\n== {label}   (rva 0x{rva_from:X} .. 0x{rva_to:X})\n{'='*78}")
    start = ib + rva_from
    end = ib + rva_to
    off = rva_from - text.VirtualAddress
    for insn in md.disasm(code[off:off + (rva_to - rva_from)], start):
        raw = " ".join(f"{b:02X}" for b in insn.bytes)
        print(f"  0x{insn.address - ib:06X}  {raw:<20s} {insn.mnemonic:<8s} {insn.op_str}")

# usage/help parser region (mentions --ue4ss-path / Failed to load proxy DLL)
dis(0x43A0, 0x4100 if False else 0x4180, "arg parsing: --ue4ss-path + 'Failed to load proxy DLL'")
# the dialog that we actually saw
dis(0x6D40, 0x6E20, "dialog: 'Failed to load UE4SS.dll' / '--disable-ue4ss'")
# where "UE4SS.dll" itself is referenced
dis(0x4BF0, 0x4C80, "reference to L\"UE4SS.dll\"")
