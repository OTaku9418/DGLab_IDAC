import struct, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
DLL=r"D:\INDAC\DGLab_IDAC\build\Release\DGLab_IDAC.dll"
pe=pefile.PE(DLL); IB=pe.OPTIONAL_HEADER.ImageBase
BLOB=open(DLL,'rb').read()
SECS=[(s.Name.rstrip(b'\0').decode(),s.VirtualAddress,s.SizeOfRawData,s.PointerToRawData,s.get_data()) for s in pe.sections]
def off(rva):
    for n,va,rs,praw,d in SECS:
        if d and va<=rva<va+max(rs,len(d)): return praw+(rva-va)
    return None
# 导出
EXP={}
for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
    if e.name: EXP[e.address]=e.name.decode(errors='replace')
# 导入表: IAT rva -> 名字
IAT={}
try:
    for entry in pe.DIRECTORY_ENTRY_IMPORT:
        dll=entry.dll.decode(errors='replace')
        for imp in entry.imports:
            if imp.name:
                IAT[imp.address-IB]="%s!%s"%(dll,imp.name.decode(errors='replace'))
except Exception as ex: print("import err",ex)
md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=True
def desc(rva):
    o=off(rva)
    if o is None: return "<no bytes>"
    out=[]
    for insn in md.disasm(BLOB[o:o+16],rva):
        out.append("%s %s"%(insn.mnemonic,insn.op_str))
        if insn.mnemonic in ('jmp','ret'): break
        if len(out)>=3: break
    return " ; ".join(out)
VT=0x28278
print("=== 虚表 RVA 0x%x 逐槽解析 ==="%VT)
for k in range(10):
    v=struct.unpack_from('<Q',BLOB,off(VT+8*k))[0]; r=v-IB
    tag=""
    if 0x25000<=r<0x54000:      # .rdata
        tgt=struct.unpack_from('<Q',BLOB,off(r))[0]
        t2=tgt-IB
        if t2 in IAT: tag="  ==> 指向 IAT: "+IAT[t2]
        else: tag="  ==> .rdata 数据 %#x"%tgt
    if r in EXP: tag+="  [导出 %s]"%EXP[r]
    print("  [%d] +%#04x RVA %#010x   %-46s %s"%(k,k*8,r,desc(r)[:46],tag))
