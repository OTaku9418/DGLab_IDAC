import struct
from capstone import *
from capstone.x86 import *

def load(p):
    d=open(p,'rb').read()
    pe=struct.unpack_from('<I',d,0x3c)[0]
    nsec=struct.unpack_from('<H',d,pe+6)[0]; opt=pe+24
    magic=struct.unpack_from('<H',d,opt)[0]
    ib=struct.unpack_from('<Q',d,opt+24)[0] if magic==0x20b else struct.unpack_from('<I',d,opt+28)[0]
    so=opt+struct.unpack_from('<H',d,pe+20)[0]
    secs=[]
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
def funcs(m, exc_rva, exc_size):
    """parse RUNTIME_FUNCTION table -> list of (begin,end,unwind)"""
    out=[]; o=r2o(m,exc_rva)
    if o is None: return out
    for i in range(exc_size//12):
        b,e,u=struct.unpack_from('<III',m['data'],o+i*12)
        if b==0: break
        out.append((b,e,u))
    return out
def find_func(fl,rva):
    for b,e,u in fl:
        if b<=rva<e: return (b,e,u)
    return None

M=load(r'D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\Mods\DGLab_IDAC\dlls\main.dll')
U=load(r'D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\UE4SS.dll')

print('=== main.dll: 崩溃 RAX = RVA 0x8278 落在哪个函数里? ===')
mf=funcs(M,0x5A000,0x1674)
print('main.dll RUNTIME_FUNCTION 数量 =', len(mf))
f=find_func(mf,0x8278)
print('  函数范围: 0x%X .. 0x%X (unwind info RVA 0x%X)' % f if f else '  不在任何函数里!')
if f:
    print('  崩溃点在函数内偏移 +0x%X / 共 0x%X 字节' % (0x8278-f[0], f[1]-f[0]))
    o=r2o(M,f[0])
    md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=True
    print('  该函数开头 12 条:')
    for i in list(md.disasm(M['data'][o:o+80], f[0]))[:12]:
        print('    %06X  %-24s %s %s' % (i.address,' '.join('%02X'%c for c in i.bytes),i.mnemonic,i.op_str))

print()
print('=== UE4SS.dll: 崩溃循环所在函数 (RVA 0x3147D0) ===')
uf=funcs(U,0,0)  # need exc dir; read from headers instead
# read exception dir from UE4SS headers
d=U['data']
pe=struct.unpack_from('<I',d,0x3c)[0]; opt=pe+24
exc_rva,exc_size=struct.unpack_from('<II',d,opt+112+3*8)
print('UE4SS EXCEPTION dir RVA=0x%X size=0x%X' % (exc_rva,exc_size))
uf=funcs(U,exc_rva,exc_size)
print('UE4SS RUNTIME_FUNCTION 数量 =', len(uf))
for target,lab in [(0x3147D0,'循环函数'),(0x314850,'循环体'),(0x314883,'崩溃返回点'),(0x2F4540,'外层调用者'),(0x2F2F90,'被调用'),(0x32857F,'更外层')]:
    g=find_func(uf,target)
    print('  %-14s RVA 0x%X -> %s' % (lab,target,('函数 0x%X..0x%X' % (g[0],g[1])) if g else '<不在函数表里>'))

print()
print('=== 找到循环函数的起点, 反汇编看它遍历什么 ===')
g=find_func(uf,0x314850)
if g:
    o=r2o(U,g[0]); md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=True
    n=(0x3148A0-g[0])
    print('  函数 0x%X 开始, 到崩溃点共 0x%X 字节:' % (g[0], n))
    for i in list(md.disasm(U['data'][o:o+n+16], g[0]))[:34]:
        mk=' <<<< 崩溃' if i.address==0x314883 else ''
        print('    %06X  %-26s %s %s%s' % (i.address,' '.join('%02X'%c for c in i.bytes),i.mnemonic,i.op_str,mk))

print()
print('=== UE4SS.dll 里所有 [reg+0x78] 的 call (候选 vtable 槽 0x78) ===')
md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=True
o=r2o(U,0x1000)
code=U['data'][o:o+0x400000]
hits=0
for i in md.disasm(code,0x1000):
    if i.mnemonic=='call' and len(i.operands)==1 and i.operands[0].type==X86_OP_MEM:
        mem=i.operands[0].mem
        if mem.disp==0x78 and mem.base!=0:
            hits+=1
            if hits<=8:
                print('   %06X  call qword ptr [%s+0x78]' % (i.address, i.reg_name(mem.base)))
print('   共 %d 处' % hits)
