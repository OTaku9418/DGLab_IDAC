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
IAT={}
for entry in pe.DIRECTORY_ENTRY_IMPORT:
    dl=entry.dll.decode(errors='replace')
    for imp in entry.imports:
        if imp.name: IAT[imp.address-IB]=(dl,imp.name.decode(errors='replace'))
md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=True
def desc(rva):
    o=off(rva)
    if o is None: return "?"
    for insn in md.disasm(BLOB[o:o+16],rva):
        if insn.mnemonic=='jmp' and 'rip' in insn.op_str:
            ops=insn.operands
            if len(ops)==1 and ops[0].type==3:
                t=insn.address+insn.size+ops[0].mem.disp; tr=t-IB
                if tr in IAT: return "thunk -> %s!%s"%IAT[tr]
                return "jmp %#x"%t
        if insn.mnemonic=='ret': return "ret"
    return "code"
def find_vt():
    for n,va,rs,praw,data in SECS:
        if n!='.rdata' or not data: continue
        run=[]
        for i in range(0,len(data)-8,8):
            v=struct.unpack_from('<Q',data,i)[0]; r=v-IB
            if 0x1000<=r<0x24ADF: run.append((va+i,r))
            else:
                if len(run)>=8: return run
                run=[]
    return None
run=find_vt()
print("=== 重编后 DGLabIdacMod 虚表 @ RVA %#x ==="%run[0][0])
for k,(a,r) in enumerate(run):
    print("  [%2d] +%#04x  RVA %#010x   %s"%(k,k*8,r,desc(r)))
