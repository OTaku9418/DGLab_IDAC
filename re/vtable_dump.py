#!/usr/bin/env python3
"""vtable_dump.py — 从本机 UE4SS.dll 的 RTTI 里读出每个 mod 相关类的**完整真实虚表**。

为什么需要:
    shim 里的虚表顺序是手工重建的。之前正是"顺序错一位"导致
    UE4SS 调 [vtbl+0x78] 跳到垃圾地址 0x6575 而崩。必须拿二进制真值核对。

RTTI 布局 (MSVC x64):
    [vtable-8] = RTTICompleteObjectLocator*    (VA)
    COL @ rva:  sig(4) offset(4) cdOffset(4) pTypeDescriptor(4) pClassDescriptor(4) pSelf(4)
    TypeDescriptor @ rva:  vftable(8) spare(8) name[]   (name = ".?AV<Class>@@")

用法:  python re/vtable_dump.py
"""
import struct
import sys

import pefile

PATH = sys.argv[1] if len(sys.argv) > 1 else \
    r"D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\UE4SS.dll"

TARGETS = [
    ".?AVCppUserModBase@RC@@",
    ".?AVCppMod@RC@@",
    ".?AVLuaMod@RC@@",
    ".?AVMod@RC@@",
]

pe = pefile.PE(PATH, fast_load=True)
IB = pe.OPTIONAL_HEADER.ImageBase
IMG = pe.OPTIONAL_HEADER.SizeOfImage
SECS = [(s.Name.rstrip(b"\x00").decode(errors="replace"), s.VirtualAddress,
         s.Misc_VirtualSize, s.SizeOfRawData, s.PointerToRawData, s.get_data())
        for s in pe.sections]


def rd(rva, n):
    for _, va, _, _, _, d in SECS:
        if d and va <= rva < va + len(d):
            return d[rva - va: rva - va + n]
    return None


def u32(rva):
    b = rd(rva, 4)
    return struct.unpack("<I", b)[0] if b and len(b) == 4 else None


def cstr(rva, maxn=200):
    b = rd(rva, maxn)
    if not b:
        return ""
    e = b.find(b"\x00")
    return (b[:e] if e >= 0 else b).decode(errors="replace")


pe.parse_data_directories(
    directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
RVA2NAME = {}
for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
    if e.name:
        RVA2NAME.setdefault(e.address, []).append(e.name.decode(errors="replace"))


def sym(rva):
    ns = RVA2NAME.get(rva)
    if not ns:
        return ""
    n = ns[0]
    if n.startswith("?"):
        # ?on_xxx@CppUserModBase@RC@@UEAAXXZ -> on_xxx  (+ 参数签名摘要)
        body = n[1:]
        nm = body.split("@")[0]
        # 取签名部分(UEAA 之后)
        i = body.find("UEAA")
        if i < 0:
            i = body.find("IEAA")
        sig = body[i + 4:].rstrip("Z") if i >= 0 else ""
        return f"{nm}  [{sig}]" if sig else nm
    return n


def find_td(name_bytes):
    """返回 (TypeDescriptor_rva, name_rva)"""
    out = []
    for sn, va, _, _, _, d in SECS:
        if not d:
            continue
        i = 0
        while True:
            i = d.find(name_bytes, i)
            if i < 0:
                break
            out.append((va + i - 0x10, va + i))
            i += 1
    return out


def find_cols_for_td(td_rva):
    cols = []
    for sn, va, _, _, _, d in SECS:
        if not d:
            continue
        for off in range(0, len(d) - 0x18, 4):
            if struct.unpack_from("<I", d, off)[0] != td_rva:
                continue
            col_rva = va + off - 0xC          # pType 在 COL+0xC
            cb = rd(col_rva, 0x18)
            if not cb or len(cb) < 0x18:
                continue
            sig, o, cd, ptype, pclass, pself = struct.unpack("<6I", cb)
            if sig == 1 and ptype == td_rva and pself == col_rva:
                cols.append((col_rva, pclass))
    return cols


def find_vtables_for_col(col_rva):
    """虚表 = 一段 8 字节数组, 其 [-8] 处等于 COL 的 VA"""
    col_va = IB + col_rva
    out = []
    for sn, va, _, _, _, d in SECS:
        if not d or sn not in (".rdata", ".data"):
            continue
        for off in range(0, len(d) - 8, 8):
            if struct.unpack_from("<Q", d, off)[0] != col_va:
                continue
            out.append(va + off + 8)
    return out


def read_vtable(vt_rva, max_slots=40):
    slots = []
    for k in range(max_slots):
        b = rd(vt_rva + 8 * k, 8)
        if not b or len(b) < 8:
            break
        v = struct.unpack("<Q", b)[0]
        r = v - IB
        if not (0 <= r < IMG):
            break
        slots.append(r)
    return slots


for tgt in TARGETS:
    nb = tgt.encode()
    print(f"\n{'='*78}\n目标类: {tgt}")
    tds = find_td(nb)
    if not tds:
        print("  (RTTI 里没有这个类)")
        continue
    for td_rva, name_rva in tds:
        cols = find_cols_for_td(td_rva)
        print(f"  TypeDescriptor @ {td_rva:#x}  名字 @ {name_rva:#x}   COL: "
              + (", ".join(f"{c:#x}" for c, _ in cols) if cols else "(无)"))
        for col_rva, pclass in cols:
            vts = find_vtables_for_col(col_rva)
            for vt in vts:
                slots = read_vtable(vt)
                print(f"\n    ★ 虚表 @ {vt:#x}  ({len(slots)} 槽)   COL @ {col_rva:#x}")
                for i, r in enumerate(slots):
                    print(f"      [{i:>2}] +{i*8:#04x}  {r:>#10x}  {sym(r)}")
