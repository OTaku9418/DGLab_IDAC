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

inline double clamp_d(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// 一阶趋近: 把 cur 朝 target 推 dt/tau 的比例。tau<=0 时直接跳到 target。
inline double smooth(double cur, double target, double dt, double tau) {
    return cur + (target - cur) * ((tau > 0.0) ? clamp01(dt / tau) : 1.0);
}

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
        // ★ 这个名字有误导性: 它恢复出来的**不是左右方向**。
        // 逆向已证实 (re/indrun_hook_mechanics.md 第 4.1/5 节): torque 的 direction
        // 参数取 0xFFFFFFFF(-1) 只表示"算出的扭矩为正", 其余情况装的是
        // (uint8_t)device[0x1678] —— 一个齿轮/档位索引, 域 0..255。
        // 所以下面这个"非 -1 即负"的判据只有在 direction 恰好是 ±1 时才成立,
        // 拿到档位值时符号是任意的。保留它只为兼容既有配置, 新配置请用 torque。
        int32_t m = f.torque_magnitude;
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
    // 增益从 in_max 起步而不是从 floor 起步: 起步瞬间如果有大信号,
    // 用 in_max 归一化得到的输出是"按静态标定应当给出的力度", 不会突刺。
    // 0.4 秒后包络自己会滑到真实峰值那一档。
    gain_a_.reset(static_cast<double>(cfg_.channel_a.in_max));
    gain_b_.reset(static_cast<double>(cfg_.channel_b.in_max));
}

void Pipeline::reconfigure(const Config& cfg) {
    const int32_t old_a_max = cfg_.channel_a.in_max;
    const int32_t old_b_max = cfg_.channel_b.in_max;
    cfg_ = cfg;
    log_info("管线已重新配置: frame=%dms A=%s B=%s max_strength=%d clamp=%d",
             cfg_.frame_ms,
             cfg_.channel_a.source.c_str(), cfg_.channel_b.source.c_str(),
             cfg_.max_strength, cfg_.clamp_strength);
    // 标定变了就把包络重置到新的起点, 否则会拿旧工况的峰值去归一化新标定。
    if (cfg_.channel_a.in_max != old_a_max || !cfg_.auto_gain) {
        gain_a_.reset(static_cast<double>(cfg_.channel_a.in_max));
    }
    if (cfg_.channel_b.in_max != old_b_max || !cfg_.auto_gain) {
        gain_b_.reset(static_cast<double>(cfg_.channel_b.in_max));
    }
}

// ---------------------------------------------------------------------------
// 自动增益: 用当前原始值推进一次包络, 返回本帧的有效 in_max。
//
// 为什么不是"整个会话的最大值": 一次会话里既有发车时的低速轻手感, 也有
// 高速弯的重手感; 取全局最大值会让前半段全部糊在底部。所以要让它落回来。
// ---------------------------------------------------------------------------
double Pipeline::update_gain(GainTracker& t, const ChannelMapping& m,
                             const FfbState& ffb, double now) {
    // 关闭时直接返回静态 in_max —— 自动增益完全不介入, 行为与加它之前一致。
    if (!cfg_.auto_gain) return static_cast<double>(m.in_max);

    bool ok = false;
    const int32_t raw = pick_source(m.source, ffb, ok, cfg_.torque_negative_is_positive);
    const double v = ok ? static_cast<double>(raw < 0 ? -raw : raw) : 0.0;

    double hi = static_cast<double>(cfg_.gain_ceiling);
    if (static_cast<double>(m.in_max) > hi) hi = static_cast<double>(m.in_max);
    // 下限不能超过上限, 否则区间是负的
    const double lo = (std::min)(static_cast<double>((std::max)(1, cfg_.gain_floor)), hi);

    const double dt = (last_gain_t_ > 0.0) ? (now - last_gain_t_) : 0.0;
    const bool active = (v >= lo);

    if (active) t.last_active = now;

    if (active && v > t.peak) {
        // 快起: 峰值上去时按 attack 时间跟上
        t.peak = smooth(t.peak, v, dt, cfg_.gain_attack_sec);
    } else if (now - t.last_active > cfg_.gain_idle_reset_sec) {
        // 没信号(菜单/暂停): 把峰值收回到它的下限, 下次发车重新观察。
        // 不这么做的话, 一次高速弯的大峰值会跟一整天, 之后的低速段全被压扁。
        t.peak = smooth(t.peak, lo, dt, cfg_.gain_release_sec);
    } else if (v < t.peak) {
        // 慢落: 信号回来但比峰值小, 缓慢释放, 免得松手瞬间增益暴涨
        t.peak = smooth(t.peak, v, dt, cfg_.gain_release_sec);
    }

    t.primed = true;
    t.gain = clamp_d(t.peak, lo, hi);
    return t.gain;
}

// ---------------------------------------------------------------------------
double Pipeline::effective_in_max(const ChannelMapping& m,
                                  const FfbState& ffb) const {
    const bool is_a = (&m == &cfg_.channel_a);
    const GainTracker& t = is_a ? gain_a_ : gain_b_;
    if (!cfg_.auto_gain) return static_cast<double>(m.in_max);
    return t.gain;
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

    const double span = effective_in_max(m, ffb) - static_cast<double>(m.in_min);
    if (span <= 0.0) return clamp01(m.level);

    double norm = (static_cast<double>(raw) - static_cast<double>(m.in_min)) / span;

    // Threshold 模式下, 低于 in_min 的值必须硬归零, 不能被后面的 level 抬起来
    if (m.mode == MapMode::Threshold && norm <= 0.0) return 0.0;

    return apply_map_mode(m.mode, norm, m.level);
}

// ---------------------------------------------------------------------------
// 该通道本帧的目标强度。
//
// static(默认): 只要通道启用就固定输出 base_strength —— 它扮演的是设备上的
//   "音量", 由用户配置决定, 不跟游戏信号抖动。全部动态交给脉冲振幅。
// level: 用映射值缩放 base_strength, 叠加 strength_margin 消除零信号死区。
//   这是早期行为, 保留下来做 A/B 对照。
// ---------------------------------------------------------------------------
int Pipeline::effective_strength(const ChannelMapping& m, double v) const {
    if (!m.enable) return 0;
    if (m.strength_source == "level") {
        const double margin = clamp01(m.strength_margin);
        const double f = margin + (1.0 - margin) * clamp01(v);
        return static_cast<int>(std::lround(f * static_cast<double>(m.base_strength)));
    }
    return m.base_strength;
}

// ---------------------------------------------------------------------------
void Pipeline::fill_channel(const ChannelMapping& m, double v, ChannelFrame& out) {
    // 脉冲强度(0..100): 这里承载**全部**的游戏动态。
    // 通道强度是音量、脉冲强度是波形振幅, 两者在郊狼上是独立的量。
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
// reason 是**输出参数**, 类型是 const char* 而不是 std::string: 它最终会被存进
// Frame::mute_reason, 而 Frame 是要跨线程传的(管线线程 -> BLE 线程)。传
// std::string 的话调用方会拿到 c_str() 指向的临时缓冲, tick() 一返回就成了野指针。
// 所以这里只允许赋字符串字面量(静态存储期), 不做任何拼接。
bool Pipeline::evaluate_safety(double now, const FfbState& ffb, const char*& reason) {
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

    // 2) 看门狗位置说明: 真正"数据流停了就归零"的看门狗不在这里, 在
    //    src/transport/transport.cpp 的 FrameMailbox 里(声明在 transport.hpp)
    //    —— 它盯的是"管线 -> 传输"
    //    这一段是否还在出帧, 停了就改发静默帧(强度 0、无脉冲), 配置项是
    //    [safety] watchdog_ms, 最小值被钳到 3×frame_ms。
    //    这里只做"从来没有数据"和"servo 状态"两件事。
    //    为什么不在这里按 FFB 回调间隔判断: 回调是**边沿触发**的(IndRun 内部
    //    有变化缓存, 值不变就不回调), "很久没回调" 通常只是"方向盘没动",
    //    不是故障。按它判断会误归零。
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

    reason = "";
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

    const double dt_raw = (last_tick_ > 0.0) ? (now - last_tick_) : 0.0;

    // ---- 看门狗 ----
    // 判据是"本 tick 距上一 tick 太久", 也就是**管线自己停顿过**(主线程被
    // 别的东西占住、断点、机器休眠)。这不是"游戏没给数据"—— 力反馈回调是
    // 边沿触发的, 力不变时不回调完全正常, 拿它做判据会一直误触发。
    const bool stalled = (last_tick_ > 0.0) && (cfg_.watchdog_ms > 0) &&
                         (dt_raw * 1000.0 > static_cast<double>(cfg_.watchdog_ms));
    last_tick_ = now;
    // 停顿后绝不能拿 dt_raw 算斜率: 那等于把停顿时长一次性乘进斜率里,
    // 一帧就从 0 跳到满值。按 dt=0 处理, 本帧强度保持不变。
    const double dt = stalled ? 0.0 : dt_raw;

    if (stalled) {
        // 安全起步: 强度累积清零 + 重新预热, 让手感从 0 慢慢爬回去,
        // 而不是"停顿结束后突然满力"。
        slew_a_ = 0.0;
        slew_b_ = 0.0;
        applied_strength_ = 0.0;
        boot_time_ = now;
        log_warn("管线停顿 %.0f ms 超过 watchdog_ms=%d, 输出归零并重新预热",
                 dt_raw * 1000.0, cfg_.watchdog_ms);
    }

    const char* reason = "";
    const bool muted = evaluate_safety(now, ffb, reason);
    // 传给上层(诊断用)。放在 evaluate_safety 之后, 免得覆盖里边写的标志。
    safety_.watchdog_tripped = stalled;


    // ---- 自动增益: 每 tick 推进一次包络, 供本帧的 A/B 两路归一化使用 ----
    // 必须在 map_input 之前, 因为 map_input 读的是本帧算出来的增益。
    update_gain(gain_a_, cfg_.channel_a, ffb, now);
    update_gain(gain_b_, cfg_.channel_b, ffb, now);
    last_gain_t_ = now;

    // ---- 目标通道强度 ----
    // 通道强度是慢变量, 每 100 ms 最多动 slew 允许的量; 快变化全部走
    // 脉冲强度(波形振幅)。默认 strength_source=static, 强度只由配置决定,
    // 游戏数据完全不参与 —— 见 config.hpp 里那个字段的注释。
    const double v_a = map_input(cfg_.channel_a, ffb);
    const double v_b = map_input(cfg_.channel_b, ffb);
    last_v_a_ = v_a;
    last_v_b_ = v_b;

    // 先过配置钳制, 再交给斜率。静音时目标就是 0 —— 让斜率把它平滑带下去,
    // 而不是硬切。
    int want_a = clamp_strength(effective_strength(cfg_.channel_a, v_a));
    int want_b = clamp_strength(effective_strength(cfg_.channel_b, v_b));
    if (muted) {
        want_a = 0;
        want_b = 0;
    }

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
    // reason 里只可能是字符串字面量(见 evaluate_safety 的注释), 所以这个指针
    // 的生命周期是整个进程, 跨线程存下来也不会悬垂。
    out.mute_reason = reason;

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
        fill_channel(cfg_.channel_a, v_a, out.a);
        fill_channel(cfg_.channel_b, v_b, out.b);
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

// ---------------------------------------------------------------------------
std::string Pipeline::gain_summary() const {
    char buf[192];
    if (!cfg_.auto_gain) {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "auto=off A(in_max=%d) B(in_max=%d)",
                    cfg_.channel_a.in_max, cfg_.channel_b.in_max);
        return buf;
    }
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "auto=on peakA=%.0f gainA=%.0f | peakB=%.0f gainB=%.0f",
                gain_a_.peak, gain_a_.gain, gain_b_.peak, gain_b_.gain);
    return buf;
}

}  // namespace dglab_idac
