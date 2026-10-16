#!/usr/bin/env python3
"""
verify_shim.py — 编译后核验: 我们 DLL 里 DGLabIdacMod 的虚表到底长什么样。

它必须满足(与本机 UE4SS.dll 的 CppUserModBase 一致):
    [0] 标量删除析构      [1] 析构             [2] on_update
    [3] on_unreal_init    [4] on_ui_init       <-- shim 补的那个
    [5] on_program_start  [6..9] on_lua_start x4
    [10..13] on_lua_stop x4                  [14] on_dll_load
    [15] render_tab                          [16] on_cpp_mods_loaded

做法:
   1) 找 start_mod -> 它调用的构造函数
   2) 在构造函数里找 `lea reg,[rip+disp]` = 虚表地址
   3) 逐槽读函数指针; 再用这些指针反查"哪个槽指向我们覆盖的函数"
      (我们的覆盖函数可以在 .pdata 里找到, 且 start_mod 附近有 X)
   4) 打印槽位 -> RVA, 并标注哪些是我们 override 的
"""
import struct
import sys

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

DLL = sys.argv[1] if len(sys.argv) > 1 else \
    r"D:\INDAC\DGLab_IDAC\build\Release\DGLab_IDAC.dll"

pe = pefile.PE(DLL, fast_load=True)
IB = pe.OPTIONAL_HEADER.ImageBase
IMG = pe.OPTIONAL_HEADER.SizeOfImage
SECS = [(s.Name.rstrip(b"\x00").decode(errors="replace"), s.VirtualAddress,
         s.Misc_VirtualSize, s.SizeOfRawData, s.PointerToRawData, s.get_data())
        for s in pe.sections]
BLOB = open(DLL, "rb").read()


def rd(rva, n):
    for _, va, _, _, _, d in SECS:
        if d and va <= rva < va + len(d):
            return d[rva - va: rva - va + n]
    return None


def u64(rva):
    b = rd(rva, 8)
    return struct.unpack("<Q", b)[0] if b and len(b) == 8 else None


def u32(rva):
    b = rd(rva, 4)
    return struct.unpack("<I", b)[0] if b and len(b) == 4 else None


def dis(rva, n):
    o = None
    for _, va, _, _, praw, d in SECS:
        if d and va <= rva < va + len(d):
            o = praw + (rva - va)
            break
    if o is None:
        return []
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    return list(md.disasm(BLOB[o:o + n], rva))


# 导出
pe.parse_data_directories(
    directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
EXP = {e.name.decode(errors="replace"): e.address
       for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name}
print(f"{DLL}\n  ImageBase={IB:#x} size={IMG:#x} 导出={list(EXP)}")

# .pdata
d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[
    pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]]
b = rd(d.VirtualAddress, d.Size)
FUNCS = []
if b:
    for i in range(0, len(b) - 11, 12):
        s, e, u = struct.unpack_from("<III", b, i)
        if s == 0:
            break
        FUNCS.append((s, e))


def is_func(r):
    return any(s <= r < e for s, e in FUNCS)


def fname(r):
    for s, e in FUNCS:
        if s <= r < e:
            return f"{s:#x}..{e:#x}"
    return "(非函数)"


sm = EXP.get("start_mod")
print(f"  start_mod @ {sm:#x}")
ctor = None
for insn in dis(sm, 0x60):
    print(f"    {insn.address:#08x}  {insn.mnemonic:<6} {insn.op_str}")
    if insn.mnemonic == "call":
        ops = insn.operands
        if ops and ops[0].type == 2:  # IMM
            t = ops[0].imm - IB
            if 0 <= t < IMG and is_func(t):
                ctor = t

if ctor is None:
    # start_mod 可能是 jmp 到别处
    print("  没直接找到构造函数调用; 改扫整个 .text 找虚表引用")
else:
    print(f"\n  构造函数 @ {ctor:#x}, 反汇编找虚表 lea:")
    for insn in dis(ctor, 0x20):
        mark = ""
        if insn.mnemonic == "lea" and "rip" in insn.op_str:
            ops = insn.operands
            if len(ops) == 2 and ops[1].type == 3:
                tgt = insn.address + insn.size + ops[1].mem.disp
                mark = f"   <== 目标 RVA {tgt - IB:#x}"
                print(f"    {insn.address:#08x}  {insn.mnemonic:<6} {insn.op_str}{mark}")
                vt = tgt - IB
                print(f"\n  ★ 虚表 @ RVA {vt:#x}:")
                for k in range(22):
                    v = u64(vt + 8 * k)
                    if v is None:
                        break
                    r = v - IB
                    if not (0 <= r < IMG):
                        print(f"    [{k:>2}] +{k*8:#04x}  {v:#x}  (越界, 停)")
                        break
                    print(f"    [{k:>2}] +{k*8:#04x}  {r:>#10x}  {fname(r)}")
        else:
            print(f"    {insn.address:#08x}  {insn.mnemonic:<6} {insn.op_str}")
