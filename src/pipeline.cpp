// ---------------------------------------------------------------------------
// DGLab_IDAC — 信号 -> 波形 管线实现
// ---------------------------------------------------------------------------
#include "pipeline.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "logger.hpp"

namespace dglab_idac {

namespace {

inline double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

inline int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// 从 FfbState 里按名字取一个原始整数。取不到时返回 0 并把 ok 置 false。
int32_t pick_source(const std::string& src, const FfbState& f, bool& ok,
                    bool neg_is_pos, int32_t* out_signed = nullptr) {
    ok = true;
    if (out_signed) *out_signed = 0;

    if (src == "torque") {
        // magnitude 已经是绝对值(0..127), 方向单独在 torque_dir 里
        return f.torque_magnitude;
    }
    if (src == "torque_signed") {
        // 按方向恢复符号。这是唯一会用到 direction 的源 —— 用于"回正力"
        // 这类需要区分左右手感的映射。
        int32_t m = f.torque_magnitude;
        // 已证实: -1 = 计算出的扭矩为正(IndRun RVA 0x2BCD1)
        // 强推断: 其它值 = 反方向
        bool positive = (f.torque_dir == static_cast<int32_t>(TorqueDirection::Zero));
        if (neg_is_pos) positive = !positive;
        const int32_t signed_m = positive ? m : -m;
        if (out_signed) *out_signed = signed_m;
        return signed_m;
    }
    if (src == "viscosity") return f.viscosity_b;
    if (src == "viscosity_a") return f.viscosity_a;
    if (src == "vibration") return f.vibration_b;
    if (src == "vibration_a") return f.vibration_a;
    if (src == "servo") return f.servo_on ? 1 : 0;
    if (src == "const") return 0;   // Constant 模式忽略输入

    ok = false;
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// 映射函数(自由函数形式, 便于单测)
//
// normalized 是已经过 (v-in_min)/(in_max-in_min) 的 0..1 值; level 是用户给的
// 输出缩放。Constant 模式忽略 normalized。
// ---------------------------------------------------------------------------
double apply_map_mode(MapMode mode, double normalized, double level) {
    double n = clamp01(normalized);
    switch (mode) {
        case MapMode::Linear:    break;
        case MapMode::AbsLinear: break;   // 取绝对值已在取源时做完
        case MapMode::Sqrt:      n = std::sqrt(n); break;
        case MapMode::Square:    n = n * n; break;
        case MapMode::Threshold:
            // normalized 已经在 in_min 以下时会 <= 0, 这里保持硬归零的语义
            n = (normalized <= 0.0) ? 0.0 : n;
            break;
        case MapMode::Constant:  n = 1.0; break;
    }
    return clamp01(n * level);
}

Pipeline::Pipeline(const Config& cfg) : cfg_(cfg) {
    boot_time_ = 0.0;
    started_ = false;
}

void Pipeline::reconfigure(const Config& cfg) {
    cfg_ = cfg;
    log_info("管线已重新配置: frame=%dms A=%s B=%s max_strength=%d clamp=%d",
             cfg_.frame_ms,
             cfg_.channel_a.source.c_str(), cfg_.channel_b.source.c_str(),
             cfg_.max_strength, cfg_.clamp_strength);
}

// ---------------------------------------------------------------------------
double Pipeline::map_input(const ChannelMapping& m, const FfbState& ffb) const {
    if (!m.enable) return 0.0;
    if (m.mode == MapMode::Constant) return clamp01(m.level);

    bool ok = false;
    int32_t raw = pick_source(m.source, ffb, ok, cfg_.torque_negative_is_positive);
    if (!ok) {
        // 每个 tick 都刷屏没意义, 所以这里不打日志 —— 配置解析时就该发现。
        return 0.0;
    }

    const double span = static_cast<double>(m.in_max) - static_cast<double>(m.in_min);
    if (span <= 0.0) return clamp01(m.level);

    double norm = (static_cast<double>(raw) - static_cast<double>(m.in_min)) / span;

    // Threshold 模式下, 低于 in_min 的值必须硬归零, 不能被后面的 level 抬起来
    if (m.mode == MapMode::Threshold && norm <= 0.0) return 0.0;

    return apply_map_mode(m.mode, norm, m.level);
}

// ---------------------------------------------------------------------------
void Pipeline::fill_channel(const ChannelMapping& m, const FfbState& ffb,
                            double /*dt_slot*/, ChannelFrame& out) {
    const double v = map_input(m, ffb);

    // 脉冲强度(0..100): 直接就是映射出来的比例。
    // 这里不乘通道强度 —— 通道强度是"音量", 脉冲强度是"波形振幅", 两者在
    // 郊狼上是独立的两个量, 分开调才有意义。
    const int intensity = clamp_int(static_cast<int>(std::lround(v * 100.0)), 0, 100);

    // 脉冲频率(10..240): 数值越大 = 单元越长 = 电气频率越低 = 越"敦实"。
    // 所以 v 变大时让值变小(频率变高), 避免大信号变成又慢又猛的刺激。
    // cfg_.pulse_freq_track = 0 时频率恒定在 pulse_freq_min。
    const double track = clamp01(cfg_.pulse_freq_track);
    const double base = static_cast<double>(cfg_.pulse_freq_min);
    const double top  = static_cast<double>(cfg_.pulse_freq_max);
    const double want = base + (top - base) * v * track;
    const int freq = clamp_int(static_cast<int>(std::lround(want)), 10, 240);

    for (int i = 0; i < kSlotsPerFrame; ++i) {
        out.slots[i].pulse_intensity = static_cast<uint8_t>(intensity);
        out.slots[i].pulse_frequency = static_cast<uint8_t>(freq);
    }
}

// ---------------------------------------------------------------------------
bool Pipeline::evaluate_safety(double now, const FfbState& ffb, std::string& reason) {
    safety_.warmup_active   = false;
    safety_.watchdog_tripped = false;
    safety_.servo_off_muted = false;

    if (!started_) {
        boot_time_ = now;
        started_ = true;
    }

    // 1) 启动静默期: 给用户时间确认连接对不对
    if (cfg_.warmup_ms > 0 && (now - boot_time_) * 1000.0 < static_cast<double>(cfg_.warmup_ms)) {
        safety_.warmup_active = true;
        reason = "warmup";
        return true;
    }

    // 2) 看门狗: 游戏停止喂数据(卡死/退出/挂载掉了)时把输出拉回 0。
    //    注意用 ffb.latest_at() 而不是"上次 tick 时间" —— 力反馈回调是
    //    **边沿触发**的(IndRun 内部有变化缓存, 值不变就不回调), 所以
    //    "很久没回调" 完全可能只是"方向盘没动", 不是故障。
    //    真正要防的是"servo 开着但数据流彻底停了", 这个由传输层的
    //    silence 帧兜底; 这里只处理 servo 状态。
    if (!ffb.ever_updated) {
        // 一次都没收到过回调 —— 可能还没进游戏, 也可能 hook 没生效
        reason = "no_ffb_data_yet";
        return true;
    }

    // 3) servo off: 游戏明确说"别反馈了"
    if (cfg_.zero_when_servo_off && !ffb.servo_on) {
        safety_.servo_off_muted = true;
        reason = "servo_off";
        return true;
    }

    reason.clear();
    return false;
}

// ---------------------------------------------------------------------------
int Pipeline::clamp_strength(int want) const {
    // 两道限制: [safety] clamp_strength 和 [safety] max_strength。
    // 配置加载时已经保证 clamp_strength <= max_strength, 这里再取 min 兜底。
    const int hard = (std::min)(cfg_.clamp_strength, cfg_.max_strength);
    return clamp_int(want, 0, hard);
}

// ---------------------------------------------------------------------------
bool Pipeline::tick(double now, const FfbState& ffb, Frame& out) {
    out = Frame{};
    out.servo_on = ffb.servo_on;

    const double dt = (last_tick_ > 0.0) ? (now - last_tick_) : 0.0;
    last_tick_ = now;

    std::string reason;
    const bool muted = evaluate_safety(now, ffb, reason);

    // ---- 目标通道强度 ----
    // 设计原则(见 docs/research-coyote-protocol.md §波形设计):
    // 通道强度是慢变量, 每 100 ms 最多动 slew 允许的量; 快变化全部走
    // 脉冲强度(波形振幅)。绝对不让游戏数据直接把通道强度打到满。
    const double v_a = map_input(cfg_.channel_a, ffb);
    const double v_b = map_input(cfg_.channel_b, ffb);

    int want_a = cfg_.channel_a.enable
                     ? static_cast<int>(std::lround(v_a * cfg_.channel_a.base_strength))
                     : 0;
    int want_b = cfg_.channel_b.enable
                     ? static_cast<int>(std::lround(v_b * cfg_.channel_b.base_strength))
                     : 0;

    if (muted) {
        want_a = 0;
        want_b = 0;
    }

    want_a = clamp_strength(want_a);
    want_b = clamp_strength(want_b);

    // ---- 斜率限制(每秒允许的变化量) ----
    const double limit = (cfg_.slew_per_second > 0.0)
                             ? cfg_.slew_per_second * (dt > 0.0 ? dt : 0.0)
                             : 1e9;
    if (applied_strength_ < 0.0) applied_strength_ = 0.0;
    // 每通道独立斜率。累加器是**成员**, 不是 static —— 否则多个 Pipeline
    // 实例(单测里会同时存在)会互相干扰。
    auto slew = [&](double& state, int target) -> int {
        const double t = static_cast<double>(target);
        if (state < t) {
            state = (std::min)(t, state + limit);
        } else if (state > t) {
            state = (std::max)(t, state - limit);
        }
        return static_cast<int>(std::lround(state));
    };
    const int final_a = slew(slew_a_, want_a);
    const int final_b = slew(slew_b_, want_b);
    safety_.applied_strength = static_cast<double>((std::max)(final_a, final_b));

    // 静音时强度必须是精确 0, 不能被斜率还在下滑的中间值拖住
    out.a.strength = muted ? 0 : final_a;
    out.b.strength = muted ? 0 : final_b;

    out.muted = muted;
    static const char* const kNoReason = "";
    out.mute_reason = muted ? reason.c_str() : kNoReason;

    // ---- 脉冲内容 ----
    if (muted) {
        // 波形强度归零 = 设备上就是"有电压没振幅", 什么都不感觉。
        for (int i = 0; i < kSlotsPerFrame; ++i) {
            out.a.slots[i].pulse_intensity = 0;
            out.a.slots[i].pulse_frequency = static_cast<uint8_t>(
                clamp_int(cfg_.pulse_freq_min, 10, 240));
            out.b.slots[i].pulse_intensity = 0;
            out.b.slots[i].pulse_frequency = static_cast<uint8_t>(
                clamp_int(cfg_.pulse_freq_min, 10, 240));
        }
    } else {
        const double dt_slot = (static_cast<double>(cfg_.frame_ms) / 1000.0) / kSlotsPerFrame;
        fill_channel(cfg_.channel_a, ffb, dt_slot, out.a);
        fill_channel(cfg_.channel_b, ffb, dt_slot, out.b);
    }

    // 返回值: 静音状态发生变化时必须返回 true(让传输层知道要改强度),
    // 否则连续静音时可以省略重复帧。但为了简单和可预测, 一律返回 true ——
    // B0 本来就要求每 100 ms 发一次, 少发反而会让设备保持旧值。
    last_muted_ = muted;
    return true;
}

// ---------------------------------------------------------------------------
std::string Pipeline::describe(const Frame& f) const {
    char buf[512];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "A(str=%d) slot0{i=%u,f=%u} | B(str=%d) slot0{i=%u,f=%u} | %s%s",
                f.a.strength,
                static_cast<unsigned>(f.a.slots[0].pulse_intensity),
                static_cast<unsigned>(f.a.slots[0].pulse_frequency),
                f.b.strength,
                static_cast<unsigned>(f.b.slots[0].pulse_intensity),
                static_cast<unsigned>(f.b.slots[0].pulse_frequency),
                f.muted ? "MUTED " : "",
                f.mute_reason ? f.mute_reason : "");
    return buf;
}

}  // namespace dglab_idac
