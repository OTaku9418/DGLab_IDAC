#!/usr/bin/env python3
"""
离线验证 IndRun.dll 的 5 个 invoker RVA 是否与 indrun_hook_source.cpp 里的
硬编码值匹配。在让游戏去撞之前先把这一步做完。

用法: python re/verify_indrun_rva.py <IndRun.dll 路径>
"""
import struct
import sys

# Windows 控制台默认是 GBK, 遇到 ✅/❌ 会 UnicodeEncodeError。
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def parse_pe(path):
    with open(path, "rb") as f:
        data = f.read()

    if data[:2] != b"MZ":
        raise SystemExit("不是 PE 文件 (缺 MZ)")

    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise SystemExit("不是 PE 文件 (缺 PE\\0\\0)")

    coff = e_lfanew + 4
    machine, n_sections, time_stamp = struct.unpack_from("<HHI", data, coff)
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]

    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    is_pe32_plus = (magic == 0x20B)

    # PE32+ 的 ImageBase 在 offset 0x18, 8 字节
    image_base = struct.unpack_from("<Q", data, opt + 0x18)[0] if is_pe32_plus \
        else struct.unpack_from("<I", data, opt + 0x1C)[0]

    # 节表紧跟在 optional header 之后
    sec_off = opt + opt_size
    sections = []
    for i in range(n_sections):
        s = sec_off + i * 40
        name = data[s:s + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, vaddr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, s + 8)
        sections.append((name, vaddr, vsize, raw_ptr, raw_size))

    return data, machine, time_stamp, image_base, sections


def rva_to_offset(sections, rva):
    for name, vaddr, vsize, raw_ptr, raw_size in sections:
        if vaddr <= rva < vaddr + max(vsize, raw_size):
            return raw_ptr + (rva - vaddr), name
    return None, None


# 与 src/source/indrun_hook_source.cpp 保持一致 (2026-10-17 第二次修正后)
# 注意长度各不相同: 无栈帧的 servo-off 只有 4 字节可比。
TARGETS = [
    (0x21C0, bytes([0x48, 0x83, 0xEC, 0x38, 0x8B, 0xC1, 0x48]), "torque",    "void(int32 dir /*rcx*/, int32 mag /*rdx*/)"),
    (0x2200, bytes([0x48, 0x83, 0xEC, 0x38, 0x8B, 0xC1, 0x48]), "viscosity", "void(int32 /*rcx*/, int32 /*rdx*/)"),
    (0x2240, bytes([0x48, 0x83, 0xEC, 0x38, 0x8B, 0xC1, 0x48]), "vibration", "void(int32 /*rcx*/, int32 /*rdx*/)"),
    (0x2160, bytes([0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x05]), "servo-on",  "void(void)  [有栈帧]"),
    (0x21A0, bytes([0x48, 0x8B, 0x05, 0xE9]),                   "servo-off", "void(void)  [无栈帧]"),
]

EXPECTED_TIMESTAMP = 0x69392E86


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else \
        r"D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Plugins\IndRun\Binaries\Win64\IndRun.dll"

    data, machine, ts, image_base, sections = parse_pe(path)

    print(f"文件     : {path}")
    print(f"大小     : {len(data):,} 字节")
    print(f"machine  : 0x{machine:04X}  ({'x64' if machine == 0x8664 else '非 x64!'})")
    print(f"ImageBase: 0x{image_base:X}")
    print(f"节区     : {', '.join(s[0] for s in sections)}")
    print()

    mark = "✅" if ts == EXPECTED_TIMESTAMP else "❌"
    print(f"{mark} TimeDateStamp = 0x{ts:08X}  (逆向报告记录的是 0x{EXPECTED_TIMESTAMP:08X})")
    if ts != EXPECTED_TIMESTAMP:
        print("   ⚠ 时间戳不同 —— 这块 DLL 很可能不是我们逆向的那个构建,")
        print("     下面即使特征码碰巧对上也不能保证 ABI 正确。")
    print()

    print("RVA 特征码校验:")
    all_ok = ts == EXPECTED_TIMESTAMP
    for rva, sig, label, proto in TARGETS:
        off, sec_name = rva_to_offset(sections, rva)
        if off is None:
            print(f"  ❌ {label:10s} RVA 0x{rva:04X}  不在任何节区里")
            all_ok = False
            continue

        actual = data[off:off + len(sig)]
        ok = actual == sig
        all_ok &= ok
        mark = "✅" if ok else "❌"
        print(f"  {mark} {label:10s} RVA 0x{rva:04X} (节 {sec_name:8s}) "
              f"期望 {sig.hex(' ').upper()}  实际 {actual.hex(' ').upper()}")

        # 顺带把后面几条指令也 dump 出来, 方便人工确认是不是 invoker 入口
        n = min(16, len(data) - off)
        text = " ".join(f"{b:02X}" for b in data[off:off + n])
        print(f"      入口字节: {text}")
        if off + n < len(data) and off >= 16:
            pre = " ".join(f"{b:02X}" for b in data[off - 16:off])
            print(f"      前面 16B: {pre}   <- 看这里是不是对齐到函数开头")
        print(f"      原型: {proto}")
        print()

    print("=" * 68)
    if all_ok:
        print("结论: ✅ 全部匹配 —— IndRunHookSource 可以安全地对这块 DLL 挂钩")
    else:
        print("结论: ❌ 有项目不匹配 —— 挂钩会被 verify_signatures() 拒绝 (不会崩,")
        print("      但拿不到力反馈数据)。需要重新逆向这块 DLL 的 RVA。")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
