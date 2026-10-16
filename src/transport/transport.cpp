// ---------------------------------------------------------------------------
// DGLab_IDAC — 传输层公共实现: B0 编码 / BF 软限位 / 频率换算
//
// 这里是**安全关键代码**。任何越界值都会让郊狼整通道静默(协议规定: 通道内
// 8 个值只要有一个越界, 该通道整帧被丢弃), 所以这里采取"宁可关掉, 也不
// 送非法值"的策略, 并显式钳制每一个字节。
// ---------------------------------------------------------------------------
#include "transport/transport.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "logger.hpp"

namespace dglab_idac {

namespace {

inline uint8_t clamp_u8(int v, int lo, int hi) {
    return static_cast<uint8_t>(v < lo ? lo : (v > hi ? hi : v));
}

}  // namespace

std::string B0::to_hex() const {
    char buf[kB0Size * 2 + 1];
    for (int i = 0; i < kB0Size; ++i) {
        _snprintf_s(buf + i * 2, 3, _TRUNCATE, "%02X", bytes[i]);
    }
    buf[kB0Size * 2] = '\0';
    return buf;
}

// ---------------------------------------------------------------------------
// 频率换算。
//
// 协议规定: 写入 B0 的脉冲频率字节, 其含义是**输出单元的时长(ms)**, 不是
// 赫兹 —— 值越小电气频率越高。官方给了"输入频率(Hz) -> 线上值"的分段表:
//     10..100    -> v                 (原样)
//     101..600   -> (v-100)/5 + 100
//     601..1000  -> (v-600)/10 + 200
//     其它       -> 10
// 本函数实现的就是这张表。要直接写"时长(ms)"的话就不要调它。
// ---------------------------------------------------------------------------
uint8_t hz_to_pulse_value(double hz) {
    if (!(hz > 0.0) || !std::isfinite(hz)) return 10;

    double v;
    if (hz < 10.0) {
        v = 10.0;
    } else if (hz <= 100.0) {
        v = hz;
    } else if (hz <= 600.0) {
        v = (hz - 100.0) / 5.0 + 100.0;
    } else if (hz <= 1000.0) {
        v = (hz - 600.0) / 10.0 + 200.0;
    } else {
        return 240;   // 超出定义域, 给最"慢"的那一端(240)而不是 10(最猛)
    }

    const int i = static_cast<int>(std::lround(v));
    return clamp_u8(i, 10, 240);
}

// ---------------------------------------------------------------------------
// B0 编码
//
// 模式语义(协议原文):
//   00 绝对设置   01 相对加   10 相对减   11 不改强度(保留原值)
//
// 关键细节: **只有模式不是 11 时, 字节 2/3 才会被解释**。模式为 11 时该
// 通道的强度值不被使用。所以"不改强度"的帧里字节 2/3 写什么都可以 ——
// 我们写 0, 但**绝不**指望它等于"强度归零"。这也正是文档里"只输出单通道
// 时给另一通道塞一个非法值"的做法能成立的原因。
// ---------------------------------------------------------------------------
B0 encode_b0(const Frame& f, StrengthMode mode_a, StrengthMode mode_b, uint8_t seq4, bool* out_a_valid,
             bool* out_b_valid) {
    B0 cmd{};
    cmd.bytes[0] = 0xB0;
    cmd.bytes[1] = 0;

    const bool a_active = f.a.strength > 0 && !f.muted;
    const bool b_active = f.b.strength > 0 && !f.muted;

    // ---- 强度 ----
    // 强度模式由调用方(传输层的序列号状态机)决定, 这里只负责写值。
    // servo 未开 / 静音时, 强度必须是精确 0, 且绝不能留在"不改强度"模式。
    StrengthMode ma = mode_a;
    StrengthMode mb = mode_b;
    if (f.muted || f.a.strength <= 0) {
        // 想降到 0 就必须真的写 0; Discard 会让设备保持上一个强度
        if (ma == StrengthMode::Discard) ma = StrengthMode::Absolute;
    }
    if (f.muted || f.b.strength <= 0) {
        if (mb == StrengthMode::Discard) mb = StrengthMode::Absolute;
    }

    cmd.set_seq(seq4);
    cmd.set_mode_a(ma);
    cmd.set_mode_b(mb);
    cmd.bytes[2] = clamp_u8(f.muted ? 0 : f.a.strength, 0, 200);
    cmd.bytes[3] = clamp_u8(f.muted ? 0 : f.b.strength, 0, 200);

    // ---- 脉冲数据 ----
    // 设计选择: 通道没在输出时(B 通道大部分时间如此)不要让它的脉冲频率
    // 取默认值 0 —— 0 是非法值, 会让**整个通道**被丢弃, 包括强度。这在
    // "以后想给 B 通道加东西"时会变成隐蔽的坑。所以这里给合法最小值 10。
    const uint8_t idle_freq = 10;
    const uint8_t zero_int  = 0;

    for (int i = 0; i < kSlotsPerFrame; ++i) {
        const Slot& sa = f.a.slots[i];
        const Slot& sb = f.b.slots[i];

        const uint8_t fa = a_active ? clamp_u8(sa.pulse_frequency, 10, 240) : idle_freq;
        const uint8_t ia = a_active ? clamp_u8(sa.pulse_intensity, 0, 100) : zero_int;
        const uint8_t fb = b_active ? clamp_u8(sb.pulse_frequency, 10, 240) : idle_freq;
        const uint8_t ib = b_active ? clamp_u8(sb.pulse_intensity, 0, 100) : zero_int;

        cmd.bytes[4 + i]  = fa;
        cmd.bytes[8 + i]  = ia;
        cmd.bytes[12 + i] = fb;
        cmd.bytes[16 + i] = ib;
    }

    // ---- 自检: 编码后绝不允许有非法值漏出去 ----
    bool a_ok = true, b_ok = true;
    for (int i = 0; i < kSlotsPerFrame; ++i) {
        if (cmd.bytes[4 + i] < 10 || cmd.bytes[4 + i] > 240) a_ok = false;
        if (cmd.bytes[8 + i] > 100) a_ok = false;
        if (cmd.bytes[12 + i] < 10 || cmd.bytes[12 + i] > 240) b_ok = false;
        if (cmd.bytes[16 + i] > 100) b_ok = false;
    }
    if (cmd.bytes[2] > 200 || cmd.bytes[3] > 200) {
        a_ok = b_ok = false;
    }
    if (!a_ok || !b_ok) {
        // 走到了就说明上面的钳制有 bug。整帧作废, 强制静音重来。
        log_error("B0 编码自检失败 (a=%d b=%d) —— 本帧作废并强制静音", a_ok ? 1 : 0, b_ok ? 1 : 0);
        B0 silent{};
        silent.bytes[0] = 0xB0;
        silent.set_seq(seq4);
        silent.set_mode_a(StrengthMode::Absolute);
        silent.set_mode_b(StrengthMode::Absolute);
        silent.bytes[2] = 0;
        silent.bytes[3] = 0;
        for (int i = 0; i < kSlotsPerFrame; ++i) {
            silent.bytes[4 + i]  = idle_freq;
            silent.bytes[8 + i]  = 0;
            silent.bytes[12 + i] = idle_freq;
            silent.bytes[16 + i] = 0;
        }
        if (out_a_valid) *out_a_valid = false;
        if (out_b_valid) *out_b_valid = false;
        return silent;
    }

    if (out_a_valid) *out_a_valid = a_ok;
    if (out_b_valid) *out_b_valid = b_ok;
    return cmd;
}

// ---------------------------------------------------------------------------
// BF 软限位
//
// 7 字节: BF | A上限 | B上限 | A频率平衡 | B频率平衡 | 0 | 0
//
// ⚠ 这个命令是最后一道安全阀: B0 里写的强度无论如何都突破不了它。
//   它**断电保存**, 而且**立即生效、没有回应** —— 所以每次重连后都必须
//   重发一次, 不能假设设备还记得。
//
// ⚠ 已知未确认项: A/B 的字节顺序是照文档措辞推断的。这是安全阀, 顺序错了
//   很危险(把给 B 的上限写到 A 上)。所以默认两个上限取同一个值 —— 这样
//   即使顺序反了也不改变效果。确实需要 A≠B 时请先在硬件上单独验证。
// ---------------------------------------------------------------------------
B0 make_bf(int limit_a, int limit_b, int freq_bal_a, int freq_bal_b) {
    B0 cmd{};
    cmd.bytes[0] = 0xBF;
    cmd.bytes[1] = clamp_u8(limit_a, 0, 200);
    cmd.bytes[2] = clamp_u8(limit_b, 0, 200);
    cmd.bytes[3] = clamp_u8(freq_bal_a, 0, 255);
    cmd.bytes[4] = clamp_u8(freq_bal_b, 0, 255);
    cmd.bytes[5] = 0;
    cmd.bytes[6] = 0;
    return cmd;
}

}  // namespace dglab_idac
