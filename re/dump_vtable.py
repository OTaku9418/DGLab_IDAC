#!/usr/bin/env python3
"""
dump_vtable.py — 直接从本机 UE4SS.dll 的 RTTI 里把 CppUserModBase 的**真实虚表**
                 按槽位顺序读出来。

为什么不再"反汇编猜顺序":
    之前 shim 就是靠"看着像"定的顺序, 结果错位一格直接崩。虚表顺序是 ABI,
    必须从二进制里取真值。

原理 (MSVC x64 RTTI 布局):
    虚表之前的那个 8 字节是 RTTICompleteObjectLocator* , 结构:
        struct RTLDCompleteObjectLocator {   // 32 位字段 MSVC x64
            uint32 signature;      // 0
            uint32 offset;         // 4
            uint32 cdOffset;       // 8
            uint32 pTypeDescriptor;   // 0xC  指向 .rdata 的 TypeDescriptor
            uint32 pClassDescriptor;  // 0x10 (RVA)
            uint32 pSelf;             // 0x14 (RVA)
        }
    所以:
      1) 在 .rdata/.data 里找字符串 ".?AVCppUserModBase@RC@@" (TypeDescriptor 名字)
      2) TypeDescriptor 往前 0x10 字节就是它的 vftable (type_info 的 vptr)
      3) 扫描整个映像找指向该 TypeDescriptor 的 COL
      4) 扫描 .rdata 找第一项 == COL 地址 的位置 —— 那后面就是完整虚表
      5) 逐项读虚表, 输出每个槽位的函数 VA 及其符号名

用法:  python re/dump_vtable.py [dll路径]
"""
import struct
import sys

import pefile

DEFAULT = r"D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\UE4SS.dll"


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT
    pe = pefile.PE(path, fast_load=True)
    ib = pe.OPTIONAL_HEADER.ImageBase
    secs = [(s.Name.rstrip(b"\x00").decode(errors="replace"),
             s.VirtualAddress, s.Misc_VirtualSize, s.SizeOfRawData,
             s.PointerToRawData, s.get_data()) for s in pe.sections]

    def rva_to_off(rva):
        for _, va, vsz, rsz, praw, _ in secs:
            if va <= rva < va + max(vsz, rsz):
                return praw + (rva - va)
        return None

    def read_at(rva, n):
        o = rva_to_off(rva)
        if o is None:
            return None
        for _, va, _, _, _, data in secs:
            if data and va <= rva < va + len(data):
                return data[rva - va: rva - va + n]
        return None

    def u32(rva):
        b = read_at(rva, 4)
        return struct.unpack("<I", b)[0] if b and len(b) == 4 else None

    def u64(rva):
        b = read_at(rva, 8)
        return struct.unpack("<Q", b)[0] if b and len(b) == 8 else None

    # ---- 1) 找 TypeDescriptor 名字 ----
    target = b".?AVCppUserModBase@RC@@"
    print(f"目标 RTTI 名字: {target.decode()}")
    found = []
    for name, va, vsz, rsz, praw, data in secs:
        if not data:
            continue
        start = 0
        while True:
            i = data.find(target, start)
            if i < 0:
                break
            found.append((name, va + i))
            start = i + 1
    print(f"命中 {len(found)} 处: " + ", ".join(f"{n}+{rva:#x}" for n, rva in found))
    if not found:
        return

    # TypeDescriptor 结构: vftable(8) + spare(8) + name[]  所以名字在 +0x10
    for sec_name, name_rva in found:
        td_rva = name_rva - 0x10
        print(f"\n=== 试 TypeDescriptor @ {sec_name}+{td_rva:#x} ===")
        # 3) 找引用它的 COL
        col_candidates = []
        for name, va, vsz, rsz, praw, data in secs:
            if not data:
                continue
            for off in range(0, len(data) - 0x18, 4):
                if struct.unpack_from("<I", data, off)[0] == td_rva:
                    col_rva = va + off
                    ptype = struct.unpack_from("<I", data, off)[0]
                    sig = struct.unpack_from("<I", data, off - 0xC)[0] if off >= 0xC else None
                    pself = struct.unpack_from("<I", data, off + 8)[0]
                    if sig == 1 and pself == col_rva:
                        col_candidates.append(col_rva)
        print(f"找到 {len(col_candidates)} 个自洽的 COL: " +
              ", ".join(f"{c:#x}" for c in col_candidates[:8]))
        if not col_candidates:
            continue

        # 4) 找虚表: 第一项 == COL VA
        for col_rva in col_candidates[:4]:
            col_va = ib + col_rva
            for name, va, vsz, rsz, praw, data in secs:
                if not data:
                    continue
                for off in range(0, len(data) - 8, 8):
                    if struct.unpack_from("<Q", data, off)[0] == col_va:
                        vt_rva = va + off + 8
                        print(f"\n★ 虚表 @ {name}+{vt_rva:#x}  (COL @ {col_rva:#x})")
                        print(f"{'槽':>3} {'VA':>18}   {'RVA':>10}  符号")
                        for slot in range(0, 40):
                            fn = u64(vt_rva + slot * 8)
                            if fn is None or fn == 0:
                                print(f"{slot:>3} {'(空/结束)':>18}")
                                break
                            # 只接受落在这个映像里的
                            if not (ib <= fn < ib + pe.OPTIONAL_HEADER.SizeOfImage):
                                print(f"{slot:>3} {fn:>#18x}  <- 不在本映像, 视为 vtable 结束")
                                break
                            print(f"{slot:>3} {fn:>#18x}   {fn - ib:>#10x}")
                        print()
                        return


if __name__ == "__main__":
    main()
