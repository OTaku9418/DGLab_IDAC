"""Pass 8: remaining vibration call sites + authoritative Bind* -> slot offset map."""
import sys
sys.path.insert(0, r"D:\INDAC\DGLab_IDAC\re")
from eng2 import pe, IB, insns, read

print("=" * 100); print("VIBRATION sites 0x2B7C1 and 0x2B7EC (aligned from func 0x148A0)"); print("=" * 100)
for i in insns(0x148A0, 0x17800):
    r = i.address - IB
    if 0x2B7A6 <= r <= 0x2B820:
        print("   %08x  %-22s %-9s %s" % (i.address, i.bytes.hex(' '), i.mnemonic, i.op_str))
print()

print("=" * 100); print("Bind*/UnBind* exports: singleton destination offset (authoritative slot map)"); print("=" * 100)
for exp in pe.DIRECTORY_ENTRY_EXPORT.symbols:
    if not exp.name:
        continue
    n = exp.name.decode()
    if "Bind" not in n and "UnBind" not in n:
        continue
    rva = exp.address
    dest = None
    callee = None
    for i in insns(rva, 0xA0):
        if i.mnemonic == "mov" and i.op_str.startswith("rcx, qword ptr [rip"):
            tgt = i.address + i.size + int(i.op_str.split("+ ")[-1].rstrip("]"), 16)
            if tgt - IB == 0x241890:
                dest = "loads g"
        elif dest == "loads g" and i.mnemonic == "add" and i.op_str.startswith("rcx, 0x"):
            dest = "+" + i.op_str.split(", ")[1]
        elif dest == "loads g" and i.mnemonic == "lea" and "rcx, [rcx + 0x" in i.op_str:
            dest = "+" + i.op_str.split("+ ")[1].rstrip("]")
        if i.mnemonic == "call" and i.op_str.startswith("0x"):
            callee = i.op_str
            if dest is not None:
                break
    slot = "?"
    if isinstance(dest, str) and dest.startswith("+"):
        v = int(dest[1:], 16)
        slot = "slot @+0x%03X  (_Ptr field @+0x%03X)" % (v, v + 0x38)
    print("   %-22s rva 0x%06X  -> %-8s  %-34s helper=%s" % (n[:22], rva, dest, slot, callee))
