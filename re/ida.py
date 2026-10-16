import pefile, capstone, sys, re

P = r"D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Plugins\IndRun\Binaries\Win64\IndRun.dll"
pe = pefile.PE(P, fast_load=True)
pe.parse_data_directories()
IB = pe.OPTIONAL_HEADER.ImageBase
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True

def sec_of(rva):
    for s in pe.sections:
        va = s.VirtualAddress
        if va <= rva < va + max(s.Misc_VirtualSize, s.SizeOfRawData):
            return s
    return None

def rva2off(rva):
    try:
        return pe.get_offset_from_rva(rva)
    except Exception:
        return None

def read(rva, n):
    o = rva2off(rva)
    if o is None: return b""
    return pe.__data__[o:o+n]

def dis(rva, n, stop_ret=True, maxins=400):
    """Disassemble n bytes at rva; annotate rip-relative + string refs."""
    code = read(rva, n)
    out = []
    cnt = 0
    for ins in md.disasm(code, IB + rva):
        ann = ""
        try:
            for op in ins.operands:
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                    tgt = ins.address + ins.size + op.mem.disp
                    trva = tgt - IB
                    s = read(trva, 96)
                    txt = ""
                    if s and all(32 <= c < 127 or c in (9,10,13) for c in s[:40]) and len(s) >= 2:
                        txt = s.split(b"\x00")[0][:80].decode(errors="replace")
                    ann += "   ; [%08x]" % trva
                    if txt:
                        ann += ' "%s"' % txt
                    else:
                        # dump qwords
                        q = read(trva, 16)
                        if q:
                            ann += " q=" + " ".join("%016x" % int.from_bytes(q[i:i+8],'little') for i in (0,8) if len(q)>=i+8)
        except Exception:
            pass
        out.append("  %08x  %-8s %s%s" % (ins.address, ins.mnemonic, ins.op_str, ann))
        cnt += 1
        if ins.mnemonic == "ret" and stop_ret:
            out.append("  ---- ret ----")
            break
        if cnt > maxins:
            out.append("  ... truncated")
            break
    return "\n".join(out)

if __name__ == "__main__":
    for arg in sys.argv[1:]:
        if "-" in arg:
            a, b = arg.split("-")
            rva = int(a, 16); end = int(b, 16)
        else:
            rva = int(arg, 16); end = rva + 400
        print("=== RVA %08x .. %08x ===" % (rva, end))
        print(dis(rva, end - rva, stop_ret=False))
        print()
