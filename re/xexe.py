"""Scan the 69MB game EXE for code referencing given strings/data."""
import pefile, numpy as np, re, sys
from collections import defaultdict

P = r"D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\GameProject-Win64-Shipping.exe"
pe = pefile.PE(P, fast_load=True)
pe.parse_data_directories()
IB = pe.OPTIONAL_HEADER.ImageBase
D = pe.__data__
SEC = [(s.Name.rstrip(b'\0').decode(), s.VirtualAddress, s.Misc_VirtualSize,
        s.PointerToRawData, s.SizeOfRawData) for s in pe.sections]

def off2va(o):
    for n, va, vsz, praw, rsz in SEC:
        if praw <= o < praw + rsz:
            return IB + va + (o - praw)
    return None
def va2off(v):
    r = v - IB
    for n, va, vsz, praw, rsz in SEC:
        if va <= r < va + vsz:
            return praw + (r - va)
    return None
def readva(v, n):
    o = va2off(v)
    return D[o:o+n] if o is not None else b""
def cstrva(v, maxn=160):
    d = readva(v, maxn)
    z = d.find(b'\0')
    return d[:z].decode('ascii', 'replace') if z > 0 else None

TXT = [s for s in SEC if s[0] == '.text'][0]
_, TVA, TVSZ, TRAW, TRSZ = TXT
print("[i] .text va=%08x vsz=%08x raw=%08x rsz=%08x" % (TVA, TVSZ, TRAW, TRSZ), file=sys.stderr)
from numpy.lib.stride_tricks import sliding_window_view
BLOB = np.frombuffer(D[TRAW:TRAW+TRSZ], dtype=np.uint8)
N = len(BLOB)
IDX = np.arange(N - 3, dtype=np.int64)
DISP = sliding_window_view(BLOB, 4).view('<i4')[:, 0].astype(np.int64)

def xrefs_to(va):
    """Return list of RVAs i where an instruction's disp32 field starts at i and resolves to va."""
    tgt_rva = va - IB
    # tgt_rva = (TVA + i) + 4 + disp  =>  disp = tgt_rva - TVA - i - 4
    want = tgt_rva - TVA - 4
    hits = np.nonzero(DISP == (want - IDX))[0]
    return [int(TVA + h) for h in hits]

if __name__ == "__main__":
    if sys.argv[1] == "find":
        for pat in sys.argv[2:]:
            pb = pat.encode()
            for m in re.finditer(re.escape(pb), D):
                o = m.start()
                print("STR %-40s file=%08x va=%s" % (pat[:40], o, hex(off2va(o)) if off2va(o) else "?"))
    elif sys.argv[1] == "xrefstr":
        for pat in sys.argv[2:]:
            pb = pat.encode()
            seen = set()
            for m in re.finditer(re.escape(pb), D):
                o = m.start(); va = off2va(o)
                if va is None or va in seen: continue
                seen.add(va)
                rv = xrefs_to(va)
                print("=== %s @va %x  (%d code xrefs) ===" % (pat[:50], va, len(rv)))
                for r in rv[:20]:
                    o2 = va2off(IB + r)
                    print("    site rva %08x  bytes: %s" % (r, D[o2-8:o2+16].hex()))
    elif sys.argv[1] == "xrefva":
        for a in sys.argv[2:]:
            va = int(a, 16)
            rv = xrefs_to(va)
            print("=== va %x : %d xrefs ===" % (va, len(rv)))
            for r in rv[:30]:
                o2 = va2off(IB + r)
                print("    site rva %08x  bytes: %s" % (r, D[o2-8:o2+16].hex()))
