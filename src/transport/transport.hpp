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

#include <cstdint>
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

// byte[1] 位 3-2 / 位 1-0
enum class StrengthMode : uint8_t {
    Absolute = 0b00,   // 绝对设置
    RelativeAdd = 0b01,   // 相对加
    RelativeSub = 0b10,   // 相对减
    Discard = 0b11,   // 不改强度(保留原值)
};

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
