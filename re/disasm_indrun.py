#!/usr/bin/env python3
"""
反汇编 IndRun.dll 的 5 个候选 invoker, 确认它们真的是
"取 std::function 槽位 -> 通过 vtable[0x10] 调用" 的转发器。

这一步在修正特征码之前做: 特征码是防止挂错地址的最后一道闸,
它本身错了, 就必须用反汇编独立确认 RVA 是对的。

用法: python re/disasm_indrun.py [IndRun.dll 路径]
"""
import sys
import struct

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
except ImportError:
    raise SystemExit("需要 capstone: pip install capstone")

DEFAULT = (r"D:\INDAC\MODAC321\WindowsNoEditor\GameProject"
           r"\Plugins\IndRun\Binaries\Win64\IndRun.dll")

# 候选 RVA (来自 indrun_hook_source.cpp) + 期望原型
TARGETS = [
    (0x2160, "servo-on",  "void(void)"),
    (0x21A0, "servo-off", "void(void)"),
    (0x21C0, "torque",    "void(int32 dir /*rcx*/, int32 mag /*rdx*/)"),
    (0x2200, "viscosity", "void(int32 /*rcx*/, int32 /*rdx*/)"),
    (0x2240, "vibration", "void(int32 /*rcx*/, int32 /*rdx*/)"),
]


def parse_pe(path):
    with open(path, "rb") as f:
        data = f.read()
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    coff = e_lfanew + 4
    n_sections = struct.unpack_from("<H", data, coff + 2)[0]
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    image_base = struct.unpack_from("<Q", data, coff + 20 + 0x18)[0]
    sec_off = coff + 20 + opt_size
    sections = []
    for i in range(n_sections):
        s = sec_off + i * 40
        name = data[s:s + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, vaddr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, s + 8)
        sections.append((name, vaddr, vsize, raw_ptr, raw_size))
    return data, image_base, sections


def rva_to_off(sections, rva):
    for name, vaddr, vsize, raw_ptr, raw_size in sections:
        if vaddr <= rva < vaddr + max(vsize, raw_size):
            return raw_ptr + (rva - vaddr), name
    return None, None


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT
    data, image_base, sections = parse_pe(path)

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True

    print(f"文件: {path}")
    print(f"ImageBase: 0x{image_base:X}")
    print("=" * 72)

    for rva, label, proto in TARGETS:
        off, sec = rva_to_off(sections, rva)
        if off is None:
            print(f"\n### {label}  RVA 0x{rva:04X}  ❌ 不在任何节区")
            continue

        # 往回看看是不是 CC 填充 (函数对齐边界)
        pad_ok = data[off - 4:off] in (b"\xCC\xCC\xCC\xCC", b"\xCC\xCC\xCC\x00") or \
                 data[off - 1] == 0xCC
        print(f"\n### {label:10s} RVA 0x{rva:04X} (节 {sec})   "
              f"{'✅ 前面是 CC 填充=函数入口' if pad_ok else '⚠ 前面不是 CC 填充'}")
        print(f"    原型: {proto}")

        code = data[off:off + 64]
        n_calls_indirect = 0
        has_vtable_10 = False
        for insn in md.disasm(code, image_base + rva):
            mark = ""
            if insn.mnemonic == "call" and insn.op_str.startswith("qword ptr [rax"):
                n_calls_indirect += 1
                if "+ 0x10]" in insn.op_str:
                    has_vtable_10 = True
                    mark = "   <== vtable[0x10]: 就是我们说的 std::function 调用"
            if insn.mnemonic in ("ret", "jmp") and not insn.op_str:
                print(f"    {insn.address:016X}  {insn.mnemonic:<8s} {insn.op_str}{mark}")
                break
            print(f"    {insn.address:016X}  {insn.mnemonic:<8s} {insn.op_str}{mark}")

        verdict = []
        verdict.append("✅ 有 call [rax+0x10]" if has_vtable_10
                       else "❌ 没有 call [rax+0x10]")
        print(f"    --> {verdict[0]}")

    print("\n" + "=" * 72)
    print("判读方法: 每个 invoker 应该是")
    print("    sub  rsp, N")
    print("    mov  eax, ecx        <- 仅 2 参数版 (torque/viscosity/vibration)")
    print("    mov  rcx, [rip+xxx]  <- 取全局 std::function 对象")
    print("    test rcx, rcx / je   <- 空则直接返回")
    print("    mov  rax, [rcx]")
    print("    call qword ptr [rax+0x10]  <- vtable 槽 0x10")
    print("    add  rsp, N / ret")
    return 0


if __name__ == "__main__":
    sys.exit(main())
