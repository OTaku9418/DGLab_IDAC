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
    // ★ 这是"PC 做服务端"旧设计的遗留键, 当前没有任何代码消费它们 ★
    // (解析留着只为不破坏老 INI 的手感)。真正的 Socket 方案是官方
    // dglab-websocket-server 的 V4 中继: 第三方控制器(即本程序)连到中继
    // 服务器, 手机 APP 再用 tid 连过来 —— 方向与这里的 "本机 bind/port"
    // 相反, 而且服务端没有 BLE, 手机仍然是必经环节。
    // 详见 docs/research-dglab-transport.md 第 1.2 节。
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
    // BLE 模式下这个值由 effective_frame_ms() 固定为 100(见下面那个函数)。
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
    // 看门狗: 管线侧超过这么多毫秒没有提交新帧, 传输层就改发静默帧(强度 0)。
    //
    // ★ 判据是"管线有没有在提交帧", 不是"力反馈回调有没有来" ★
    // 力反馈回调是**边沿触发**的(IndRun 内部有变化缓存, 值不变就不回调),
    // 所以方向盘稳定受力时"几秒没有回调"完全正常 —— 拿回调间隔做看门狗
    // 会在游戏正常运行时把输出静音。
    //
    // 实现分两层, 缺一不可:
    //   * src/transport/transport.cpp 的 FrameMailbox::take() —— 传输线程侧,
    //     逐帧判断, 超时就换静默帧(这是真正"拉回 0"的那一层)。
    //   * src/pipeline.cpp 的 tick() —— 管线侧, 自己停顿超过这个值就清零强度
    //     累积并重新预热, 避免停顿结束后一帧跳到满力。
    // 取值由 effective_watchdog_ms() 折算: 抬到至少 3*frame_ms, 压到最多 2000。
    int         watchdog_ms         = 500;
    // servo off / 未就绪时是否强制归零
    bool        zero_when_servo_off = true;
    // T17: viscosity 源的"保持超时"(毫秒)。
    // viscosity 的回调是"值变了才回调", 退出比赛回菜单后游戏会把阻尼停在一个
    // 非零值上不再更新 —— 那个值会被当成当前状态一直输出(实测停住的 visc_b=10
    // 对应脉冲 42/100, 指示灯亮着)。超过这个毫秒数没有新回调就把该通道的原始值
    // 按 0 处理, 源一更新立刻恢复。
    //   0 = 关闭; 其他值钳到 500..10000(低于 500 会把正常行驶中的短暂间隙误判,
    //   实测 viscosity 在驾驶时间隔几乎全 <0.5 s)。
    // 只影响 source=viscosity / viscosity_a 的通道, 另一通道不受影响。
    int         viscosity_hold_ms   = 2000;
    // T22b: torque / vibration 源的保持超时(毫秒)。0 = 关闭。
    //
    // ★ 默认 10000, 不是 2000 ★
    // 2026-10-18 那局(100 分钟、5,667 条心跳)里, "方向盘稳住不动"会让 torque
    // 的回调停 3~7 秒(实测最长 7.0 s), 期间粘度仍在每秒更新 —— 说明车在动、
    // 这次保持是真实的。2 s 的超时会在这种真实过弯里把力砍掉(约每 1.4 分钟
    // 一次), 所以把单源超时放到 10 s, 只用来兜"源彻底不更新"的底。
    int         torque_hold_ms      = 10000;
    int         vibration_hold_ms   = 10000;
    // T22b 一级: 三个源"同时"停更超过这么多毫秒, 就把所有已启用通道的原始值
    // 按 0 处理。0 = 关闭。
    //
    // 为什么需要它: *_hold_ms 是"单个源"的兜底, 对"游戏不再喂任何力反馈"这类
    // 场景反应太慢。而"三源齐停"是个干净的信号 —— 同一局里它只出现 22 次,
    // 最短 48 s, 正常驾驶中一次都没有。用 1 s 就能把"匹配成功打断计时赛"这类
    // 场景在一秒内压到 0, 且不会误伤任何真实保持。
    int         silent_mute_ms      = 1000;
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

// 把 INI 里写的 frame_ms 折算成"实际生效值"(纯函数, 但会打 WARN)。
//
// 只有 BLE 直连有约束: V3 的 B0 一帧覆盖 4×25 ms = 100 ms, 设备也按 100 ms 的
// 窗口回 B1, 把节拍配成别的值不会让设备变快或变慢, 只会让管线提交节拍与设备
// 消费节拍错位(信箱覆盖率、看门狗阈值都跟着算错)。所以 BLE 模式下固定 100 ms,
// 配成别的值打 WARN 并忽略。空输出/WebSocket 没有这个约束, 原样返回。
int effective_frame_ms(int requested_ms, LinkMode link);

// 把 INI 里写的 viscosity_hold_ms 折算成"实际生效值"(纯函数, 但会打 WARN)。
//
// 语义: 0 = 关闭保持超时; 非 0 时钳到 [500, 10000]。下限 500 ms 的理由是实测
// viscosity 在正常驾驶时间隔几乎全 <0.5 s, 再小会把正常抖动当成"停更";
// 上限 10 s 的理由是超过它以后残值已经被感知到很久了, 等于没做保护。
int effective_viscosity_hold_ms(int requested_ms);

// T22b: 单源保持超时的折算(纯函数, 会打 WARN)。
//
// 0 = 关闭; 其他值钳到 [500, hi_ms]。下限 500 ms 的理由与 viscosity 那条相同;
// 上限由调用方给(torque/vibration 20000, viscosity 10000), 因为这两类源的
// 正常保持长度差了一个数量级(实测 torque 最长 7.0 s, viscosity 几乎全 <0.5 s)。
int effective_source_hold_ms(int requested_ms, const char* key_name, int hi_ms);

// T22b 一级: 全源静默阈值的折算(纯函数, 会打 WARN)。
//
// 0 = 关闭; 其他值钳到 [200, 10000]。下限 200 ms 是"再小就会被自定义帧节拍
// 抖动误触发"的保守值(实测齐停最短 48 s, 离得极远); 上限 10 s 的理由是
// 超过它就等于退化成单源超时, 这一级存在与否没有区别。
int effective_silent_mute_ms(int requested_ms);

// 把 INI 里写的 watchdog_ms 折算成"实际生效值"(纯函数, 但会打 WARN)。
//
// 三条约束, 都与帧周期绑定:
//   * 下限 50 ms —— 再小没有意义;
//   * 下限 3*frame_ms —— 管线按 frame_ms 提交, Windows 默认定时器精度约
//     15.6 ms, 正常抖动会让帧间隔偶尔超过 frame_ms, 留两次迟到的余量;
//   * 上限 2000 ms —— 看门狗的意义是"数据流断了就归零"。配得比 2 s 还大,
//     等于容忍设备在卡死期间长时间保持最后的强度, 那就不是兜底了。
// 如果 frame_ms 大到 3*frame_ms 超过 2000, 以 3*frame_ms 为准(否则约束自相矛盾),
// 这条路径会打 WARN。0 不表示"关闭": 看门狗是安全阀, 不能靠一个数字失效。
int effective_watchdog_ms(int requested_ms, int frame_ms);

// 写一份带全部注释的默认 INI(首次运行时铺出来)
bool write_default_config(const std::string& ini_path);

// 解析耗时用: 把相对路径按 exe 目录展开
std::string resolve_config_path();

// 给日志用
const char* to_string(LinkMode m);
const char* to_string(ProtocolVersion v);
const char* to_string(MapMode m);

// 这个输入源有没有"停住的值会被一直输出"的风险(见 README「已知风险与使用须知」)。
//
// 目前只有 viscosity / viscosity_a: 它的回调是**值变了才回调**(IndRun 内部有
// 变化缓存), 而强制退出比赛回到菜单时, 游戏会把阻尼停在一个可感的值上却不再
// 回调 —— 本程序没有"源数据陈旧"判定, 于是那个值会被管线一直输出, 直到下一次
// 变化。torque/vibration 在回菜单时会被游戏压到接近 0, 所以不在名单里。
//
// 纯函数, 供 load_config 告警与离线自检共用。
bool is_held_risk_source(const std::string& src);

}  // namespace dglab_idac
