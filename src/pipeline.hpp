// ---------------------------------------------------------------------------
// DGLab_IDAC — 信号 -> 波形 管线
//
// 这是整个 mod 的核心。职责:
//
//   1. 以 frame_ms(默认 100 ms)为节拍推进
//   2. 把每一帧切成 4 个 25 ms 的"槽"(slot) —— 因为 V3 的 B0 命令里,
//      每个通道的 Pulse 数据是 4 组, 每组管 25 ms
//   3. 每个槽上从 FfbState 采一次值, 经映射 -> 安全限幅 -> 量化成
//      pulse intensity (0..100) 和 pulse frequency (10..240)
//   4. 输出一个可直接编码的 Frame
//
// 设计要点:
//   * 时间推进是**被动的** —— 由调用方(工作线程 / tick)喂 now。
//     这样整个管线可以在没有游戏的情况下单元测试。
//   * 安全逻辑集中在这里, 不散落到传输层。
// ---------------------------------------------------------------------------
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "config.hpp"
#include "ffb_types.hpp"

namespace dglab_idac {

// V3 的 B0 每通道带 4 组 Pulse 数据, 每组 25 ms。这个数字是协议固定的。
inline constexpr int kSlotsPerFrame = 4;

struct Slot {
    uint8_t pulse_intensity = 0;   // 0..100
    uint8_t pulse_frequency = 10;  // 10..240
};

struct ChannelFrame {
    Slot slots[kSlotsPerFrame]{};
    // 该帧使用的通道绝对强度(0..200) —— 只在变化时才会被编码进 B0
    int  strength = 0;
};

struct Frame {
    ChannelFrame a;
    ChannelFrame b;
    bool servo_on   = false;
    bool muted      = false;   // true = 本帧被安全逻辑压制为静音
    // 静音原因, 空串表示没静音。
    // **必须指向静态存储期的字符串**(字符串字面量)—— Frame 会跨线程传
    // (管线 -> 传输层), 指向局部 std::string 的 c_str() 会立刻悬垂。
    const char* mute_reason = "";
};

// 安全相关的运行期状态, 暴露出来给日志/诊断
struct SafetyState {
    bool   watchdog_tripped = false;
    bool   warmup_active    = false;
    bool   servo_off_muted  = false;
    bool   strength_clamped = false;
    double applied_strength = 0.0;   // 经斜率限制后的当前强度
};

// ---------------------------------------------------------------------------
// 自动增益: 把"当前的输入峰值"当作动态 in_max
//
// 包络是快起慢落 —— 峰值上去立刻跟上, 回落时按 release 时间常数慢慢降。
// 这样做的原因见 config.hpp 里 [output] 自动增益那段注释。
//
// active 标志用来区分"没信号"和"信号很小": 没信号时不衰减峰值, 免得
// 菜单里挂机几十秒后增益被磨到 floor, 一发车就糊成平线。
// ---------------------------------------------------------------------------
struct GainTracker {
    double  gain        = 0.0;    // 当前动态 in_max
    double  peak        = 0.0;    // 已观测到的峰值(未衰减)
    double  last_active = -1e9;   // 最近一次收到 >= floor 的信号的时刻(秒)
    bool    primed      = false;  // 是否已经用第一个采样初始化过

    void reset(double start_gain) {
        gain = start_gain;
        peak = start_gain;
        last_active = -1e9;
        primed = false;
    }
};

class Pipeline {
public:
    explicit Pipeline(const Config& cfg);

    // 重新读取配置(热改配置用)
    void reconfigure(const Config& cfg);

    // 推进一步。now 用 steady_clock 秒。
    // 返回 false 表示"这一帧不该发送"(比如被静音且上一次也静音)。
    bool tick(double now, const FfbState& ffb, Frame& out);

    // 帧节拍(毫秒), 供调度器用
    int frame_ms() const { return cfg_.frame_ms; }

    const SafetyState& safety() const { return safety_; }

    // 诊断: 一帧的映射结果摘要
    std::string describe(const Frame& f) const;

    // 诊断: 当前各通道的动态增益, 形如 "peakA=18 gainA=24 peakB=40 gainB=40"
    std::string gain_summary() const;

private:
    // 单通道: 把原始信号映射成 0..1
    double map_input(const ChannelMapping& m, const FfbState& ffb) const;
    // 单通道: 把原始信号映射成帧
    void fill_channel(const ChannelMapping& m, double v, ChannelFrame& out);
    // 单通道: 本帧的目标强度(static = 固定 base_strength, level = 按 v 缩放)
    int  effective_strength(const ChannelMapping& m, double v) const;
    // 安全: 静音判定 + 斜率限制
    // reason 只赋字符串字面量(静态存储期), 详见 pipeline.cpp 里的实现注释。
    bool evaluate_safety(double now, const FfbState& ffb, const char*& reason);
    int  clamp_strength(int want) const;

    // 本帧该用的归一化上界: auto_gain 关闭时就是 m.in_max
    double effective_in_max(const ChannelMapping& m, const FfbState& ffb) const;

    // 自动增益: 每个 tick 用当前原始值推进一次包络, 返回本帧的有效 in_max
    double update_gain(GainTracker& t, const ChannelMapping& m,
                       const FfbState& ffb, double now);

    Config cfg_;
    SafetyState safety_{};

    // 自动增益状态(通道级)
    GainTracker gain_a_{};
    GainTracker gain_b_{};
    double      last_gain_t_ = 0.0;

    // 本帧映射结果(0..1), 供 strength_source=level 与诊断使用
    double last_v_a_ = 0.0;
    double last_v_b_ = 0.0;

    // 斜率限制状态(通道级) —— 这里是"通道强度"的斜率, 不是脉冲强度的
    double applied_strength_ = 0.0;
    double slew_a_           = 0.0;
    double slew_b_           = 0.0;
    double last_tick_        = 0.0;
    bool   started_          = false;
    bool   last_muted_       = false;

    // 启动时刻, 用于 warmup
    double boot_time_        = 0.0;
};

// --- 映射函数的自由函数形式, 便于单测 ---
double apply_map_mode(MapMode mode, double normalized, double level);

}  // namespace dglab_idac
