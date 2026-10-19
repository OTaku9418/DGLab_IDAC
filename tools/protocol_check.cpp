// ---------------------------------------------------------------------------
// protocol_check —— B0/BF 编码自检
//
// 为什么要有这个文件:
//   2026-10-17 之前, StrengthMode 的四个枚举值与官方文档**正好相反**
//   (Absolute 写成 0b00, Unchanged 写成 0b11)。游戏能正常跑、日志全绿、
//   编码自检也通过 —— 唯一的表现是"设备不动"。因为这个错误只存在于
//   "我们以为的线上语义"和"设备真正的语义"之间, 只有对着协议的官方例子
//   逐位核对才能发现。
//
//   所以这个工具的基准不是我们的实现, 而是官方文档里的例子。
//   权威来源: dungeonlab-open/dglab-bluetooth-protocol, coyote/v3/README.md
//   按快照提交 045720d 引用; 链接与 sha256 在 re/reference/dglab-bluetooth-protocol/PROVENANCE.md。
//
// 官方例子里**没有给强度模式的完整十六进制**(那 4 个 mode 例子只有文字
// 描述), 所以下面用官方表格明确写出的位模式自己构造期望值。这是刻意的:
// 把"文档写的是什么"和"代码算出来是什么"并排放, 而不是拿代码自己的常量
// 去断言代码自己。
//
// 用法: protocol_check.exe        全部通过返回 0, 有失败返回 1
// ---------------------------------------------------------------------------

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "transport/transport.hpp"

using namespace dglab_idac;

// ---------------------------------------------------------------------------
namespace {

int g_pass = 0;
int g_fail = 0;
const char* g_group = "";

void group(const char* name) {
    g_group = name;
    std::printf("\n== %s ==\n", name);
}

void check(bool ok, const char* what) {
    if (ok) {
        ++g_pass;
        std::printf("  [ok]   %s\n", what);
    } else {
        ++g_fail;
        std::printf("  [FAIL] %s   <-- %s\n", what, g_group);
    }
}

void check_eq(long long got, long long want, const char* what) {
    if (got == want) {
        ++g_pass;
        std::printf("  [ok]   %s = %lld\n", what, got);
    } else {
        ++g_fail;
        std::printf("  [FAIL] %s = %lld, 期望 %lld   <-- %s\n", what, got, want, g_group);
    }
}

// 造一个干净的帧: A 通道全力输出方波, B 通道静默
Frame make_frame(int strength_a, int strength_b) {
    Frame f{};
    f.a.strength = static_cast<uint8_t>(strength_a);
    f.b.strength = static_cast<uint8_t>(strength_b);
    for (int i = 0; i < kSlotsPerFrame; ++i) {
        f.a.slots[i].pulse_frequency = 30;
        f.a.slots[i].pulse_intensity = 50;
        f.b.slots[i].pulse_frequency = 30;
        f.b.slots[i].pulse_intensity = 50;
    }
    return f;
}

// byte[1] 的高低两段
int mode_a_of(const B0& c) { return (c.bytes[1] >> 2) & 0x03; }
int mode_b_of(const B0& c) { return c.bytes[1] & 0x03; }
int seq_of(const B0& c) { return (c.bytes[1] >> 4) & 0x0F; }

}  // namespace

// ---------------------------------------------------------------------------
int main() {
    std::printf("protocol_check —— 对照 官方协议(见 PROVENANCE.md) 校验 B0/BF 编码\n");

    // -----------------------------------------------------------------------
    group("强度模式: 枚举值必须等于文档写的位模式");

    // 文档 "Intensity Interpretation Mode" 表:
    //   0b00 不变(忽略强度字节) / 0b01 相对加 / 0b10 相对减 / 0b11 绝对设置
    check_eq(static_cast<int>(StrengthMode::Unchanged), 0b00, "Unchanged == 0b00(不变)");
    check_eq(static_cast<int>(StrengthMode::RelativeAdd), 0b01, "RelativeAdd == 0b01");
    check_eq(static_cast<int>(StrengthMode::RelativeSub), 0b10, "RelativeSub == 0b10");
    check_eq(static_cast<int>(StrengthMode::Absolute), 0b11, "Absolute == 0b11(绝对设置)");

    // -----------------------------------------------------------------------
    group("byte[1] 的位布局: A 在 bit3-2, B 在 bit1-0, seq 在高 4 位");

    {
        // 文档举例 0b1101 => A 模式 0b11(绝对设置), B 模式 0b01(相对加)
        const Frame f = make_frame(0, 0);
        const B0 c = encode_b0(f, StrengthMode::Absolute, StrengthMode::RelativeAdd, 0, nullptr, nullptr);
        check_eq(mode_a_of(c), 0b11, "文档例 0b1101 的 A 半段");
        check_eq(mode_b_of(c), 0b01, "文档例 0b1101 的 B 半段");
    }
    {
        // 文档举例 0b0110 => A 0b01(相对加), B 0b10(相对减)
        const Frame f = make_frame(0, 0);
        const B0 c = encode_b0(f, StrengthMode::RelativeAdd, StrengthMode::RelativeSub, 0, nullptr, nullptr);
        check_eq(mode_a_of(c), 0b01, "文档例 0b0110 的 A 半段");
        check_eq(mode_b_of(c), 0b10, "文档例 0b0110 的 B 半段");
    }
    {
        // 官方 20 字节例子的 byte[1] = 0x04 再配 seq=5 => 0x54。
        // 这里校验 seq 与"不改强度"两段不互相踩。
        // 注意必须给非零强度 —— 强度为 0 时 encode_b0 会**故意**把模式提升为
        // 绝对设置(见下面"静音路径"一节), 那是正确行为, 用 0 强度测不出这里。
        const Frame f = make_frame(40, 40);
        const B0 c = encode_b0(f, StrengthMode::Unchanged, StrengthMode::Unchanged, 5, nullptr, nullptr);
        check_eq(seq_of(c), 5, "seq 高 4 位 = 5 (官方例 0x54)");
        check_eq(mode_a_of(c), 0b00, "两条通道都不改强度 => A 半段 0b00");
        check_eq(mode_b_of(c), 0b00, "两条通道都不改强度 => B 半段 0b00");
    }
    {
        // 官方例子的 byte[1] 是 0x00(seq=0) —— 验证 set_seq(0) 不污染模式位
        const Frame f = make_frame(40, 40);
        const B0 c = encode_b0(f, StrengthMode::Absolute, StrengthMode::Absolute, 0, nullptr, nullptr);
        check_eq(c.bytes[1], 0x0F, "seq=0 + 两通道绝对设置 => byte[1]==0x0F");
    }

    // -----------------------------------------------------------------------
    group("强度字节: 0..200 且绝对设置时真的带上请求值");

    {
        const Frame f = make_frame(32, 7);
        const B0 c = encode_b0(f, StrengthMode::Absolute, StrengthMode::Absolute, 3, nullptr, nullptr);
        check_eq(c.bytes[0], 0xB0, "byte[0]");
        check_eq(c.bytes[2], 32, "byte[2] = A 强度");
        check_eq(c.bytes[3], 7, "byte[3] = B 强度");
        check_eq(seq_of(c), 3, "seq");
    }
    {
        // 200 是文档给的合法上限
        const Frame f = make_frame(200, 200);
        const B0 c = encode_b0(f, StrengthMode::Absolute, StrengthMode::Absolute, 1, nullptr, nullptr);
        check_eq(c.bytes[2], 200, "A=200 合法且原样送出");
        check_eq(c.bytes[3], 200, "B=200 合法且原样送出");
    }
    {
        // 越界要走钳制, 不能回绕(0xFF 会变成 255 -> 设备视为 0 或非法)
        const Frame f = make_frame(255, 255);
        bool va = false, vb = false;
        const B0 c = encode_b0(f, StrengthMode::Absolute, StrengthMode::Absolute, 1, &va, &vb);
        check_eq(c.bytes[2], 200, "A=255 被钳到 200 而不是回绕");
        check_eq(c.bytes[3], 200, "B=255 被钳到 200 而不是回绕");
        check(va && vb, "钳制后仍然算作有效编码");
    }

    // -----------------------------------------------------------------------
    group("静音路径: 强度必须真的写成 0, 且不能停在'不变'模式");

    {
        Frame f = make_frame(40, 40);
        f.muted = true;
        // 传输层的状态机在"不想改强度"时会传 Unchanged —— 静音时它必须被推翻
        const B0 c = encode_b0(f, StrengthMode::Unchanged, StrengthMode::Unchanged, 1, nullptr, nullptr);
        check_eq(mode_a_of(c), 0b11, "静音时 A 模式被提升为绝对设置(否则设备保持旧强度)");
        check_eq(mode_b_of(c), 0b11, "静音时 B 模式被提升为绝对设置");
        check_eq(c.bytes[2], 0, "静音时 A 强度字节 = 0");
        check_eq(c.bytes[3], 0, "静音时 B 强度字节 = 0");
    }
    {
        // 没静音但强度就是 0: 同样必须真的下发 0
        const Frame f = make_frame(0, 0);
        const B0 c = encode_b0(f, StrengthMode::Unchanged, StrengthMode::Unchanged, 1, nullptr, nullptr);
        check_eq(mode_a_of(c), 0b11, "强度 0 时 A 模式提升为绝对设置");
        check_eq(c.bytes[2], 0, "强度 0 + 不变模式 => A 字节被清成 0");
    }

    // -----------------------------------------------------------------------
    group("脉冲数据: 合法域 频率 10..240 / 强度 0..100");

    {
        const Frame f = make_frame(40, 40);
        const B0 c = encode_b0(f, StrengthMode::Absolute, StrengthMode::Absolute, 1, nullptr, nullptr);
        bool freq_ok = true, int_ok = true;
        for (int i = 0; i < kSlotsPerFrame; ++i) {
            if (c.bytes[4 + i] < 10 || c.bytes[4 + i] > 240) freq_ok = false;
            if (c.bytes[12 + i] < 10 || c.bytes[12 + i] > 240) freq_ok = false;
            if (c.bytes[8 + i] > 100) int_ok = false;
            if (c.bytes[16 + i] > 100) int_ok = false;
        }
        check(freq_ok, "所有频率字节落在 10..240");
        check(int_ok, "所有脉冲强度字节 <= 100");
    }
    {
        // 通道没输出时不能塞非法频率 0 —— 0 会让整个通道(含强度)被设备丢弃
        const Frame f = make_frame(40, 0);
        const B0 c = encode_b0(f, StrengthMode::Absolute, StrengthMode::Absolute, 1, nullptr, nullptr);
        check_eq(c.bytes[12], 10, "B 通道空闲时频率用合法最小值 10, 不是 0");
        check_eq(c.bytes[16], 0, "B 通道空闲时脉冲强度 0");
    }
    {
        // 越界的脉冲值要被钳进合法域, 而不是原样送出导致整通道被丢
        Frame f = make_frame(40, 40);
        f.a.slots[0].pulse_frequency = 5;      // 低于 10
        f.a.slots[1].pulse_intensity = 200;    // 高于 100
        bool va = false, vb = false;
        const B0 c = encode_b0(f, StrengthMode::Absolute, StrengthMode::Absolute, 1, &va, &vb);
        check_eq(c.bytes[4], 10, "越界频率 5 被钳到 10");
        check_eq(c.bytes[9], 100, "越界脉冲强度 200 被钳到 100");
        check(va, "钳制后 A 通道仍标记为有效");
    }

    // -----------------------------------------------------------------------
    group("BF 软限位: 字节顺序(写错会把限位设成别的值, 这是安全项)");

    {
        const B0 bf = make_bf(150, 30);
        check_eq(bf.bytes[0], 0xBF, "byte[0] = 0xBF");
        check_eq(bf.bytes[1], 150, "byte[1] = A 通道软限位");
        check_eq(bf.bytes[2], 30, "byte[2] = B 通道软限位");
    }
    {
        // 文档: 合法 0..200; 越界则**保持设备原值不变**
        const B0 bf = make_bf(999, -5);
        check_eq(bf.bytes[1], 200, "超上限 999 被钳到 200");
        check_eq(bf.bytes[2], 0, "负值被钳到 0");
        bool in_range = true;
        for (int i = 1; i <= 2; ++i) {
            if (bf.bytes[i] > 200) in_range = false;
        }
        check(in_range, "两个限位字节都在设备认可的 0..200 内");
    }
    {
        // 主限位 40(B 通道 30)是当前配置, 确认能原样表达
        const B0 bf = make_bf(40, 30);
        check_eq(bf.bytes[1], 40, "byte[1] = 40");
        check_eq(bf.bytes[2], 30, "byte[2] = 30");
    }

    // -----------------------------------------------------------------------
    group("频率换算: 文档的分段表");

    check_eq(hz_to_pulse_value(10), 10, "10Hz -> 10");
    check_eq(hz_to_pulse_value(100), 100, "100Hz -> 100 (第一段顶端)");
    check_eq(hz_to_pulse_value(600), 200, "600Hz -> 200 (第二段顶端)");
    check_eq(hz_to_pulse_value(1000), 240, "1000Hz -> 240 (第三段顶端)");
    check_eq(hz_to_pulse_value(300), 140, "300Hz -> (300-100)/5+100 = 140");
    check_eq(hz_to_pulse_value(0), 10, "0Hz -> 10 (定义域外, 取最快端)");
    {
        // 单调性: 频率升高时线上值绝不能下降
        bool mono = true;
        uint8_t prev = hz_to_pulse_value(1);
        for (int hz = 2; hz <= 1200; hz += 7) {
            const uint8_t v = hz_to_pulse_value(hz);
            if (v < prev) mono = false;
            prev = v;
        }
        check(mono, "换算函数单调不减");
        check(hz_to_pulse_value(1200) <= 240, "超出 1000Hz 也不会越界");
    }

    // -----------------------------------------------------------------------
    group("整帧长度与十六进制打印");

    {
        const Frame f = make_frame(40, 30);
        const B0 c = encode_b0(f, StrengthMode::Absolute, StrengthMode::Absolute, 1, nullptr, nullptr);
        check(c.to_hex().size() == static_cast<size_t>(kB0Size) * 2, "to_hex 长度 = 40 字符");
        check(c.to_hex().rfind("B0", 0) == 0, "to_hex 以 B0 开头");
        check_eq(kB0Size, 20, "B0 固定 20 字节");
        check_eq(kBfSize, 7, "BF 固定 7 字节");
    }

    // -----------------------------------------------------------------------
    std::printf("\n-------------------------------------------\n");
    std::printf("通过 %d, 失败 %d\n", g_pass, g_fail);
    if (g_fail == 0) {
        std::printf("结论: B0/BF 编码与官方协议一致。\n");
    } else {
        std::printf("结论: 有 %d 项不符 —— 先对着 官方协议(见 PROVENANCE.md) 复核, 不要改测试去迁就代码。\n",
                    g_fail);
    }
    return g_fail == 0 ? 0 : 1;
}
