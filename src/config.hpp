// ---------------------------------------------------------------------------
// DGLab_IDAC — 配置
//
// 单文件 INI。用户要求: 连接方式(WS/BLE)通过配置文件切换, 并且配置文件里
// 要留有控制最大强度的接口。
//
// 所有字段都有编译期默认值 —— 文件缺失或某项缺失时不会崩, 只会在日志里说。
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>

namespace dglab_idac {

enum class LinkMode { WebSocket, Ble, Disabled };
enum class ProtocolVersion { V2, V3 };

// ---------------------------------------------------------------------------
// 原始整数 -> 0..1 的映射。
//
// 力反馈量的物理范围还没标定(见 docs/research-overview.md §8), 所以这里
// 把标定做成配置项: 改 INI 就能标定, 不用重新编译。
// ---------------------------------------------------------------------------
enum class MapMode {
    Linear,        // 线性: (v - in_min) / (in_max - in_min)
    AbsLinear,     // 取绝对值后线性
    Sqrt,          // 线性后再开方 —— 小信号更明显
    Square,        // 线性后再平方 —— 大信号更明显
    Threshold,     // 低于 in_min 直接归零, 其余线性
    Constant,      // 忽略输入, 恒为 level
};

// 一个输出通道的映射配置
struct ChannelMapping {
    bool        enable      = true;
    MapMode     mode        = MapMode::AbsLinear;

    // 输入范围(原始整数)
    int32_t     in_min      = 0;
    int32_t     in_max      = 1000;

    // 输出 0..1 域内的缩放与偏置
    double      level       = 1.0;

    // 该通道的基础强度(写入 B0 的 channel intensity setting, 0..200)
    // 上限还会被 [safety] max_strength 和 BF 软限位双重压住。
    int32_t     base_strength = 60;

    // 通道强度的来源:
    //   "static" = 固定用 base_strength, 全部动态走脉冲振幅(默认, 见下)
    //   "level"  = 用映射值缩放 base_strength(旧行为, 留作 A/B)
    //
    // 为什么默认 static: 通道强度是"音量", 脉冲强度是"波形振幅", 两者独立。
    // 游戏每 ~10ms 就推一次力反馈(实测 viscosity 一段赛道 10913 次), 把它
    // 接到通道强度上就等于让强度跟着游戏抖, 既过不了斜率也毫无层次; 而脉冲
    // 振幅本来就能 100ms 内自由变化。所以动态全部交给脉冲, 强度只作音量。
    std::string strength_source = "static";
    // strength_source=level 时, 映射值到通道强度的下限比例。
    // 例如 0.2 表示信号为 0 时强度也有 base_strength 的 20%(手感连续, 不留死区)。
    double      strength_margin = 0.0;

    // 该通道映射哪个信号源
    std::string source      = "torque";   // torque | viscosity | vibration | const
};

struct Config {
    // ---- [general] ----
    bool        enabled             = true;
    bool        verbose             = true;
    std::string log_file            = "";      // 空 = 只写 UE4SS 日志

    // ---- [link] ----
    // ★ 默认值必须是最安全的状态 ★
    // 以前默认 WebSocket, 而 WebSocket 根本没实现 —— "忘了配 mode"的结果是
    // 什么都不连, 日志上却一切正常。默认 Disabled 至少是明确地不做任何事。
    LinkMode         link            = LinkMode::Disabled;
    ProtocolVersion  protocol        = ProtocolVersion::V3;

    // ---- [websocket] ----
    // 本机做服务端, DGLab 客户端/APP 连过来。需要本机蓝牙的那个方案不用它。
    std::string ws_bind_address     = "127.0.0.1";
    uint16_t    ws_port             = 9999;
    std::string ws_path             = "/dglab";
    // 简单握手口令: 非空时要求客户端在 query 里带 ?token=...
    std::string ws_token            = "";
    int         ws_heartbeat_ms     = 10000;

    // ---- [ble] ----
    // 本机蓝牙直连(用户机器是 Intel AX210, 带蓝牙)。
    std::string ble_device_name     = "47L121000";  // V3 刺激器 3.0 的广播名
    // V2/V3 的 GATT 参数由协议文档定义, 这里只在需要非标设备时覆盖。
    std::string ble_service_uuid    = "";           // 空 = 用协议默认
    std::string ble_write_uuid      = "";
    std::string ble_notify_uuid     = "";
    int         ble_reconnect_ms    = 3000;
    int         ble_connect_timeout_ms = 15000;

    // ---- [output] ----
    // 帧节拍。V3 的 B0 必须每 100 ms 发一次, 每次带 4 个 25 ms 的组。
    int         frame_ms            = 100;
    // 通道 A / B 的映射
    ChannelMapping channel_a;   // 默认: 扭矩
    ChannelMapping channel_b;   // 默认: 振动

    // 脉冲频率(Pulse frequency value, 10..240)。
    // 注意这个字段是"输出单元的时长(ms)", 不是赫兹 —— 10 = 100 Hz。
    int         pulse_freq_base     = 30;    // 默认 ~33 ms 周期
    int         pulse_freq_min      = 10;
    int         pulse_freq_max      = 60;
    // 频率是否跟随振动信号调制(0 = 恒定)
    double      pulse_freq_track    = 0.0;

    // ---- [output] 自动增益 ----
    // 为什么需要它: 力反馈原始值(IndRun 侧 clamp 到 ±127)的实际取值范围
    // 随车/赛道/工况变化极大 —— 实测某次计时赛 torque 只落在 12..18,
    // 而满量程是 127。固定 in_max 要么把小信号压成平线, 要么在大信号上削顶。
    //
    // 规律: 输入幅值直接就是物理力的大小, 所以需要保留线性、随工况平移;
    // 开方/平方那些曲线反而会扭曲力度层次。自动增益把当前的"峰值"当作
    // 动态 in_max, 于是幅值归一化后总是铺满 0..1。
    //
    // 包络用快起慢落(attack / release), 只影响标定不影响手感延迟:
    // 峰值上去立刻跟上, 下来时慢慢回, 避免松手瞬间增益暴涨。
    bool        auto_gain           = true;
    double      gain_attack_sec     = 0.7;   // 峰值上升时, 增益跟到峰值所需的时间
    double      gain_release_sec    = 8.0;   // 峰值回落时, 增益衰减的时间常数
    int32_t     gain_floor          = 24;    // 动态 in_max 的下限
    int32_t     gain_ceiling        = 127;   // 动态 in_max 的上限(不得超过物理满量程)
    double      gain_idle_reset_sec = 3.0;   // 无信号多久后放弃当前峰值重新起步

    // ---- [safety] ----
    // ★ 用户要的"最大强度"接口 ★
    // 写进 BF 命令的通道软限位(0..200), 设备侧持久生效, 且每次重连后必须重发。
    int         max_strength        = 40;
    // 软件侧再压一道(0..200): 实际写入 B0 的强度不会超过它
    int         clamp_strength      = 40;
    // 每秒允许的强度变化量(0 = 不限制)。避免阶跃冲击。
    double      slew_per_second     = 40.0;
    // ★ 目前**没有实现**, 配了不会生效 —— 保留键位是为了兼容既有 INI。
    // 原意是"力反馈静默超过这个毫秒数就把输出拉到 0", 实际没有任何代码读它。
    // 现有的静默保护只到"超过 20 秒没有任何回调就打一条 WARN 日志"为止
    // (见 src/dllmain.cpp 的心跳分支), 那是一句告警, 不是归零动作。
    // 真要做需要: 在 worker loop 里记录 last_callback_at, 超时后强制
    // clamp_strength(0) 并 skip 后续 submit —— 尚未实现。
    int         watchdog_ms         = 500;
    // servo off / 未就绪时是否强制归零
    bool        zero_when_servo_off = true;
    // 启动后前 N 毫秒强制输出 0, 让用户有时间确认连接
    int         warmup_ms           = 1500;
    // 全局总开关: 设为 false 时只记日志不发数据(干跑模式)
    // ★ 默认 true ★: INI 里漏写这个键就等于允许真实输出, 与本项目"默认安全"
    // 的设计意图相反 —— 一个字的遗漏不该产生电刺激。要真实输出必须在 INI 里
    // 显式写 dry_run=0。缺键时 load_config 会打一条 WARN 提醒。
    bool        dry_run             = true;

    // ---- [ranges] ----
    // 上面 ChannelMapping 里的 in_min/in_max 覆盖了标定; 这里额外给
    // 扭矩方向的符号约定留个开关。
    bool        torque_negative_is_positive = false;

    // ---- [debug] ----
    // 关掉 IndRun.dll 挂钩, 只留日志/管线 —— 出问题时用来做 A/B 对照:
    //   hook=0 还能崩 -> 问题不在挂钩;  hook=0 就不崩 -> 问题在挂钩。
    bool        hook                = true;
};

// 从 INI 读; 文件不存在时返回默认值并写日志
Config load_config(const std::string& ini_path);

// 写一份带全部注释的默认 INI(首次运行时铺出来)
bool write_default_config(const std::string& ini_path);

// 解析耗时用: 把相对路径按 exe 目录展开
std::string resolve_config_path();

// 给日志用
const char* to_string(LinkMode m);
const char* to_string(ProtocolVersion v);
const char* to_string(MapMode m);

}  // namespace dglab_idac
