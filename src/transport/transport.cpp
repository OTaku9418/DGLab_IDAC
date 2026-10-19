// ---------------------------------------------------------------------------
// DGLab_IDAC — 传输层公共实现: B0 编码 / BF 软限位 / 频率换算
//
// 这里是**安全关键代码**。任何越界值都会让郊狼整通道静默(协议规定: 通道内
// 8 个值只要有一个越界, 该通道整帧被丢弃), 所以这里采取"宁可关掉, 也不
// 送非法值"的策略, 并显式钳制每一个字节。
// ---------------------------------------------------------------------------
#include "transport/transport.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "logger.hpp"

namespace dglab_idac {

// ===========================================================================
// 帧信箱 + 看门狗
// ===========================================================================

int64_t steady_now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

Frame make_idle_frame(const char* reason) {
    Frame f;
    f.servo_on = false;
    f.muted = true;
    f.mute_reason = reason ? reason : "idle";
    for (int i = 0; i < kSlotsPerFrame; ++i) {
        f.a.slots[i].pulse_intensity = 0;
        f.a.slots[i].pulse_frequency = 10;
        f.b.slots[i].pulse_intensity = 0;
        f.b.slots[i].pulse_frequency = 10;
    }
    f.a.strength = 0;
    f.b.strength = 0;
    return f;
}

void FrameMailbox::put(const Frame& f, int64_t now_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    // 上一帧还没被取走就又来一帧 = 传输线程跟不上, 这一帧直接丢弃。
    // 队列深度必须是 1: 发"最新"的波形, 而不是排队发"过时"的。
    // 但"消费端还没就绪"(链路没连上)时的覆盖不是传输跟不上, 单独记一栏,
    // 否则每局开头那 3 秒都会凭空贡献 20 多次 over(T13c)。
    if (has_pending_ && put_count_ > taken_count_) {
        if (consumer_ready_.load(std::memory_order_relaxed)) {
            ++stats_.overwritten;
        } else {
            ++stats_.overwritten_pre;
        }
    }
    pending_ = f;
    has_pending_ = true;
    last_put_ms_ = now_ms;
    ++put_count_;
    ++stats_.submitted;
}

void FrameMailbox::set_consumer_ready(bool ready) {
    consumer_ready_.store(ready, std::memory_order_relaxed);
}

bool FrameMailbox::consumer_ready() const {
    return consumer_ready_.load(std::memory_order_relaxed);
}

MailboxTake FrameMailbox::take(int64_t now_ms, int watchdog_ms) {
    MailboxTake out;
    bool trip_now = false;
    bool untrip_now = false;
    int64_t stalled_for = 0;

    {
        std::lock_guard<std::mutex> lk(mu_);

        if (!has_pending_) {
            // 从来没有人 put 过 —— 这是"启动后还没进游戏", 不是故障。
            out.frame = make_idle_frame("idle");
            ++stats_.idle_sent;
        } else {
            // 看门狗: 距上次 put 超过 watchdog_ms 就认为管线侧断了。
            // 判据是"上一次 put 的时刻", 不是"距离上次 take" —— 后者在
            // 传输线程自己节拍正常时会永远为 0, 看门狗就失效了。
            const int64_t gap = now_ms - last_put_ms_;
            const bool stale = (watchdog_ms > 0) && (gap > static_cast<int64_t>(watchdog_ms));

            if (stale) {
                out.frame = make_idle_frame("watchdog");
                out.stale = true;
                ++stats_.idle_sent;
                if (!tripped_) {
                    tripped_ = true;
                    trip_now = true;
                    stalled_for = gap;
                    ++stats_.watchdog_trips;
                }
            } else {
                if (tripped_) {
                    tripped_ = false;
                    untrip_now = true;
                }
                out.frame = pending_;
                if (put_count_ > taken_count_) {
                    taken_count_ = put_count_;
                    out.fresh = true;
                    ++stats_.taken_fresh;
                } else {
                    ++stats_.repeated;
                }
            }
        }
    }  // 锁在这里释放, 下面打日志

    // 日志只在"状态切换"时打, 不是每帧 —— 停顿持续 3 秒也只有一条告警。
    if (trip_now) {
        log_warn("看门狗触发: 管线 %lld ms 没有提交新帧 (watchdog_ms=%d), 改发静默帧",
                 static_cast<long long>(stalled_for), watchdog_ms);
    } else if (untrip_now) {
        log_info("看门狗恢复: 管线重新开始提交帧");
    }
    return out;
}

MailboxStats FrameMailbox::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stats_;
}

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
        v = 240.0;   // 超出定义域, 给最"慢"的那一端(240)而不是 10(最猛的)
    }

    // 注意钳位两端是不对称的: 高端的 240 是合法的(最慢), 低端的 10 也是
    // 合法的(最快), 中间任何值都合法。clamp 只是防御 lround 的边界。
    const int i = static_cast<int>(std::lround(v));
    return clamp_u8(i, 10, 240);
}

// ---------------------------------------------------------------------------
// B0 编码
//
// 模式语义(官方文档原文, 见
// https://github.com/dungeonlab-open/dglab-bluetooth-protocol/blob/045720d5c5ee4879e25b7bad555c962f97ed423f/coyote/v3/README.md
// 的 "Intensity Interpretation Mode" 表; 链接与校验和见
// re/reference/dglab-bluetooth-protocol/PROVENANCE.md):
//   0b00 不变        —— 忽略本通道强度字节, 维持设备当前值
//   0b01 相对增加
//   0b10 相对减少
//   0b11 绝对设置
//
// ⚠ 这四行曾经写成 00=绝对/11=不变, 与文档正好相反, 而且枚举也是照这个
//   反的对应关系写的 —— 于是两个方向一起错, 编译和自检都发现不了, 只有
//   设备不动作。改这里之前先把官方表格再对一遍。
//
// 关键细节: **只有模式不是 0b00 时, 字节 2/3 才会被解释**。模式为 0b00
// 时该通道的强度值不被使用。所以"不改强度"的帧里字节 2/3 写什么都可以 ——
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
    // servo 未开 / 静音 / 强度为 0 时, 强度必须是精确 0, 所以**绝不能留在
    // "不变"模式** —— 0b00 会让设备忽略强度字节、维持上一个值, 那样静音就
    // 静不掉(曾经就是这样: 枚举值写反, 于是每次静音都指令被忽略, 设备一直
    // 停在旧强度上)。
    StrengthMode ma = mode_a;
    StrengthMode mb = mode_b;
    if (f.muted || f.a.strength <= 0) {
        if (ma == StrengthMode::Unchanged) ma = StrengthMode::Absolute;
    }
    if (f.muted || f.b.strength <= 0) {
        if (mb == StrengthMode::Unchanged) mb = StrengthMode::Absolute;
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
