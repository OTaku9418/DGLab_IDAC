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
def funcs(m,rva,size):
    o=r2o(m,rva); out=[]
    for i in range(size//12):
        b,e,u=struct.unpack_from('<III',m['data'],o+i*12)
        if b==0: break
        out.append((b,e,u))
    return out
def ff(fl,r):
    for b,e,u in fl:
        if b<=r<e: return (b,e,u)
    return None
def dis(m,rva,n,back=0):
    o=r2o(m,rva-back)
    md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=True
    return list(md.disasm(m['data'][o:o+n], rva-back))

M=load(r'D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\Mods\DGLab_IDAC\dlls\main.dll')
U=load(r'D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\UE4SS.dll')
d=U['data']; pe=struct.unpack_from('<I',d,0x3c)[0]; opt=pe+24
uf=funcs(U,*struct.unpack_from('<II',d,opt+112+3*8))

print('#### 崩溃所在函数 UE4SS+0x314850 全文 ####')
g=ff(uf,0x314850)
print('  函数范围 0x%X..0x%X (%d 字节)' % (g[0],g[1],g[1]-g[0]))
for i in dis(U,g[0],g[1]-g[0]+8):
    mk=' <<<<< 崩溃在这里' if i.address==0x314883 else ''
    print('  %06X  %-28s %s %s%s' % (i.address,' '.join('%02X'%c for c in i.bytes),i.mnemonic,i.op_str,mk))

print()
print('#### 调用者 UE4SS+0x3147D0 尾部(它如何遍历 mod 列表) ####')
for i in dis(U,0x314830,0x60):
    print('  %06X  %-28s %s %s' % (i.address,' '.join('%02X'%c for c in i.bytes),i.mnemonic,i.op_str))

print()
print('#### main.dll 的 vtable / 0x6575 线索 ####')
# scan main.dll for the 2-byte pattern 75 65 appearing as data in .rdata (wide "ue")
for nm,va,vs,raw,rs in M['secs']:
    if nm not in ('.rdata','.data'): continue
    blob=M['data'][raw:raw+rs]
    idx=0; found=[]
    while True:
        j=blob.find(b'\x75\x00\x65\x00', idx)   # L"ue" UTF-16LE
        if j<0: break
        found.append(va+j); idx=j+1
        if len(found)>20: break
    if found:
        print('  %s 里 L"ue" 出现 %d 处:' % (nm,len(found)))
        for r in found[:12]:
            o=r2o(M,r)
            s=M['data'][o:o+40]
            try: txt=s.decode('utf-16-le','replace').split('\x00')[0]
            except: txt='?'
            print('     RVA 0x%06X  %r' % (r, txt[:36]))

# our vtable: look for the export/class vtable via our ctor RVA
print()
print('#### main.dll 里所有指向 .text 的函数指针表 (vtable 候选) ####')
for nm,va,vs,raw,rs in M['secs']:
    if nm!='.rdata': continue
    blob=M['data'][raw:raw+rs]
    cur=[]
    for i in range(0, len(blob)-8, 8):
        v=struct.unpack_from('<Q',blob,i)[0]
        if 0x1000 <= v < 0x24ADF:
            cur.append((va+i, v))
        else:
            if len(cur)>=6:
                print('  表 @ RVA 0x%06X, %d 项:' % (cur[0][0], len(cur)))
                for a,t in cur: print('      [%06X] -> %06X' % (a,t))
            cur=[]
