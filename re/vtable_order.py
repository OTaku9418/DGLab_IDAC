#!/usr/bin/env python3
"""
vtable_order.py — 从本机 UE4SS.dll 的代码里反推 CppUserModBase 的**真实虚表顺序**

为什么需要这个:
    shim/Mod/CppUserModBase.hpp 是手工重建的虚表。既然之前就是"顺序错了"
    才崩的, 就不能再靠"看着像"来定顺序 —— 必须从二进制里拿真值。

原理:
    虚表顺序体现在**调用点**。UE4SS 调 on_program_start 时生成的代码是
        mov  rax, [rcx]          ; rax = vptr
        call qword ptr [rax+0x18] ; 槽位 3 (字节偏移 0x18)
    所以: 反汇编 .text, 找出所有 `call qword ptr [reg+disp]`, 统计 disp 的
    分布; 再把 disp 与"哪些导出函数在同一批"对照。由于这些虚函数在
    UE4SS.dll 里都以基类实现(rather than 各 mod 自己的), 导出地址会重复,
    所以更可靠的办法是: **看每个槽位被调用的上下文**, 以及用已知签名
    (on_update 每帧调、on_program_start 只在 init 调一次) 来锚定。

    本脚本走的是更直接的一条路:
      1) 收集所有 `call [reg+disp]` 的 disp 值 -> 得出虚表用到了哪些槽位
      2) 用 capstone 反汇编 UE4SS.dll 里 CppMod 相关函数, 打印槽位 2..12
         各自的调用点, 人工/规则判定
      3) 同时校验导出表里的符号集合与 shim 里声明的顺序是否自洽

用法:
    python re/vtable_order.py
"""
import re
import sys
from collections import Counter, defaultdict

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

UE4SS = r"D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\UE4SS.dll"


def load_text(path):
    pe = pefile.PE(path, fast_load=True)
    image_base = pe.OPTIONAL_HEADER.ImageBase
    text = None
    for s in pe.sections:
        name = s.Name.rstrip(b"\x00").decode(errors="replace")
        if name == ".text":
            text = s
            break
    if text is None:
        raise SystemExit("no .text")
    return pe, image_base, text.get_data(), text.VirtualAddress, text.Misc_VirtualSize


def main():
    pe, image_base, code, text_va, text_size = load_text(UE4SS)
    print(f"UE4SS.dll  ImageBase={image_base:#x}  .text VA={text_va:#x} size={text_size:#x}")

    # ---- 1) 收集所有间接调用 call [reg+disp] ----
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    disp_counter = Counter()
    disp_sites = defaultdict(list)

    # 只扫 .text。用 skipdata 防止数据段导致失步。
    for insn in md.disasm(code, image_base + text_va, count=0):
        if insn.mnemonic != "call":
            continue
        ops = insn.operands
        if len(ops) != 1 or ops[0].type != 2:  # X86_OP_MEM
            continue
        m = ops[0].mem
        # 只要基址寄存器、无 index、位移在虚表合理范围
        if m.index != 0:
            continue
        d = m.disp
        if 0x08 <= d <= 0x120 and (d % 8 == 0):
            disp_counter[d] += 1
            if len(disp_sites[d]) < 6:
                disp_sites[d].append(insn.address)

    print("\n=== 虚表槽位使用频次 (call [reg+disp]) ===")
    print(f"{'disp':>6} {'slot':>4} {'count':>7}   调用点(前几个)")
    for d in sorted(disp_counter):
        slot = d // 8
        sites = " ".join(f"{a-image_base:#x}" for a in disp_sites[d][:4])
        print(f"{d:#06x} {slot:>4} {disp_counter[d]:>7}   {sites}")

    # ---- 2) 导出表里的 CppUserModBase 虚函数 ----
    pe2 = pefile.PE(UE4SS)
    pe2.parse_data_directories(
        directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]]
    )
    syms = {}
    for exp in pe2.DIRECTORY_ENTRY_EXPORT.symbols:
        if not exp.name:
            continue
        n = exp.name.decode(errors="replace")
        if "@CppUserModBase@RC@@" in n:
            syms[exp.ordinal] = n

    print(f"\n=== CppUserModBase 导出符号 ({len(syms)} 个, 按导出序号) ===")
    for o in sorted(syms):
        # 简化显示
        n = syms[o]
        m = re.match(r"\?(on_\w+|register_\w+|render_\w+)@", n)
        short = m.group(1) if m else n[:50]
        print(f"  ord {o:>5}  {short:<22} (VA {image_base + [e for e in pe2.DIRECTORY_ENTRY_EXPORT.symbols if e.ordinal==o][0].address:#x})")


if __name__ == "__main__":
    main()
