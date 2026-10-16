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

namespace dglab_idac {

namespace {

inline uint64_t to_bits(double v) { return std::bit_cast<uint64_t>(v); }
inline double from_bits(uint64_t b) { return std::bit_cast<double>(b); }

// steady_clock 秒。只用于超时判定, 精度 ms 级足够。
inline double now_seconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

void log_line(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    // UE4SS 会捕获 OutputDebugString; 同时写 stdout 便于控制台模式
    ::OutputDebugStringA("[DGLab_IDAC] ");
    ::OutputDebugStringA(buf);
    ::OutputDebugStringA("\n");
    std::fputs("[DGLab_IDAC] ", stdout);
    std::fputs(buf, stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
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
// 形如: mov eax,ecx / mov rcx,[rip+disp] / test rcx,rcx / je ...
// 前 3 字节 `8B C1 48` 对扭矩/粘度/振动都成立(都从 mov eax,ecx 开始)。
// servo-on/off 从 `48 8B 05` (mov rax,[rip+disp]) 开始。
constexpr uint8_t kSigTwoInts[3] = {0x8B, 0xC1, 0x48};
constexpr uint8_t kSigVoid[3]    = {0x48, 0x8B, 0x05};

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
        s.torque_at.store(to_bits(now_seconds()), std::memory_order_relaxed);
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
        s.visc_at.store(to_bits(now_seconds()), std::memory_order_relaxed);
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
        s.vib_at.store(to_bits(now_seconds()), std::memory_order_relaxed);
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
            s.servo_at.store(to_bits(now_seconds()), std::memory_order_relaxed);
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
            s.servo_at.store(to_bits(now_seconds()), std::memory_order_relaxed);
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
            log_line("找到 IndRun.dll @ %p", reinterpret_cast<void*>(base_));
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
        waited += step;
    }
    log_line("超时: 等了 %lu ms 没等到 IndRun.dll 加载", timeout_ms);
    return false;
}

bool IndRunHookSource::verify_signatures() const {
    struct Check {
        uintptr_t rva;
        const uint8_t* sig;
        const char* label;
    };
    const Check checks[] = {
        {kRvaInvokeTorque,    kSigTwoInts, "torque"},
        {kRvaInvokeViscosity, kSigTwoInts, "viscosity"},
        {kRvaInvokeVibration, kSigTwoInts, "vibration"},
        {kRvaInvokeServoOn,   kSigVoid,    "servo-on"},
        {kRvaInvokeServoOff,  kSigVoid,    "servo-off"},
    };

    bool all_ok = true;
    for (const auto& c : checks) {
        const auto* p = reinterpret_cast<const uint8_t*>(base_ + c.rva);
        if (std::memcmp(p, c.sig, 3) != 0) {
            log_line("特征码不匹配 [%s] RVA 0x%04llX: 期望 %02X %02X %02X, 实际 %02X %02X %02X"
                     " —— 这个 IndRun.dll 是别的构建, 拒绝挂钩",
                     c.label, static_cast<unsigned long long>(c.rva),
                     c.sig[0], c.sig[1], c.sig[2], p[0], p[1], p[2]);
            all_ok = false;
        }
    }
    return all_ok;
}

bool IndRunHookSource::create_hook(LPVOID target, LPVOID detour, LPVOID* original,
                                   const char* label) {
    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK) {
        log_line("MH_CreateHook 失败 [%s]: %s", label, MH_StatusToString(st));
        return false;
    }
    st = MH_EnableHook(target);
    if (st != MH_OK) {
        log_line("MH_EnableHook 失败 [%s]: %s", label, MH_StatusToString(st));
        return false;
    }
    log_line("已挂钩 [%s] @ %p", label, target);
    return true;
}

bool IndRunHookSource::install() {
    if (installed_) return true;

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        log_line("MH_Initialize 失败: %s", MH_StatusToString(st));
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
        log_line("没有任何 hook 安装成功");
    }
    return installed_;
}

void IndRunHookSource::uninstall() {
    if (!minhook_ready_) return;

    // 先把 detour 停掉再删, 避免游戏线程在执行中被抽掉
    MH_DisableHook(MH_ALL_HOOKS);
    MH_RemoveHook(MH_ALL_HOOKS);

    if (installed_) {
        log_line("已卸载全部 hook");
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
    f.torque_at        = from_bits(s.torque_at.load(std::memory_order_relaxed));

    f.viscosity_a      = s.visc_a.load(std::memory_order_relaxed);
    f.viscosity_b      = s.visc_b.load(std::memory_order_relaxed);
    f.viscosity_at     = from_bits(s.visc_at.load(std::memory_order_relaxed));

    f.vibration_a      = s.vib_a.load(std::memory_order_relaxed);
    f.vibration_b      = s.vib_b.load(std::memory_order_relaxed);
    f.vibration_at     = from_bits(s.vib_at.load(std::memory_order_relaxed));

    f.servo_on         = s.servo_on.load(std::memory_order_relaxed);
    f.servo_changed_at = from_bits(s.servo_at.load(std::memory_order_relaxed));

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

void IndRunHookSource::note_tick(double now) {
    last_tick_.store(to_bits(now), std::memory_order_relaxed);
}

}  // namespace dglab_idac
