#!/usr/bin/env python3
"""
mod_vtable.py — 从 mod 自己的 DLL 里找出 `start_mod` 里 new 出来的对象的**虚表指针**,
               并把它与 UE4SS.dll 里的基类虚表对照。

思路(为什么这样最可靠):
    LocalRadio 是**在这台机器的 UE4SS 上能正常跑**的 C++ mod。它的构造函数
    会把自己的虚表地址写进对象首 8 字节(vptr)。所以:
        LocalRadio 的 vptr -> 它编时的基类布局
        我们的   vptr -> 我们编时的基类布局
    把两者的"槽位 -> 函数 VA"并排列出来, 谁错位一眼可见。
    这比在 UE4SS.dll 里猜 RTTI 强, 因为 mod DLL 里根本没有那条 RTTI 链。

做法:
    1) 解析导出表, 得到 start_mod 的 RVA
    2) 从 start_mod 开始反汇编, 找 `lea reg, [rip+disp]` (载入 vtable 地址) 和
       `call` (new / 构造函数)
    3) 把 lea 目标当作虚表, 逐槽读 8 字节 -> 函数 RVA; 用 .pdata 判断是否真实函数

用法: python re/mod_vtable.py
"""
import struct
import sys

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

TARGETS = [
    ("LocalRadio (可用)",
     r"D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\Mods\LocalRadio\dlls\main.dll"),
    ("DGLab_IDAC (崩溃)",
     r"D:\INDAC\DGLab_IDAC\build\Release\DGLab_IDAC.dll"),
    ("AbiTest (最小探针)",
     r"D:\INDAC\DGLab_IDAC\build\Release\abitest.dll"),
]


class Img:
    def __init__(self, path):
        self.path = path
        self.pe = pefile.PE(path, fast_load=True)
        self.name = path.rsplit("\\", 1)[-1]
        self.ib = self.pe.OPTIONAL_HEADER.ImageBase
        self.img = self.pe.OPTIONAL_HEADER.SizeOfImage
        self.secs = [(s.Name.rstrip(b"\x00").decode(errors="replace"),
                      s.VirtualAddress, s.Misc_VirtualSize,
                      s.SizeOfRawData, s.PointerToRawData, s.get_data())
                     for s in self.pe.sections]
        self.exports = {}
        try:
            self.pe.parse_data_directories(directories=[
                pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
            for e in self.pe.DIRECTORY_ENTRY_EXPORT.symbols:
                if e.name:
                    self.exports[e.name.decode(errors="replace")] = e.address
        except Exception:
            pass
        # .pdata 函数表
        self.funcs = []
        try:
            d = self.pe.OPTIONAL_HEADER.DATA_DIRECTORY[
                pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]]
            b = self.rd(d.VirtualAddress, d.Size)
            if b:
                for i in range(0, len(b) - 11, 12):
                    s, e, u = struct.unpack_from("<III", b, i)
                    if s == 0:
                        break
                    self.funcs.append((s, e))
        except Exception:
            pass

    def rd(self, rva, n):
        for _, va, _, _, _, d in self.secs:
            if d and va <= rva < va + len(d):
                return d[rva - va: rva - va + n]
        return None

    def u64(self, rva):
        b = self.rd(rva, 8)
        return struct.unpack("<Q", b)[0] if b and len(b) == 8 else None

    def is_func(self, rva):
        for s, e in self.funcs:
            if s <= rva < e:
                return True
        return False

    def dis(self, rva, n):
        o = None
        for _, va, _, _, praw, d in self.secs:
            if d and va <= rva < va + len(d):
                o = praw + (rva - va)
                break
        if o is None:
            return []
        md = Cs(CS_ARCH_X86, CS_MODE_64)
        md.detail = True
        blob = open(self.path, "rb").read()
        return list(md.disasm(blob[o:o + n], rva))


def find_vtable_candidates(img):
    """在 start_mod 里找 lea reg,[rip+x] 的目标, 那个目标就是 vtable。"""
    sm = None
    for k, v in img.exports.items():
        if k in ("start_mod", "?start_mod@@YAPEAVCppUserModBase@RC@@XZ"):
            sm = v
            break
    if sm is None:
        print("  找不到 start_mod 导出")
        return []
    print(f"  start_mod @ RVA {sm:#x}")
    out = []
    for insn in img.dis(0x1000 if sm < 0x1000 else sm, 0x400):
        if insn.address < sm:
            continue
        if insn.mnemonic == "lea" and "rip" in insn.op_str:
            ops = insn.operands
            if len(ops) == 2 and ops[1].type == 3:      # X86_OP_MEM
                tgt = insn.address + insn.size + ops[1].mem.disp
                r = tgt - img.ib
                if 0 <= r < img.img:
                    out.append((insn.address, r))
    return out


for label, path in TARGETS:
    print(f"\n{'='*78}\n### {label}\n    {path}")
    try:
        img = Img(path)
    except Exception as ex:
        print(f"  读取失败: {ex}")
        continue
    print(f"  ImageBase={img.ib:#x}  size={img.img:#x}  "
          f".pdata 函数 {len(img.funcs)} 个  导出 {len(img.exports)} 个")
    cands = find_vtable_candidates(img)
    print(f"  start_mod 里的 lea rip 目标 {len(cands)} 个:")
    for at, r in cands[:10]:
        print(f"    @{at:#x} -> RVA {r:#x}")
    # 对每个候选, 试着当虚表读
    for at, vt in cands[:10]:
        slots = []
        for k in range(24):
            v = img.u64(vt + 8 * k)
            if v is None:
                break
            r = v - img.ib
            if not (0 <= r < img.img):
                break
            slots.append(r)
        real = sum(1 for r in slots if img.is_func(r))
        if len(slots) >= 4 and real >= 3:
            print(f"\n  ★ 虚表候选 @ RVA {vt:#x}  ({len(slots)} 槽, 其中 {real} 个是 .pdata 里的真实函数)")
            for k, r in enumerate(slots):
                tag = "fn" if img.is_func(r) else "??"
                print(f"      [{k:>2}] +{k*8:#04x}  {r:>#10x}  {tag}")
