import struct, os, re, sys

DLL = r'D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\Mods\DGLab_IDAC\dlls\main.dll'
UE4SS = r'D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\UE4SS.dll'

def load(p):
    d = open(p,'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe+6)[0]
    opt = pe+24
    magic = struct.unpack_from('<H', d, opt)[0]
    ib = struct.unpack_from('<Q', d, opt+24)[0] if magic==0x20b else struct.unpack_from('<I', d, opt+28)[0]
    secoff = opt + struct.unpack_from('<H', d, pe+20)[0]
    secs=[]
    for i in range(nsec):
        o=secoff+i*40
        nm=d[o:o+8].rstrip(b'\0').decode('latin1')
        vs,va,rs,raw = struct.unpack_from('<IIII', d, o+8)
        secs.append((nm,va,vs,raw,rs))
    # data directories
    ddirs=[]
    for i in range(16):
        rva,sz = struct.unpack_from('<II', d, opt+112+i*8)
        ddirs.append((rva,sz))
    return dict(data=d, ib=ib, secs=secs, ddirs=ddirs)

def r2o(m, rva):
    for nm,va,vs,raw,rs in m['secs']:
        if va <= rva < va+max(vs,rs):
            off = raw + (rva-va)
            if off < len(m['data']): return off
    return None

m = load(DLL)
u = load(UE4SS)
print('=== main.dll ImageBase = 0x%X ; UE4SS.dll ImageBase = 0x%X ===' % (m['ib'], u['ib']))
print()
print('=== main.dll data directories ===')
names=['EXPORT','IMPORT','RESOURCE','EXCEPTION','CERT','BASERELOC','DEBUG','ARCH','GLOBALPTR','TLS','LOADCFG','BOUNDIMP','IAT','DELAYIMP','CLR','RESERVED']
for i,(rva,sz) in enumerate(m['ddirs']):
    if rva: print('  %-10s RVA=0x%06X size=0x%X' % (names[i], rva, sz))
print()
print('=== main.dll 各段 ===')
for nm,va,vs,raw,rs in m['secs']:
    print('  %-9s VA=0x%06X VSize=0x%06X Raw=0x%06X RawSize=0x%06X  %s' % (nm,va,vs,raw,rs,'CODE' if nm=='.text' else 'DATA'))

# ---- 1) Dump the real IAT bytes of the UE4SS.dll import ----
print()
print('=== UE4SS.dll import: IAT RVA 0x250F8, 需要看它每个槽的字节 ===')
off = r2o(m, 0x250F8)
if off:
    print('  file off 0x%X' % off)
    for i in range(10):
        v = struct.unpack_from('<Q', m['data'], off+i*8)[0]
        print('    IAT[%d] RVA 0x%X = 0x%016X' % (i, 0x250F8+i*8, v))

# ---- 2) 崩溃 RAX = main.dll+0x28278 -> 反汇编那段代码 ----
print()
print('=== 崩溃 RAX 指向的代码 main.dll+0x28278 (RVA 0x8278) ===')
off = r2o(m, 0x8278-32)
if off:
    b = m['data'][off:off+96]
    print('  bytes:', ' '.join('%02X'%c for c in b))
import subprocess
