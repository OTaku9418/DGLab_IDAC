import struct
from capstone import *
from capstone.x86 import *
def load(p):
    d=open(p,'rb').read()
    pe=struct.unpack_from('<I',d,0x3c)[0]; nsec=struct.unpack_from('<H',d,pe+6)[0]; opt=pe+24
    magic=struct.unpack_from('<H',d,opt)[0]
    ib=struct.unpack_from('<Q',d,opt+24)[0] if magic==0x20b else struct.unpack_from('<I',d,opt+28)[0]
    so=opt+struct.unpack_from('<H',d,pe+20)[0]; secs=[]
    for i in range(nsec):
        o=so+i*40; nm=d[o:o+8].rstrip(b'\0').decode('latin1')
        vs,va,rs,raw=struct.unpack_from('<IIII',d,o+8); secs.append((nm,va,vs,raw,rs))
    return dict(data=d,ib=ib,secs=secs)
def r2o(m,rva):
    for nm,va,vs,raw,rs in m['secs']:
        if va<=rva<va+max(vs,rs):
            o=raw+(rva-va)
            if o<len(m['data']): return o
    return None
M=load(r'D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\Mods\DGLab_IDAC\dlls\main.dll')
U=load(r'D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\UE4SS.dll')

print('=== 局部变量那两个字符串常量是什么? ===')
for rva,lab in [(0xF16D70-0x100000,'?'),]: pass
for ins_rva,lab in [(0x314854,'r9 (第4个实参)'),(0x31485B,'r8 (第3个实参)')]:
    o=r2o(U,ins_rva)
    disp=struct.unpack_from('<i',U['data'],o+3)[0]
    tgt=ins_rva+7+disp
    off=r2o(U,tgt)
    raw=U['data'][off:off+120] if off else b''
    print('  %s: RVA 0x%X -> 目标 RVA 0x%X' % (lab,ins_rva,tgt))
    print('     原始:', raw[:80])
    print('      宽字符:', repr(raw[:80].decode('utf-16-le','replace').split('\x00')[0][:40]))
    print('      窄字符:', repr(raw[:60].decode('latin1').split('\x00')[0][:40]))

print()
print('=== UE4SS+0x947f60 是什么 (第一个调用) ===')
o=r2o(U,0x947f60)
md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=True
for i in list(md.disasm(U['data'][o:o+140],0x947f60))[:30]:
    print('  %06X  %-26s %s %s' % (i.address,' '.join('%02X'%c for c in i.bytes),i.mnemonic,i.op_str))

print()
print('=== 我们自己的 DLL: 有没有把 0x6575 当常量/指针用 ===')
for nm,va,vs,raw,rs in M['secs']:
    blob=M['data'][raw:raw+rs]
    for pat,pname in [(b'\x75\x65\x00\x00','uint32 0x00006575'),(b'\x75\x65','0x6575 little-endian 2B')]:
        idx=0; n=0
        while True:
            j=blob.find(pat,idx)
            if j<0 or n>=6: break
            print('  %s @ %s RVA 0x%06X' % (pname,nm,va+j)); idx=j+1; n+=1
