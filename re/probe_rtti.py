#!/usr/bin/env python3
"""probe_rtti.py — 找出 CppUserModBase 的 vtable 到底在哪 (逐个布局假设验证)"""
import struct
import sys

import pefile

path = sys.argv[1] if len(sys.argv) > 1 else \
    r"D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\UE4SS.dll"
pe = pefile.PE(path, fast_load=True)
ib = pe.OPTIONAL_HEADER.ImageBase
secs = [(s.Name.rstrip(b"\x00").decode(errors="replace"), s.VirtualAddress,
         s.Misc_VirtualSize, s.SizeOfRawData, s.PointerToRawData, s.get_data())
        for s in pe.sections]


def rd(rva, n):
    for _, va, _, _, _, d in secs:
        if d and va <= rva < va + len(d):
            return d[rva - va: rva - va + n]
    return None


def u32(rva):
    b = rd(rva, 4)
    return struct.unpack("<I", b)[0] if b and len(b) == 4 else None


name = b".?AVCppUserModBase@RC@@"
hits = []
for sn, va, _, _, _, d in secs:
    if not d:
        continue
    i = 0
    while True:
        i = d.find(name, i)
        if i < 0:
            break
        hits.append((sn, va + i))
        i += 1
print("TypeDescriptor 名字命中:", [(s, hex(r)) for s, r in hits])

for sn, name_rva in hits:
    print(f"\n{'='*70}\n名字 RVA {name_rva:#x} ({sn})")
    # 往前 dump 0x60 字节, 找 vftable / spare
    base = name_rva - 0x60
    b = rd(base, 0x70)
    print(f"--- {base:#x} 起 0x70 字节 ---")
    for off in range(0, 0x70, 0x10):
        row = b[off:off + 0x10]
        hexs = " ".join(f"{x:02x}" for x in row)
        vals = " ".join(f"{struct.unpack_from('<Q', b, off+i)[0]:#x}" for i in (0, 8))
        asc = "".join(chr(x) if 32 <= x < 127 else "." for x in row)
        print(f"  +{off:02x}  {hexs}  | {asc}")

    # 假设 TypeDescriptor 在 name_rva-0x10, vftable 在 name_rva-0x20
    td = name_rva - 0x10
    print(f"\n假设 TypeDescriptor 结构起点 = {td:#x}")
    for guess in (0x8, 0x10, 0x18):
        vt = name_rva - guess - 0x10
        v = u32(vt)
        print(f"  若 vftable @ name-{guess:#x}-0x10 = {vt:#x}  ->  {v if v is None else hex(v)}")

    # 扫描谁引用了 name_rva-0x10 附近的所有 4 字节值
    for cand in range(name_rva - 0x20, name_rva, 4):
        refs = []
        for sn2, va2, _, _, _, d2 in secs:
            if not d2:
                continue
            for off in range(0, len(d2) - 4, 4):
                if struct.unpack_from("<I", d2, off)[0] == cand:
                    refs.append(va2 + off)
        if refs:
            print(f"  RVA {cand:#x} 被 {len(refs)} 处 4 字节引用: "
                  + ", ".join(f"{r:#x}" for r in refs[:6]))
