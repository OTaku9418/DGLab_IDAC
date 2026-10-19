// ---------------------------------------------------------------------------
// DGLab_IDAC — 传输层抽象
//
// 管线只认这个接口, 不关心底下是 BLE 还是别的什么。
//
// 分工很重要:
//   * 管线(游戏线程 / 工作线程)只负责算波形, 调用 submit()
//   * 传输层自己起工作线程, 按 frame_ms 节拍把最近一帧发出去
// 这样游戏的卡顿不会变成刺激的卡顿 —— 传输线程永远发"最新"的帧,
// 而不是排队发"过时"的帧。队列深度刻意限制为 1。
// ---------------------------------------------------------------------------
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "config.hpp"
#include "pipeline.hpp"

namespace dglab_idac {

// ---------------------------------------------------------------------------
// 编好的 B0 命令(20 字节)。
//
// 布局(来自 docs/research-dglab-transport.md, 全部按 coyote/v3/README.md 核对):
//   [0]     0xB0
//   [1]     seq(高 4 位) | A模式(位3-2) | B模式(位1-0)
//   [2]     A 通道强度设定值 0..200
//   [3]     B 通道强度设定值 0..200
//   [4..7]  A 脉冲频率 ×4   (各 10..240, 每组 25 ms)
//   [8..11] A 脉冲强度 ×4   (各 0..100)
//   [12..15] B 脉冲频率 ×4
//   [16..19] B 脉冲强度 ×4
// 通道内 8 个值(4 频率 + 4 强度)**只要有一个越界, 该通道整帧被丢弃**。
// ---------------------------------------------------------------------------
inline constexpr int kB0Size = 20;
inline constexpr int kBfSize = 7;

// byte[1] 位 3-2 = A 通道模式, 位 1-0 = B 通道模式。
//
// ⚠⚠ 这四个值曾经**全部反了**, 2026-10-17 照官方文档
//    (dungeonlab-open/dglab-bluetooth-protocol, coyote/v3/README.md,
//     逐字节副本存于 re/reference/dglab-bluetooth-protocol/coyote-v3-README.md)
//    逐个核对后改正:
//
//       0b00  不变 —— **忽略本通道的强度字节**, 维持设备当前值
//       0b01  相对增加 (在当前值上加)
//       0b10  相对减少
//       0b11  绝对设置 (把通道设为给定值)
//
//    官方表格(起始 A=10): 0b0000→A 仍 10 / 0b0100→15 / 0b1000→0 / 0b1100→0。
//    可见"绝对设置"是 0b11 而**不是** 0b00, "不变"是 0b00 而不是 0b11 ——
//    改之前正好写成了相反的对应关系, 后果见下面 encode_b0 的注释。
enum class StrengthMode : uint8_t {
    Unchanged   = 0b00,   // 不改强度(忽略强度字节, 保留设备原值)
    RelativeAdd = 0b01,   // 相对加
    RelativeSub = 0b10,   // 相对减
    Absolute    = 0b11,   // 绝对设置(把通道设为给定值)
};

// 旧名字, 保留以免漏改的调用点变成编译错误之外的东西。
// ★ 新代码请用 Unchanged。下一轮清理时删掉它。
inline constexpr StrengthMode Discard = StrengthMode::Unchanged;

struct B0 {
    uint8_t bytes[kB0Size]{};

    void set_seq(uint8_t seq4) { bytes[1] = static_cast<uint8_t>((bytes[1] & 0x0F) | ((seq4 & 0x0F) << 4)); }
    void set_mode_a(StrengthMode m) {
        bytes[1] = static_cast<uint8_t>((bytes[1] & ~0x0C) | ((static_cast<uint8_t>(m) & 0x03) << 2));
    }
    void set_mode_b(StrengthMode m) {
        bytes[1] = static_cast<uint8_t>((bytes[1] & ~0x03) | (static_cast<uint8_t>(m) & 0x03));
    }
    uint8_t seq() const { return static_cast<uint8_t>((bytes[1] >> 4) & 0x0F); }

    std::string to_hex() const;
};

// 从 Frame 编码出 B0。
//
// strength_mode_a/b 由调用方(传输层)决定, 因为"要不要改强度、用绝对还是相对"
// 取决于序列号记账, 那属于传输层的状态机。
B0 encode_b0(const Frame& f, StrengthMode mode_a, StrengthMode mode_b, uint8_t seq4, bool* out_a_valid,
             bool* out_b_valid);

// BF 软限位命令(7 字节)。设备侧持久生效, 每次重连后必须重发。
// 这是最后一道安全阀 —— B0 里的强度无法突破它。
B0 make_bf(int limit_a, int limit_b, int freq_bal_a = 128, int freq_bal_b = 128);

// 频率换算: 赫兹 -> 协议里的"输出单元时长"值(10..240)。
// 官方公式: transmitted = round(10 + input*(240-10)/(1000-10)), 输入域 10..1000。
// ⚠ 线上值越小频率越高 —— 典型的符号反向 bug 来源。
// 本函数输入直接是赫兹, 内部先转成 ms 周期的等价量再套公式。
uint8_t hz_to_pulse_value(double hz);

// ---------------------------------------------------------------------------
// 帧信箱 —— 管线线程 put(), 传输线程 take()。深度 1, 覆盖式。
//
// 为什么抽出来做成独立的纯 C++ 类:
//   * 它承载 [safety] watchdog_ms 的实现, 是安全逻辑, 必须能**离线测试**
//     (时间由调用方传进来, 不依赖真实时钟)。
//   * 以后的 Socket 传输要复用同一套语义, 不能各写一遍。
//
// 看门狗只判断"管线 → 传输"这条数据流有没有断, **不**按 FFB 回调间隔判断:
// 力反馈回调是边沿触发的(值不变就不回调), 方向盘稳定受力时"很久没回调"
// 完全正常, 按它做看门狗会误静音。
//
// ★ 覆盖边界 (实测, 见 README 5.3 节) ★
// 它能兜住的是"发送线程还在跑、但管线不再出帧"(管线卡住/崩溃/被饿死)。它兜不住
// 整个进程一起冻结: 那时本类的 take() 自己也不会被调用, 设备保持最后一帧的强度。
// 421 s 的那局里两次 10.9 s / 14.0 s 的卡顿, trips 全程为 0 就是这个原因。
// ---------------------------------------------------------------------------
struct MailboxTake {
    Frame frame;
    bool  fresh = false;   // 上次 take 之后有没有新帧
    bool  stale = false;   // 看门狗已触发(此时 frame 是静默帧)
};

struct MailboxStats {
    uint64_t submitted      = 0;
    uint64_t overwritten    = 0;   // 传输层就绪时, 上一帧还没被取走就被新帧覆盖
    // 传输层**还没就绪**时发生的覆盖(典型: 连接建立的 ~3 s 里 BLE 线程不取帧,
    // 管线照样 10 Hz 提交)。和 overwritten 分开计, 否则那 3 秒的 26 次覆盖会
    // 把所有局的 over/submitted 比例都污染成"传输跟不上"(T13c)。
    uint64_t overwritten_pre = 0;
    uint64_t taken_fresh    = 0;
    uint64_t repeated       = 0;   // 重复发送了旧帧
    uint64_t idle_sent      = 0;   // 发出的静默帧数
    uint64_t watchdog_trips = 0;
};

class FrameMailbox {
public:
    void        put(const Frame& f, int64_t now_ms);
    MailboxTake take(int64_t now_ms, int watchdog_ms);
    MailboxStats stats() const;

    // "消费端就绪"标志: 传输线程真正开始逐帧取帧时置 true, 链路断了置 false。
    // 只影响覆盖计数归到哪一栏, 不影响看门狗判定。
    void set_consumer_ready(bool ready);
    bool consumer_ready() const;

private:
    mutable std::mutex mu_;
    Frame    pending_{};
    bool     has_pending_   = false;
    int64_t  last_put_ms_   = 0;
    uint64_t put_count_     = 0;   // put 的总次数
    uint64_t taken_count_   = 0;   // 已经被 take 走的 put 次数
    bool     tripped_       = false;
    std::atomic<bool> consumer_ready_{false};
    MailboxStats stats_{};
};

// steady_clock 的毫秒数。看门狗用单调时钟, 不受系统时间调整影响。
int64_t steady_now_ms();

// 静默帧: 强度 0、4 个槽的脉冲强度 0、频率 10、muted=true。
// reason 必须是字符串字面量 —— 它会存进 Frame::mute_reason, 那个字段要求
// 静态存储期(见 pipeline.hpp)。
Frame make_idle_frame(const char* reason);

// ---------------------------------------------------------------------------
// 传输接口
// ---------------------------------------------------------------------------
class ITransport {
public:
    virtual ~ITransport() = default;

    virtual const char* name() const = 0;

    // 起工作线程 / 建连接。线程内部自己重连, 返回 false 只表示无法启动。
    virtual bool start() = 0;
    virtual void stop() = 0;

    // 提交一帧。线程安全, 覆盖式(只保留最新一帧)。
    virtual void submit(const Frame& f) = 0;

    // 诊断用
    virtual std::string status() const = 0;
};

// 工厂。cfg.link 决定返回哪个实现; 依赖不可用时返回 nullptr 并写日志。
ITransport* create_transport(const Config& cfg);
void destroy_transport(ITransport* t);

// 各实现自己的构造入口(供 create_transport 分派)
ITransport* create_null_transport(const Config& cfg);
ITransport* create_ble_transport(const Config& cfg);

}  // namespace dglab_idac
