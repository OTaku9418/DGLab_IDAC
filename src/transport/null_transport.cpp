// ---------------------------------------------------------------------------
// DGLab_IDAC — 空传输(干跑)
//
// 用途:
//   1. 探针阶段 —— 我们要的是"游戏到底发不发力反馈、值域多大", 完全不需要
//      真实输出。默认配置就是 disabled + dry_run。
//   2. BLE 不可用时的降级 —— 至少让管线跑起来, 日志里能看到波形。
// ---------------------------------------------------------------------------
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

#include "logger.hpp"
#include "transport/transport.hpp"

namespace dglab_idac {

namespace {

class NullTransport final : public ITransport {
public:
    explicit NullTransport(const Config& cfg) : cfg_(cfg) {}

    const char* name() const override { return "NullTransport"; }

    bool start() override {
        running_ = true;
        log_warn("使用空传输(干跑): 波形只会写进日志, 不会送到任何设备。"
                 "要真实输出请把 [link] mode 改成 ble 并确认郊狼已开机。");
        return true;
    }

    void stop() override { running_ = false; }

    void submit(const Frame& f) override {
        if (!running_) return;

        std::lock_guard<std::mutex> lk(mtx_);
        ++frames_;

        // 每帧都打会把日志刷爆(10 行/秒)。默认按 1 Hz 打一次摘要,
        // 变化时立刻打 —— 探针阶段要看的正是"变化"。
        const bool changed =
            f.a.strength != last_a_ || f.b.strength != last_b_ || f.muted != last_muted_ ||
            f.a.slots[0].pulse_intensity != last_ai_ || f.b.slots[0].pulse_intensity != last_bi_;

        const bool due = (frames_ % 10) == 0;

        if (changed || (cfg_.verbose && due)) {
            last_a_ = f.a.strength;
            last_b_ = f.b.strength;
            last_ai_ = f.a.slots[0].pulse_intensity;
            last_bi_ = f.b.slots[0].pulse_intensity;
            last_muted_ = f.muted;

            // 编码出真实字节也打出来, 这样日志里可以直接核对协议正确性
            bool va = false, vb = false;
            const B0 cmd = encode_b0(f, StrengthMode::Absolute, StrengthMode::Absolute, 0, &va, &vb);

            const std::string hex = cmd.to_hex();
            log_info("frame#%llu %s A(str=%d,i=%u,f=%u%s) B(str=%d,i=%u,f=%u%s) B0=%s",
                     static_cast<unsigned long long>(frames_),
                     f.muted ? "MUTED" : "     ",
                     f.a.strength,
                     static_cast<unsigned>(f.a.slots[0].pulse_intensity),
                     static_cast<unsigned>(f.a.slots[0].pulse_frequency),
                     va ? "" : "!INVALID",
                     f.b.strength,
                     static_cast<unsigned>(f.b.slots[0].pulse_intensity),
                     static_cast<unsigned>(f.b.slots[0].pulse_frequency),
                     vb ? "" : "!INVALID",
                     hex.c_str());
        }
    }

    std::string status() const override {
        char buf[160];
        // 注意 frames_ 是普通 uint64_t(受 mtx_ 保护), 不是 atomic —— 所以
        // 这里要锁一下再读, 不能直接 .load()。
        uint64_t n = 0;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            n = frames_;
        }
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "NullTransport frames=%llu (dry run)",
                    static_cast<unsigned long long>(n));
        return buf;
    }

private:
    Config cfg_;
    std::atomic<bool> running_{false};
    mutable std::mutex mtx_;
    uint64_t frames_ = 0;

    int  last_a_ = -1;
    int  last_b_ = -1;
    int  last_ai_ = -1;
    int  last_bi_ = -1;
    bool last_muted_ = false;
};

}  // namespace

ITransport* create_null_transport(const Config& cfg) { return new NullTransport(cfg); }

}  // namespace dglab_idac
