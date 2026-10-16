"""Dump the UE4SS proxy (dwmapi.dll) strings properly + find what path it builds for UE4SS.dll."""
import pefile, capstone, re, sys

P = r"D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\dwmapi.dll"
pe = pefile.PE(P, fast_load=True)
pe.parse_data_directories()
ib = pe.OPTIONAL_HEADER.ImageBase
print(f"ImageBase 0x{ib:x}  SizeOfImage 0x{pe.OPTIONAL_HEADER.SizeOfImage:x}")
print(f"DllCharacteristics 0x{pe.OPTIONAL_HEADER.DllCharacteristics:x}")
for s in pe.sections:
    print(f"  {s.Name.decode(errors='replace').strip(chr(0)):10s} VA 0x{s.VirtualAddress:X} VSize 0x{s.Misc_VirtualSize:X} Raw 0x{s.PointerToRawData:X} RawSize 0x{s.SizeOfRawData:X}")

# ---- UTF-16 strings (the interesting ones are wide) ----
print("\n=== UTF-16LE strings len>=4 ===")
data = open(P, "rb").read()
for m in re.finditer(rb"(?:[\x20-\x7e]\x00){4,}", data):
    off = m.start()
    txt = m.group().decode("utf-16-le")
    # map file offset -> RVA
    rva = None
    for s in pe.sections:
        if s.PointerToRawData <= off < s.PointerToRawData + s.SizeOfRawData:
            rva = s.VirtualAddress + (off - s.PointerToRawData)
            break
    print(f"  0x{off:06X} (rva 0x{rva:06X})  {txt!r}" if rva is not None else f"  0x{off:06X}  {txt!r}")

# ---- ASCII strings len>=5 ----
print("\n=== ASCII strings len>=5 ===")
for m in re.finditer(rb"[\x20-\x7e]{5,}", data):
    off = m.start()
    txt = m.group().decode("ascii")
    rva = None
    for s in pe.sections:
        if s.PointerToRawData <= off < s.PointerToRawData + s.SizeOfRawData:
            rva = s.VirtualAddress + (off - s.PointerToRawData)
            break
    if rva is not None:
        print(f"  0x{off:06X} (rva 0x{rva:06X})  {txt!r}")

# ---- imports ----
print("\n=== imports ===")
if hasattr(pe, "DIRECTORY_ENTRY_IMPORT"):
    for e in pe.DIRECTORY_ENTRY_IMPORT:
        names = [i.name.decode() if i.name else f"ord{i.ordinal}" for i in e.imports]
        print(f"  {e.dll.decode():20s} {names}")

# ---- exports ----
print("\n=== exports ===")
if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
    for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        print(f"  rva 0x{e.address:06X}  {e.name.decode() if e.name else '(ord %d)' % e.ordinal}")
