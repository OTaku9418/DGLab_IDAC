// ---------------------------------------------------------------------------
// DGLab_IDAC — UE4SS C++ mod 入口
//
// 结构:
//   UE4SS 加载 <mod>/dlls/main.dll -> 调 start_mod() -> on_program_start()
//     -> 读配置 / 起日志 / 挂 IndRun.dll 的力反馈 invoker / 建传输 / 起管线线程
//
// ── 为什么在游戏进程内挂钩, 而不是挂 AMDaemon 的 idachook.dll ──────────────
//   * IndRun.dll 是**力反馈的语义源头**(torque / viscosity / vibration /
//     servo), 而且它跟 UE4SS 在同一个进程里 —— 直接满足"接入 UE4SS 统一
//     hook 管理"的要求。
//   * idachook.dll 只存在于 AMDaemon 目录, AMDaemon 不是 UE4SS 的宿主进程。
//     挂它就得为 AMDaemon 单独做一次注入, 脱离 UE4SS 体系。
//   * 反过来, 游戏进程里**没有** idachook.dll, 而且 EXE 里
//     `idac`/`IDAC`/`idachook` 字符串出现 0 次 —— 两条路没有符号级联系。
//
// ── 关于 ABI 的硬约束(踩了会崩) ───────────────────────────────────────────
//   UE4SS 的官方文档原话: C++ mod 与 UE4SS 必须使用**同一个 C 运行时版本和
//   构建配置**。所以:
//     * 必须 Release 构建(UE4SS.dll 是 Game__Shipping__Win64 (MSVC))
//     * 必须 /MD(它导入 msvcp140.dll / vcruntime140.dll)
//     * CppUserModBase 的 vtable 布局必须完全一致 —— 所以我们锁死 v3.0.1
//   下面的 static_assert 与运行期自检就是为了让"配错了"变成可见的失败,
//   而不是随机崩溃。
// ---------------------------------------------------------------------------
#include <Mod/CppUserModBase.hpp>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "config.hpp"
#include "logger.hpp"
#include "pipeline.hpp"
#include "source/indrun_hook_source.hpp"
#include "transport/transport.hpp"

namespace dglab_idac {
namespace {

// ---------------------------------------------------------------------------
// 阶段 1 (探针)还是阶段 2 (真输出)
//
// 现在只做阶段 1: 确认 mode=xinput 下游戏到底有没有发力反馈、值域多大、
// 更新频率多高。这决定了后面所有映射参数, 而且不需要郊狼在场。
// 默认配置里 [link] mode=disabled + [safety] dry_run=1 就是干跑。
// ---------------------------------------------------------------------------
constexpr double kHeartbeatSeconds = 1.0;
constexpr double kProbeDetailSeconds = 10.0;   // 前 10 秒每次都打原始值

double now_seconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

}  // namespace
}  // namespace dglab_idac

// ===========================================================================
// UE4SS mod 类
//
// 注意: 必须定义在 namespace dglab_idac 里, 才能非限定地看到上面匿名
// namespace 里的 kHeartbeatSeconds / kProbeDetailSeconds / now_seconds。
// 类里不会通过名字引用它, 所以加不加命名空间都不影响基类 vtable 布局。
// ===========================================================================
namespace dglab_idac {

class DGLabIdacMod final : public RC::CppUserModBase {
public:
    DGLabIdacMod() {
        ModName = STR("DGLab_IDAC");
        ModVersion = STR("0.1.0");
        ModDescription = STR("Initial D The Arcade 力反馈 -> 郊狼(DG-LAB) 波形输出");
        ModAuthors = STR("DGLab_IDAC");
        ModIntendedSDKVersion = STR("3.0.1");

        // 注意: 这里**不能**读配置 —— UE4SS 在 start_mod() 之后才注册 mod,
        // 此时 DllMain 已经返回。所有初始化放 on_program_start。
    }

    ~DGLabIdacMod() override {
        shutdown();
        dglab_idac::log_info("mod 已卸载");
        dglab_idac::log_shutdown();
    }

    auto on_program_start() -> void override {
        // ---- 日志先起来, 后面所有失败都要能看到 ----
        // 路径不写死: UE4SS 的工作目录随启动方式变, 所以按 DLL 位置推导。
        const std::string ini = dglab_idac::resolve_config_path();
        dglab_idac::log_init();
        dglab_idac::log_info("==== DGLab_IDAC %s 启动 ====", "0.1.0");
        dglab_idac::log_info("日志文件: %s", dglab_idac::log_path());
        dglab_idac::log_info("配置文件: %s", ini.c_str());

        // ---- 配置 ----
        if (ini.empty()) {
            dglab_idac::log_warn("无法推导配置路径, 全部使用内置默认值");
        } else if (::GetFileAttributesA(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
            // 首次运行: 铺一份能跑的默认配置, 然后照默认值跑
            dglab_idac::write_default_config(ini);
        }
        cfg_ = dglab_idac::load_config(ini);
        dglab_idac::log_info("配置: enable=%d verbose=%d link=%s protocol=%s dry_run=%d",
                             cfg_.enabled ? 1 : 0, cfg_.verbose ? 1 : 0,
                             dglab_idac::to_string(cfg_.link),
                             dglab_idac::to_string(cfg_.protocol),
                             cfg_.dry_run ? 1 : 0);
        dglab_idac::log_info("安全: max_strength=%d clamp_strength=%d slew=%.1f/s "
                             "watchdog=%dms warmup=%dms",
                             cfg_.max_strength, cfg_.clamp_strength, cfg_.slew_per_second,
                             cfg_.watchdog_ms, cfg_.warmup_ms);
        dglab_idac::log_info("通道A: source=%s %s in=[%d,%d] level=%.2f base=%d",
                             cfg_.channel_a.source.c_str(),
                             dglab_idac::to_string(cfg_.channel_a.mode),
                             cfg_.channel_a.in_min, cfg_.channel_a.in_max,
                             cfg_.channel_a.level, cfg_.channel_a.base_strength);
        dglab_idac::log_info("通道B: source=%s %s in=[%d,%d] level=%.2f base=%d",
                             cfg_.channel_b.source.c_str(),
                             dglab_idac::to_string(cfg_.channel_b.mode),
                             cfg_.channel_b.in_min, cfg_.channel_b.in_max,
                             cfg_.channel_b.level, cfg_.channel_b.base_strength);

        if (!cfg_.enabled) {
            dglab_idac::log_warn("[general] enable=0 —— 不挂任何 hook, mod 空转");
            return;
        }

        // ---- 力反馈源: 挂 IndRun.dll ----
        source_ = std::make_unique<dglab_idac::IndRunHookSource>();
        if (!source_->install()) {
            // 挂不上不代表世界末日 —— IndRun.dll 可能还没被 UE4 加载。
            // 交给线程里重试, 而不是在这里失败退出(退出就没人再试了)。
            dglab_idac::log_warn("首次挂钩失败, 后台线程会继续重试");
        } else {
            dglab_idac::log_info("力反馈源就绪: %s", source_->status_report().c_str());
        }

        // ---- 传输 ----
        transport_ = dglab_idac::create_transport(cfg_);
        if (transport_ && !transport_->start()) {
            dglab_idac::log_error("传输启动失败, 降级到空输出");
            dglab_idac::destroy_transport(transport_);
            transport_ = nullptr;
        }

        // ---- 管线 ----
        pipeline_ = std::make_unique<dglab_idac::Pipeline>(cfg_);

        running_ = true;
        worker_ = std::thread([this] { thread_main(); });

        dglab_idac::log_info("已启动。输出=干跑(%s) 探头日志=%s",
                             transport_ ? transport_->name() : "none",
                             cfg_.dry_run ? "on" : "off");
        dglab_idac::log_info("提示: 进游戏后方向盘一动, 日志里就该出现 torque/vibration 计数增长");
    }

    auto on_unreal_init() -> void override {
        dglab_idac::log_info("on_unreal_init: UE4 引擎已就绪");
    }

    auto on_update() -> void override {
        // 每次 tick 都被调用, 频率很高。除了记账什么都不做。
        ++game_ticks_;
    }

private:
    // -----------------------------------------------------------------------
    // 管线线程
    //
    // 周期 100 ms(V3 的 B0 节拍)。独立线程的原因: 游戏的 tick 会被加载
    // 卡顿拖住, 而郊狼要求稳定节拍; 由独立线程按"最新一帧"发送, 丢拍时
    // 重发上一帧, 而不是补发过期帧。
    // -----------------------------------------------------------------------
    void thread_main() {
        using clock = std::chrono::steady_clock;

        // 给 IndRun.dll 一点加载时间 —— 它是 UE4 插件, 可能晚于本 mod 加载。
        for (int i = 0; i < 100 && running_; ++i) {
            if (source_ && source_->installed()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (source_ && !source_->installed()) {
                if (source_->install()) {
                    dglab_idac::log_info("延迟挂钩成功: %s", source_->status_report().c_str());
                }
            }
        }

        const auto period = std::chrono::milliseconds(
            cfg_.frame_ms > 0 ? cfg_.frame_ms : 100);

        auto next = clock::now();
        double last_heartbeat = 0.0;
        double start_time = now_seconds();

        uint64_t frames = 0;
        uint64_t prev_torque = 0, prev_visc = 0, prev_vib = 0;

        while (running_) {
            next += period;

            const double t = now_seconds();
            dglab_idac::Frame frame;
            bool have_frame = false;

            if (source_) {
                const dglab_idac::FfbState ffb = source_->snapshot();
                have_frame = pipeline_->tick(t, ffb, frame);

                // ---- 探针日志: 前 10 秒打原始值, 之后只在变化时打 ----
                const bool changed = ffb.torque_calls != prev_torque ||
                                     ffb.viscosity_calls != prev_visc ||
                                     ffb.vibration_calls != prev_vib;
                const bool early = (t - start_time) < kProbeDetailSeconds;

                if ((changed || early) && ffb.ever_updated) {
                    dglab_idac::log_info(
                        "[probe] t=%.2f torque(d=%d,m=%d,n=%llu) visc(a=%d,b=%d,n=%llu) "
                        "vib(a=%d,b=%d,n=%llu) servo=%d",
                        t - start_time,
                        ffb.torque_dir, ffb.torque_magnitude,
                        static_cast<unsigned long long>(ffb.torque_calls),
                        ffb.viscosity_a, ffb.viscosity_b,
                        static_cast<unsigned long long>(ffb.viscosity_calls),
                        ffb.vibration_a, ffb.vibration_b,
                        static_cast<unsigned long long>(ffb.vibration_calls),
                        ffb.servo_on ? 1 : 0);
                }
                prev_torque = ffb.torque_calls;
                prev_visc = ffb.viscosity_calls;
                prev_vib = ffb.vibration_calls;

                // ---- 周期心跳: 这是判断"游戏到底发不发 FFB"的关键证据 ----
                if (t - last_heartbeat >= kHeartbeatSeconds) {
                    last_heartbeat = t;
                    const dglab_idac::FfbState s = source_->snapshot();
                    dglab_idac::log_info(
                        "[hb] t=%.0fs frames=%llu | torque=%llu mag[%d..%d] dir[%d..%d] | "
                        "visc=%llu b[%d..%d] | vib=%llu b[%d..%d] | servo on/off=%llu/%llu=%d | %s",
                        t - start_time,
                        static_cast<unsigned long long>(frames),
                        static_cast<unsigned long long>(s.torque_calls),
                        s.torque_mag_min == INT32_MAX ? -1 : s.torque_mag_min,
                        s.torque_mag_max == INT32_MIN ? -1 : s.torque_mag_max,
                        s.torque_dir_min == INT32_MAX ? -1 : s.torque_dir_min,
                        s.torque_dir_max == INT32_MIN ? -1 : s.torque_dir_max,
                        static_cast<unsigned long long>(s.viscosity_calls),
                        s.viscosity_b_min == INT32_MAX ? -1 : s.viscosity_b_min,
                        s.viscosity_b_max == INT32_MIN ? -1 : s.viscosity_b_max,
                        static_cast<unsigned long long>(s.vibration_calls),
                        s.vibration_b_min == INT32_MAX ? -1 : s.vibration_b_min,
                        s.vibration_b_max == INT32_MIN ? -1 : s.vibration_b_max,
                        static_cast<unsigned long long>(s.servo_on_calls),
                        static_cast<unsigned long long>(s.servo_off_calls),
                        s.servo_on ? 1 : 0,
                        source_->status_report().c_str());

                    // 明确告警: 挂了 hook 但一次回调都没有 —— 说明要么还没进
                    // 游戏, 要么力反馈通路在 mode=xinput 下根本不走。
                    if (!s.ever_updated && (t - start_time) > 20.0) {
                        dglab_idac::log_warn(
                            "[hb] 已经 %.0f 秒没有任何力反馈回调。可能原因: "
                            "(a) 还没进游戏; (b) 游戏在 xinput 模式下不生成力反馈; "
                            "(c) 增益 [manager+0x4E4] 恒为 0(见 ffb_types.hpp 的说明)。",
                            t - start_time);
                    }
                }
            }

            if (have_frame && transport_) {
                transport_->submit(frame);
            }
            ++frames;

            // 按绝对时间点对齐, 避免累计漂移; 落后太多就跳拍(绝不用 sleep 追赶)
            const auto now = clock::now();
            if (next > now) {
                std::this_thread::sleep_until(next);
            } else {
                // 落后了。重发当前帧的节拍(不补发), 并把基准拉回现在。
                next = now;
            }
        }

        dglab_idac::log_info("管线线程退出, 共 %llu 帧",
                             static_cast<unsigned long long>(frames));
    }

    void shutdown() {
        if (!running_.exchange(false)) return;
        if (worker_.joinable()) worker_.join();
        if (source_) {
            source_->uninstall();
            source_.reset();
        }
        dglab_idac::destroy_transport(transport_);
        transport_ = nullptr;
        pipeline_.reset();
    }

    dglab_idac::Config cfg_{};
    std::unique_ptr<dglab_idac::IndRunHookSource> source_;
    std::unique_ptr<dglab_idac::Pipeline> pipeline_;
    dglab_idac::ITransport* transport_ = nullptr;

    std::atomic<bool> running_{false};
    std::thread worker_;
    std::atomic<uint64_t> game_ticks_{0};
};

}  // namespace dglab_idac

// ===========================================================================
// UE4SS C++ mod ABI —— 必须 extern "C" + dllexport
// ===========================================================================
#define DGLAB_MOD_API __declspec(dllexport)

extern "C" {

DGLAB_MOD_API RC::CppUserModBase* start_mod() {
    return new dglab_idac::DGLabIdacMod();
}

DGLAB_MOD_API void uninstall_mod(RC::CppUserModBase* mod) {
    delete mod;
}

// ---------------------------------------------------------------------------
// 额外的诊断导出 —— 没有它们就只能翻日志文件, 有的话可以在调试器里
// 直接看 mod 状态。UE4SS 不要求这些, 纯属方便。
// ---------------------------------------------------------------------------
DGLAB_MOD_API const char* dglab_status_string() {
    static thread_local char buf[512];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "DGLab_IDAC 0.1.0 (probe stage) log=%s", dglab_idac::log_path());
    return buf;
}

}  // extern "C"
