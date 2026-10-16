import struct
from capstone import *
from capstone.x86 import *
U=r'D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\UE4SS.dll'
d=open(U,'rb').read()
pe=struct.unpack_from('<I',d,0x3c)[0]; nsec=struct.unpack_from('<H',d,pe+6)[0]; opt=pe+24
so=opt+struct.unpack_from('<H',d,pe+20)[0]; secs=[]
for i in range(nsec):
    o=so+i*40; nm=d[o:o+8].rstrip(b'\0').decode('latin1')
    vs,va,rs,raw=struct.unpack_from('<IIII',d,o+8); secs.append((nm,va,vs,raw,rs))
def r2o(rva):
    for nm,va,vs,raw,rs in secs:
        if va<=rva<va+max(vs,rs):
            o=raw+(rva-va)
            if o<len(d): return o
    return None
print('=== 第 0 个调用 UE4SS+0x947f60 是什么函数 (找 0x1000-0xA00000 内对它的 call) ===')
md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=True
o=r2o(0x1000)
callers=[]
for i in md.disasm(d[o:o+0x200000],0x1000):
    if i.mnemonic=='call' and i.operands and i.operands[0].type==X86_OP_IMM:
        if i.operands[0].imm==0x947f60:
            callers.append(i.address)
print('  被 call 的位置:', ['0x%X'%c for c in callers[:20]], '共',len(callers))
# and find its prologue
o=r2o(0x947f60)
print('  0x947f60 前 24 条:')
for i in list(md.disasm(d[o:o+160],0x947f60))[:24]:
    s=''
    # annotate rip-relative targets
    if 'rip' in i.op_str:
        for op in i.operands:
            if op.type==X86_OP_MEM and op.mem.base!=0 and i.reg_name(op.mem.base)=='rip':
                t=i.address+i.size+op.mem.disp
                off=r2o(t)
                if off:
                    raw=d[off:off+48]
                    try: txt=raw.decode('latin1').split('\x00')[0]
                    except: txt=''
                    if txt and all(32<=ord(c)<127 for c in txt[:8]): s='  ; "%s"' % txt[:40]
    print('   %06X  %-26s %s %s%s' % (i.address,' '.join('%02X'%c for c in i.bytes),i.mnemonic,i.op_str,s))

print()
print('=== UE4SS.dll 是否导出 __RTDynamicCast / dynamic_cast 相关 ===')
for pat in [b'__RTDynamicCast', b'__CxxFrameHandler', b'bad_cast', b'__RTtypeid']:
    print('  %-20s 出现 %d 次' % (pat.decode(), d.count(pat)))
print('  UE4SS 的 IAT 里有没有 msvcp140 的 __RTDynamicCast:')
print('  msvcp140 导入符号 __RTDynamicCast:', d.count(b'__RTDynamicCast'))
