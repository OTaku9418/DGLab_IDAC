import struct, sys
from capstone import *
from capstone.x86 import *

def load(p):
    d=open(p,'rb').read()
    pe=struct.unpack_from('<I',d,0x3c)[0]
    nsec=struct.unpack_from('<H',d,pe+6)[0]
    opt=pe+24
    magic=struct.unpack_from('<H',d,opt)[0]
    ib=struct.unpack_from('<Q',d,opt+24)[0] if magic==0x20b else struct.unpack_from('<I',d,opt+28)[0]
    so=opt+struct.unpack_from('<H',d,pe+20)[0]
    secs=[]
    for i in range(nsec):
        o=so+i*40
        nm=d[o:o+8].rstrip(b'\0').decode('latin1')
        vs,va,rs,raw=struct.unpack_from('<IIII',d,o+8)
        secs.append((nm,va,vs,raw,rs))
    return dict(data=d,ib=ib,secs=secs)
def r2o(m,rva):
    for nm,va,vs,raw,rs in m['secs']:
        if va<=rva<va+max(vs,rs):
            o=raw+(rva-va)
            if o<len(m['data']): return o
    return None
def dis(m, rva, count=25, back=0):
    off=r2o(m,rva-back)
    if off is None: return []
    code=m['data'][off:off+220]
    md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=True
    out=[]
    for i in md.disasm(code, rva-back):
        out.append(i)
        if len(out)>=count: break
    return out

M=load(r'D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\Mods\DGLab_IDAC\dlls\main.dll')
U=load(r'D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\UE4SS.dll')

print('#### A) main.dll 崩溃时 RAX 指向处 (RVA 0x8278) 的反汇编 ####')
for i in dis(M,0x8278,26):
    print('  %06X  %-30s %s %s' % (i.address,' '.join('%02X'%c for c in i.bytes),i.mnemonic,i.op_str))

print()
print('#### B) UE4SS.dll 崩溃帧返回地址 RVA 0x314883 的反汇编 ####')
for i in dis(U,0x314883,20,back=24):
    mark=' <<<< 崩溃返回地址' if i.address==0x314883 else ''
    print('  %06X  %-30s %s %s%s' % (i.address,' '.join('%02X'%c for c in i.bytes),i.mnemonic,i.op_str,mark))

print()
print('#### C) 崩溃帧其它返回地址 ####')
for rva,lab in [(0x2F4540,'rsp+0x70'),(0x2F2EC0,'rsp+0xE0'),(0x21ED9F,'rsp+0x100'),(0x32857F,'rsp+0x270'),(0xD5BB90,'rsp+0xB8')]:
    print('--- UE4SS+0x%X (%s) ---' % (rva,lab))
    for i in dis(U,rva,6,back=12):
        mark=' <<<' if i.address==rva else ''
        print('   %06X  %-24s %s %s%s' % (i.address,' '.join('%02X'%c for c in i.bytes),i.mnemonic,i.op_str,mark))
