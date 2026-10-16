import pefile, sys, json

P = r"D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Plugins\IndRun\Binaries\Win64\IndRun.dll"
pe = pefile.PE(P, fast_load=True)
pe.parse_data_directories()

print("=== HEADER ===")
print("Machine      : %04x" % pe.FILE_HEADER.Machine)
print("TimeDateStamp: %08x" % pe.FILE_HEADER.TimeDateStamp)
print("Characteristics: %04x" % pe.FILE_HEADER.Characteristics)
print("ImageBase    : %016x" % pe.OPTIONAL_HEADER.ImageBase)
print("EntryPoint   : %08x (RVA)" % pe.OPTIONAL_HEADER.AddressOfEntryPoint)
print("SizeOfImage  : %x" % pe.OPTIONAL_HEADER.SizeOfImage)
print("Subsystem    : %d" % pe.OPTIONAL_HEADER.Subsystem)
print("DllCharacteristics: %04x" % pe.OPTIONAL_HEADER.DllCharacteristics)

print("\n=== SECTIONS ===")
for s in pe.sections:
    print("%-8s VA=%08x VSZ=%08x RAW=%08x RSZ=%08x CH=%08x" % (
        s.Name.rstrip(b"\x00").decode(errors="replace"),
        s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData,
        s.SizeOfRawData, s.Characteristics))

print("\n=== DATA DIRECTORIES ===")
names = ["EXPORT","IMPORT","RESOURCE","EXCEPTION","SECURITY","BASERELOC","DEBUG",
         "ARCH","GLOBALPTR","TLS","LOADCFG","BOUNDIMP","IAT","DELAYIMP",
         "CLR","RESERVED"]
for i, d in enumerate(pe.OPTIONAL_HEADER.DATA_DIRECTORY):
    n = names[i] if i < len(names) else str(i)
    print("  [%2d] %-10s RVA=%08x SIZE=%08x" % (i, n, d.VirtualAddress, d.Size))

# ---------- imports ----------
print("\n=== IMPORTS ===")
imps = []
try:
    ents = pe.DIRECTORY_ENTRY_IMPORT
except AttributeError:
    ents = None
    print("  (!) DIRECTORY_ENTRY_IMPORT missing; manual parse")
    dd = pe.OPTIONAL_HEADER.DATA_DIRECTORY[1]
    if dd.VirtualAddress:
        off = pe.get_offset_from_rva(dd.VirtualAddress)
        while True:
            d = pe.__data__[off:off+20]
            if len(d) < 20 or d == b"\x00"*20:
                break
            oft, ts, fc, nrva, fthunk = (
                int.from_bytes(d[0:4],'little'), d[4:8],
                int.from_bytes(d[8:12],'little'),
                int.from_bytes(d[12:16],'little'),
                int.from_bytes(d[16:20],'little'))
            if nrva == 0:
                break
            print("   DLL RVA %08x name=%08x" % (nrva, oft))
            off += 20
if ents:
    for e in ents:
        print("  %s (%d imports)" % (e.dll.decode(errors="replace"), len(e.imports)))
        for imp in e.imports:
            nm = imp.name.decode(errors="replace") if imp.name else ("ord#%d" % imp.ordinal)
            imps.append((e.dll.decode(errors="replace"), nm, imp.address))
            print("      %08x %s" % (imp.address, nm))

print("\n=== DELAY IMPORTS ===")
try:
    for e in pe.DIRECTORY_ENTRY_DELAY_IMPORT:
        print("  %s" % e.dll.decode(errors="replace"))
        for imp in e.imports:
            nm = imp.name.decode(errors="replace") if imp.name else ("ord#%d" % imp.ordinal)
            print("      %s" % nm)
except AttributeError:
    print("  (none)")

# ---------- exports ----------
print("\n=== EXPORTS ===")
exps = []
try:
    syms = pe.DIRECTORY_ENTRY_EXPORT.symbols
    print("  count=%d  dllname=%s" % (len(syms), pe.DIRECTORY_ENTRY_EXPORT.name))
    for s in syms:
        nm = s.name.decode(errors="replace") if s.name else None
        exps.append({"ord": s.ordinal, "rva": s.address, "name": nm,
                     "fwd": s.forwarder.decode(errors="replace") if s.forwarder else None})
except AttributeError as ex:
    print("  (!) no export dir:", ex)

for e in exps:
    print("  ord=%4d rva=%08x %s%s" % (e["ord"], e["rva"],
          e["name"] or "(noname)", " -> "+e["fwd"] if e["fwd"] else ""))

json.dump({"exports": exps, "imports": imps},
          open(r"D:\INDAC\DGLab_IDAC\re\recon1.json","w"), indent=1)
print("\nwrote recon1.json")
