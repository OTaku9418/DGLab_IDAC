#!/usr/bin/env python3
"""vtable_real.py — 读本机 UE4SS.dll 里 CppUserModBase 的真实虚表。

从 probe_rtti 得知 TypeDescriptor(.?AVCppUserModBase@RC@@) 的 vftable 在
0xf2c5e0, 它被 0xd71e70 与 0xd71f54 两处引用 —— 这两处就是候选 COL。
崩溃时 RDX/R8 = UE4SS+0xd71e30, 正在这段附近。

本脚本:
  1) 在 .rdata 里找 "第一项 == COL 地址" 的 8 字节数组 -> 那就是虚表
  2) 逐槽打印 VA / RVA, 并尝试把 RVA 与导出符号对上
"""
import struct
import sys

import pefile

path = sys.argv[1] if len(sys.argv) > 1 else \
    r"D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\UE4SS.dll"
pe = pefile.PE(path, fast_load=True)
ib = pe.OPTIONAL_HEADER.ImageBase
img_size = pe.OPTIONAL_HEADER.SizeOfImage
secs = [(s.Name.rstrip(b"\x00").decode(errors="replace"), s.VirtualAddress,
         s.Misc_VirtualSize, s.SizeOfRawData, s.PointerToRawData, s.get_data())
        for s in pe.sections]


def rd(rva, n):
    for _, va, _, _, _, d in secs:
        if d and va <= rva < va + len(d):
            return d[rva - va: rva - va + n]
    return None


def u64(rva):
    b = rd(rva, 8)
    return struct.unpack("<Q", b)[0] if b and len(b) == 8 else None


# 导出表: RVA -> 名字
pe.parse_data_directories(
    directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
rva2name = {}
for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
    if e.name:
        rva2name.setdefault(e.address, []).append(e.name.decode(errors="replace"))
print(f"导出 {len(pe.DIRECTORY_ENTRY_EXPORT.symbols)} 个, 唯一 RVA {len(rva2name)} 个")


def short(rva):
    names = rva2name.get(rva)
    if not names:
        return ""
    n = names[0]
    # ?on_xxx@CppUserModBase@RC@@... -> on_xxx
    if n.startswith("?"):
        n = n[1:].split("@")[0]
    return n


# 先看 0xd71e30 那段
print("\n=== RVA 0xd71e20 - 0xd71f40 (.rdata) ===")
b = rd(0xD71E20, 0x120)
for off in range(0, 0x120, 8):
    v = struct.unpack_from("<Q", b, off)[0]
    rva = v - ib
    tag = "  <in image>" if 0 <= rva < img_size else ""
    nm = short(rva) if tag else ""
    print(f"  {0xD71E20+off:#010x}  {v:#018x}  rva={rva:#010x}{tag} {nm}")

# 找虚表: 连续 8 字节数组, 第一项是指向 .rdata 的指针(= COL), 后面若干项是 .text 指针
print("\n=== 扫描候选虚表 (首项为 COL 指针, 后续为 .text 函数) ===")
text_va = next(va for n, va, _, _, _, _ in secs if n == ".text")
text_end = text_va + next(vs for n, va, vs, _, _, _ in secs if n == ".text")

found = []
for sn, va, vsz, _, _, d in secs:
    if not d or sn not in (".rdata", ".data"):
        continue
    for off in range(0, len(d) - 8 * 6, 8):
        first = struct.unpack_from("<Q", d, off)[0]
        col_rva = first - ib
        if not (0 <= col_rva < img_size):
            continue
        # COL 指向的数据应该也在 .rdata / .data, 且下一项是 .text 函数
        second = struct.unpack_from("<Q", d, off + 8)[0]
        s_rva = second - ib
        if not (text_va <= s_rva < text_end):
            continue
        # 至少有 5 项连续落在 .text
        cnt = 0
        slots = []
        for k in range(1, 30):
            if (off + 8 * k + 8) > len(d):
                break
            v = struct.unpack_from("<Q", d, off + 8 * k)[0]
            r = v - ib
            if text_va <= r < text_end:
                cnt += 1
                slots.append(r)
            else:
                break
        if cnt >= 5:
            found.append((va + off, col_rva, slots))

print(f"候选 {len(found)} 个:")
for vt_rva, col_rva, slots in found[:20]:
    print(f"  虚表 @ {vt_rva:#x}  COL @ {col_rva:#x}  {len(slots)} 项")

# 挑出名字匹配的那个: COL 里的 TypeDescriptor 应该指向 .?AVCppUserModBase@RC@@
NAME_RVA = 0xF2C618
TD_RVA = NAME_RVA - 0x10
print(f"\n目标 TypeDescriptor @ {TD_RVA:#x} (名字 @ {NAME_RVA:#x})")
for vt_rva, col_rva, slots in found:
    # COL 结构: sig(4) off(4) cd(4) pType(4) pClass(4) pSelf(4)
    cb = rd(col_rva, 0x18)
    if not cb:
        continue
    sig, o, cd, ptype, pclass, pself = struct.unpack("<6I", cb)
    if ptype == TD_RVA:
        print(f"\n★★★ 命中! 虚表 @ {vt_rva:#x}  COL @ {col_rva:#x}")
        print(f"    COL: sig={sig} offset={o} cdOffset={cd} pType={ptype:#x} "
              f"pClass={pclass:#x} pSelf={pself:#x}")
        print(f"\n{'槽':>3} {'RVA':>12}  符号")
        for i, r in enumerate(slots):
            print(f"{i:>3} {r:>#12x}  {short(r)}")
