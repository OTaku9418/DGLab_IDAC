// ---------------------------------------------------------------------------
// DGLab_IDAC — IndRun.dll 力反馈 hook 源实现
// ---------------------------------------------------------------------------
#include "source/indrun_hook_source.hpp"

#include <MinHook.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "bisect_logger.hpp"
#include "logger.hpp"

namespace dglab_idac {

namespace {


// steady_clock 秒。只用于超时判定, 精度 ms 级足够。
inline double now_seconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

// 本文件内部统一用这个转发到 dglab_idac::log_line（logger.hpp）。
// 注意**不要**起名叫 log_line —— 那会和外层的 dglab_idac::log_line 撞成
// 重载二义性（C2668），因为本文件所有调用点都在 dglab_idac 里。
//
// 顺带记一笔: 这里以前是自己 fputs(stdout) 的, 那是 0xC0000005 的来源 ——
// 游戏是 GUI 子系统程序, 没有 stdio, `stdout` 是野指针。绝不要再碰它。
void ffb_log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    log_info("%s", buf);
}

// ---------------------------------------------------------------------------
// RVA 表 —— 特定构建。TimeDateStamp 0x69392E86
// ---------------------------------------------------------------------------
constexpr uintptr_t kRvaInvokeTorque    = 0x21C0;
constexpr uintptr_t kRvaInvokeViscosity = 0x2200;
constexpr uintptr_t kRvaInvokeVibration = 0x2240;
constexpr uintptr_t kRvaInvokeServoOn   = 0x2160;
constexpr uintptr_t kRvaInvokeServoOff  = 0x21A0;

// 期望的 prologue 字节 —— 用来确认我们挂的是正确的构建。
//
// ⚠ 2026-10-16 修正: 这三个常量原先各少了开头一条 `sub rsp,N`, 导致
//   verify_signatures() 对 5 个目标里的 4 个恒返回 false —— 挂钩会被自己
//   的校验挡下, 永远拿不到力反馈数据(且不会崩, 所以很难发现)。
//
// 现在的值是用 re/disasm_indrun.py 反汇编实机 IndRun.dll 得到的入口字节:
//
//   0x2160 servo-on   sub rsp,0x28 ; mov rax,[rip+..]   = 48 83 EC 28 48 8B 05
//   0x21A0 servo-off  mov rax,[rip+..]                  = 48 8B 05      (无栈帧)
//   0x21C0 torque     sub rsp,0x38 ; mov eax,ecx        = 48 83 EC 38 8B C1
//   0x2200 viscosity  sub rsp,0x38 ; mov eax,ecx        = 48 83 EC 38 8B C1
//   0x2240 vibration  sub rsp,0x38 ; mov eax,ecx        = 48 83 EC 38 8B C1
//
// 前 6 字节里包含了 `sub rsp,0x38/0x28` 这个帧大小, 所以它同时校验了
// "是不是 2 参数版"(0x38) 和 "是不是无参版"(0x28) —— 比原来只看一条
// `mov` 更严格: 就算两个 invoker 的 RVA 被写反了, 这里也会报错。
//
// 另外记录一个事实: servo-on/servo-off 是通过 `add rsp,N` + `jmp [rax+0x10]`
// **尾跳**进 std::function 的, 不是 `call`。所以"invoker 内部是真实 call"
// 这句只对 torque/viscosity/vibration 成立。我们在 invoker 边界挂钩,
// 拿到的是寄存器里的原始参数, 与尾跳与否无关。
//
// ⚠ 2026-10-17 第二次修正: servo-on 和 servo-off **不是同一种序言**。
//   servo-on  有栈帧: sub rsp,0x28 ; mov rax,[rip+..]   -> 7 字节
//   servo-off 无栈帧: mov rax,[rip+..]                    -> 上一版给它套了
//              kSigVoid(7 字节, 含 sub rsp,0x28), 于是恒不匹配。
//   症状很好认: 只有 servo-off 一条报错, 其余 4 条全过。
//   现在两者各自一个常量, 长度也不同(7 vs 4), 不会再被当成一回事。
constexpr uint8_t kSigTwoInts[7] = {0x48, 0x83, 0xEC, 0x38, 0x8B, 0xC1, 0x48};
constexpr uint8_t kSigServoOn[7] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x05};
constexpr uint8_t kSigServoOff[4] = {0x48, 0x8B, 0x05, 0xE9};

}  // namespace

IndRunHookSource* IndRunHookSource::s_instance_ = nullptr;

IndRunHookSource::IndRunHookSource() {
    s_instance_ = this;
}

IndRunHookSource::~IndRunHookSource() {
    uninstall();
    s_instance_ = nullptr;
}

// ---------------------------------------------------------------------------
// detour
// ---------------------------------------------------------------------------
void __fastcall IndRunHookSource::detour_torque(int32_t direction, int32_t magnitude) {
    auto* self = s_instance_;
    if (self) {
        auto& s = self->shared_;
        s.torque_dir.store(direction, std::memory_order_relaxed);
        s.torque_mag.store(magnitude, std::memory_order_relaxed);
        s.torque_at.store(now_seconds(), std::memory_order_relaxed);
        s.n_torque.fetch_add(1, std::memory_order_relaxed);
        s.mag_min.store((std::min)(s.mag_min.load(std::memory_order_relaxed), magnitude),
                        std::memory_order_relaxed);
        s.mag_max.store((std::max)(s.mag_max.load(std::memory_order_relaxed), magnitude),
                        std::memory_order_relaxed);
        s.dir_min.store((std::min)(s.dir_min.load(std::memory_order_relaxed), direction),
                        std::memory_order_relaxed);
        s.dir_max.store((std::max)(s.dir_max.load(std::memory_order_relaxed), direction),
                        std::memory_order_relaxed);
        s.ever.store(true, std::memory_order_relaxed);
    }
    if (self && self->original_torque_) {
        self->original_torque_(direction, magnitude);
    }
}

void __fastcall IndRunHookSource::detour_viscosity(int32_t a, int32_t b) {
    auto* self = s_instance_;
    if (self) {
        auto& s = self->shared_;
        s.visc_a.store(a, std::memory_order_relaxed);
        s.visc_b.store(b, std::memory_order_relaxed);
        s.visc_at.store(now_seconds(), std::memory_order_relaxed);
        s.n_visc.fetch_add(1, std::memory_order_relaxed);
        s.visc_min.store((std::min)(s.visc_min.load(std::memory_order_relaxed), b),
                         std::memory_order_relaxed);
        s.visc_max.store((std::max)(s.visc_max.load(std::memory_order_relaxed), b),
                         std::memory_order_relaxed);
        s.ever.store(true, std::memory_order_relaxed);
    }
    if (self && self->original_viscosity_) {
        self->original_viscosity_(a, b);
    }
}

void __fastcall IndRunHookSource::detour_vibration(int32_t a, int32_t b) {
    auto* self = s_instance_;
    if (self) {
        auto& s = self->shared_;
        s.vib_a.store(a, std::memory_order_relaxed);
        s.vib_b.store(b, std::memory_order_relaxed);
        s.vib_at.store(now_seconds(), std::memory_order_relaxed);
        s.n_vib.fetch_add(1, std::memory_order_relaxed);
        s.vib_min.store((std::min)(s.vib_min.load(std::memory_order_relaxed), b),
                        std::memory_order_relaxed);
        s.vib_max.store((std::max)(s.vib_max.load(std::memory_order_relaxed), b),
                        std::memory_order_relaxed);
        s.ever.store(true, std::memory_order_relaxed);
    }
    if (self && self->original_vibration_) {
        self->original_vibration_(a, b);
    }
}

void __fastcall IndRunHookSource::detour_servo_on() {
    auto* self = s_instance_;
    if (self) {
        auto& s = self->shared_;
        if (!s.servo_on.exchange(true, std::memory_order_relaxed)) {
            s.servo_at.store(now_seconds(), std::memory_order_relaxed);
        }
        s.n_servo_on.fetch_add(1, std::memory_order_relaxed);
    }
    if (self && self->original_servo_on_) {
        self->original_servo_on_();
    }
}

void __fastcall IndRunHookSource::detour_servo_off() {
    auto* self = s_instance_;
    if (self) {
        auto& s = self->shared_;
        if (s.servo_on.exchange(false, std::memory_order_relaxed)) {
            s.servo_at.store(now_seconds(), std::memory_order_relaxed);
        }
        s.n_servo_off.fetch_add(1, std::memory_order_relaxed);
    }
    if (self && self->original_servo_off_) {
        self->original_servo_off_();
    }
}

// ---------------------------------------------------------------------------
// 安装
// ---------------------------------------------------------------------------
bool IndRunHookSource::wait_for_module(DWORD timeout_ms) {
    const DWORD step = 100;
    DWORD waited = 0;
    while (waited <= timeout_ms) {
        HMODULE m = ::GetModuleHandleW(L"IndRun.dll");
        if (m) {
            module_ = m;
            base_ = reinterpret_cast<uintptr_t>(m);
            ffb_log("找到 IndRun.dll @ %p", reinterpret_cast<void*>(base_));
            bisect_note("H0  找到 IndRun.dll @ %p (等待了 %lu ms)",
                        reinterpret_cast<void*>(base_), waited);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
        waited += step;
    }
    ffb_log("超时: 等了 %lu ms 没等到 IndRun.dll 加载", timeout_ms);
    return false;
}

bool IndRunHookSource::verify_signatures() const {
    struct Check {
        uintptr_t rva;
        const uint8_t* sig;
        size_t len;
        const char* label;
    };
    // 长度各不相同 —— 无栈帧的 servo-off 只有 4 字节可比。
    const Check checks[] = {
        {kRvaInvokeTorque,    kSigTwoInts,  sizeof(kSigTwoInts),  "torque"},
        {kRvaInvokeViscosity, kSigTwoInts,  sizeof(kSigTwoInts),  "viscosity"},
        {kRvaInvokeVibration, kSigTwoInts,  sizeof(kSigTwoInts),  "vibration"},
        {kRvaInvokeServoOn,   kSigServoOn,  sizeof(kSigServoOn),  "servo-on"},
        {kRvaInvokeServoOff,  kSigServoOff, sizeof(kSigServoOff), "servo-off"},
    };

    bool all_ok = true;
    int n_ok = 0;
    for (const auto& c : checks) {
        const auto* p = reinterpret_cast<const uint8_t*>(base_ + c.rva);
        if (std::memcmp(p, c.sig, c.len) != 0) {
            // 期望/实际各打 8 字节, 保证足够看清差异(不依赖 c.len)。
            ffb_log("特征码不匹配 [%s] RVA 0x%04llX (比 %zu 字节):"
                    " 期望 %02X %02X %02X %02X %02X %02X %02X %02X,"
                    " 实际 %02X %02X %02X %02X %02X %02X %02X %02X"
                    " —— 这个 IndRun.dll 是别的构建, 拒绝挂钩",
                    c.label, static_cast<unsigned long long>(c.rva), c.len,
                    c.sig[0], c.sig[1], c.sig[2], c.sig[3],
                    c.sig[4], c.sig[5], c.sig[6],
                    c.len > 7 ? c.sig[7] : 0,
                    p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
            all_ok = false;
        } else {
            ++n_ok;
        }
    }
    if (all_ok) {
        ffb_log("特征码校验通过: 5/5 个 invoker 与逆向报告一致 (TimeDateStamp 0x69392E86)");
    } else {
        ffb_log("特征码校验结果: %d/5 通过", n_ok);
    }
    return all_ok;
}

bool IndRunHookSource::create_hook(LPVOID target, LPVOID detour, LPVOID* original,
                                   const char* label) {
    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK) {
        ffb_log("MH_CreateHook 失败 [%s]: %s", label, MH_StatusToString(st));
        return false;
    }
    st = MH_EnableHook(target);
    if (st != MH_OK) {
        ffb_log("MH_EnableHook 失败 [%s]: %s", label, MH_StatusToString(st));
        return false;
    }
    ffb_log("已挂钩 [%s] @ %p", label, target);
    return true;
}

bool IndRunHookSource::install() {
    if (installed_) return true;

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        ffb_log("MH_Initialize 失败: %s", MH_StatusToString(st));
        return false;
    }
    minhook_ready_ = true;

    // 这一步可能是从 on_unreal_init 调用的, IndRun.dll 作为 UE4 插件模块
    // 有可能还没被 LoadLibraryW 加载, 所以等一等。
    if (!wait_for_module(15000)) {
        return false;
    }

    if (!verify_signatures()) {
        return false;
    }

    bool any = false;
    any |= create_hook(reinterpret_cast<LPVOID>(base_ + kRvaInvokeTorque),
                       reinterpret_cast<LPVOID>(&detour_torque),
                       reinterpret_cast<LPVOID*>(&original_torque_), "torque");
    any |= create_hook(reinterpret_cast<LPVOID>(base_ + kRvaInvokeViscosity),
                       reinterpret_cast<LPVOID>(&detour_viscosity),
                       reinterpret_cast<LPVOID*>(&original_viscosity_), "viscosity");
    any |= create_hook(reinterpret_cast<LPVOID>(base_ + kRvaInvokeVibration),
                       reinterpret_cast<LPVOID>(&detour_vibration),
                       reinterpret_cast<LPVOID*>(&original_vibration_), "vibration");
    any |= create_hook(reinterpret_cast<LPVOID>(base_ + kRvaInvokeServoOn),
                       reinterpret_cast<LPVOID>(&detour_servo_on),
                       reinterpret_cast<LPVOID*>(&original_servo_on_), "servo-on");
    any |= create_hook(reinterpret_cast<LPVOID>(base_ + kRvaInvokeServoOff),
                       reinterpret_cast<LPVOID>(&detour_servo_off),
                       reinterpret_cast<LPVOID*>(&original_servo_off_), "servo-off");

    installed_ = any;
    if (!any) {
        ffb_log("没有任何 hook 安装成功");
    }
    return installed_;
}

void IndRunHookSource::uninstall() {
    if (!minhook_ready_) return;

    // 先把 detour 停掉再删, 避免游戏线程在执行中被抽掉
    MH_DisableHook(MH_ALL_HOOKS);
    MH_RemoveHook(MH_ALL_HOOKS);

    if (installed_) {
        ffb_log("已卸载全部 hook");
    }
    installed_ = false;
}

// ---------------------------------------------------------------------------
// 读取
// ---------------------------------------------------------------------------
FfbState IndRunHookSource::snapshot() const {
    FfbState f{};
    const auto& s = shared_;

    f.torque_dir       = s.torque_dir.load(std::memory_order_relaxed);
    f.torque_magnitude = s.torque_mag.load(std::memory_order_relaxed);
    f.torque_at        = s.torque_at.load(std::memory_order_relaxed);

    f.viscosity_a      = s.visc_a.load(std::memory_order_relaxed);
    f.viscosity_b      = s.visc_b.load(std::memory_order_relaxed);
    f.viscosity_at     = s.visc_at.load(std::memory_order_relaxed);

    f.vibration_a      = s.vib_a.load(std::memory_order_relaxed);
    f.vibration_b      = s.vib_b.load(std::memory_order_relaxed);
    f.vibration_at     = s.vib_at.load(std::memory_order_relaxed);

    f.servo_on         = s.servo_on.load(std::memory_order_relaxed);
    f.servo_changed_at = s.servo_at.load(std::memory_order_relaxed);

    f.torque_calls     = s.n_torque.load(std::memory_order_relaxed);
    f.viscosity_calls  = s.n_visc.load(std::memory_order_relaxed);
    f.vibration_calls  = s.n_vib.load(std::memory_order_relaxed);
    f.servo_on_calls   = s.n_servo_on.load(std::memory_order_relaxed);
    f.servo_off_calls  = s.n_servo_off.load(std::memory_order_relaxed);

    f.torque_mag_min   = s.mag_min.load(std::memory_order_relaxed);
    f.torque_mag_max   = s.mag_max.load(std::memory_order_relaxed);
    f.torque_dir_min   = s.dir_min.load(std::memory_order_relaxed);
    f.torque_dir_max   = s.dir_max.load(std::memory_order_relaxed);
    f.vibration_b_min  = s.vib_min.load(std::memory_order_relaxed);
    f.vibration_b_max  = s.vib_max.load(std::memory_order_relaxed);
    f.viscosity_b_min  = s.visc_min.load(std::memory_order_relaxed);
    f.viscosity_b_max  = s.visc_max.load(std::memory_order_relaxed);

    f.ever_updated     = s.ever.load(std::memory_order_relaxed);
    return f;
}

std::string IndRunHookSource::status_report() const {
    char buf[1024];
    const auto& s = shared_;
    auto fmt_range = [](int32_t lo, int32_t hi) -> std::string {
        if (lo == INT32_MAX || hi == INT32_MIN) return "n/a";
        char b[64];
        _snprintf_s(b, sizeof(b), _TRUNCATE, "[%d..%d]", lo, hi);
        return b;
    };
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "hooks=%s base=%p | torque=%llu mag%s dir%s | visc=%llu b%s | "
                "vib=%llu b%s | servo on/off=%llu/%llu state=%d",
                installed_ ? "OK" : "FAIL",
                reinterpret_cast<void*>(base_),
                static_cast<unsigned long long>(s.n_torque.load(std::memory_order_relaxed)),
                fmt_range(s.mag_min.load(std::memory_order_relaxed),
                          s.mag_max.load(std::memory_order_relaxed)).c_str(),
                fmt_range(s.dir_min.load(std::memory_order_relaxed),
                          s.dir_max.load(std::memory_order_relaxed)).c_str(),
                static_cast<unsigned long long>(s.n_visc.load(std::memory_order_relaxed)),
                fmt_range(s.visc_min.load(std::memory_order_relaxed),
                          s.visc_max.load(std::memory_order_relaxed)).c_str(),
                static_cast<unsigned long long>(s.n_vib.load(std::memory_order_relaxed)),
                fmt_range(s.vib_min.load(std::memory_order_relaxed),
                          s.vib_max.load(std::memory_order_relaxed)).c_str(),
                static_cast<unsigned long long>(s.n_servo_on.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(s.n_servo_off.load(std::memory_order_relaxed)),
                s.servo_on.load(std::memory_order_relaxed) ? 1 : 0);
    return buf;
}

}  // namespace dglab_idac
