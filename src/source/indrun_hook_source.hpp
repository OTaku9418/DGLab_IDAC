// ---------------------------------------------------------------------------
// DGLab_IDAC — IndRun.dll 力反馈 hook 源
//
// 挂钩目标与 ABI 全部来自静态逆向(见 re/indrun_ffb_report.md)。
// 关键结论:
//
//   * IndRun.dll 是**语义层** —— 它算出力反馈的工程量, 然后调用 7 个
//     std::function 槽位。它自己没有任何出站 I/O 能力(不导入 CreateFileW /
//     WriteFile / DeviceIoControl / socket / CreateThread), 所以钩这里就是
//     钩信号源头。
//   * 真正的设备编码在 EXE 的 /Script/SerialPort 模块里, 走 UART 到机台。
//     AMDaemon 侧的 idachook.dll 跟这个游戏没有符号级链接, 是另一个进程。
//
// 挂钩的 5 个 invoker(全部 MSVC x64 __fastcall):
//
//   RVA 0x21C0  torque     void(int32 direction /*rcx*/, int32 magnitude /*rdx*/)
//   RVA 0x2200  viscosity  void(int32 /*rcx*/, int32 /*rdx*/)
//   RVA 0x2240  vibration  void(int32 /*rcx*/, int32 /*rdx*/)
//   RVA 0x2160  servo-on   void(void)      —— 有守卫: if(servoOff.set()) servoOn()
//   RVA 0x21A0  servo-off  void(void)
//
// invoker 内部会做 `mov [rsp+0x20],edx; mov [rsp+0x28],eax`(把两个参数落到栈上)
// 然后以 (rdx=&dir, r8=&mag) 指针形式调 std::function 的 vtable[0x10]。
// **但我们在 invoker 边界挂钩, 拿到的是寄存器里的值本身**, 不需要碰那个
// std::function。invoker 是真实 `call [rax+0x10]` 而不是尾 jmp, 所以
// prologue 里前 5 字节就是可重定位的完整指令, MinHook 安全。
//
// ⚠ 绝对不要在另一个线程里自己去解引用那个槽位 —— UnBindAllFB 会在没有
//   同步的情况下释放 payload。要观察就挂 invoker。
// ⚠ RVA 是**特定构建**的(TimeDateStamp 0x69392E86)。换 dump 版会失效。
//   所以这里做特征码校验, 对不上就拒绝安装而不是跳进错误的地址。
// ---------------------------------------------------------------------------
#pragma once

#include <atomic>
#include <string>
#include <windows.h>

#include "ffb_types.hpp"

namespace dglab_idac {

class IndRunHookSource final : public IFfbSource {
public:
    IndRunHookSource();
    ~IndRunHookSource() override;

    // 尝试挂载。module_base 传 0 表示自己去等/找 IndRun.dll。
    bool install() override;
    void uninstall() override;

    FfbState snapshot() const override;
    const char* name() const override { return "IndRunHookSource"; }

    // 诊断: hook 是否就绪 / 各自被调用了多少次
    std::string status_report() const;

    bool installed() const { return installed_; }

private:
    // ---- invoker 函数指针类型 ----
    using FnTwoInts = void(__fastcall*)(int32_t, int32_t);
    using FnVoid    = void(__fastcall*)();

    // ---- detour 目标(静态) ----
    static void __fastcall detour_torque(int32_t direction, int32_t magnitude);
    static void __fastcall detour_viscosity(int32_t a, int32_t b);
    static void __fastcall detour_vibration(int32_t a, int32_t b);
    static void __fastcall detour_servo_on();
    static void __fastcall detour_servo_off();

    // 单例: detour 是静态的, 只能通过这里回到实例
    static IndRunHookSource* s_instance_;

    // ---- 安装辅助 ----
    bool  wait_for_module(DWORD timeout_ms);
    bool  verify_signatures() const;   // 校验 RVA 处的字节特征
    bool  create_hook(LPVOID target, LPVOID detour, LPVOID* original, const char* label);
    void  publish() const;             // 把内部状态写进 snapshot

    HMODULE module_ = nullptr;
    uintptr_t base_ = 0;

    FnTwoInts original_torque_    = nullptr;
    FnTwoInts original_viscosity_ = nullptr;
    FnTwoInts original_vibration_ = nullptr;
    FnVoid    original_servo_on_  = nullptr;
    FnVoid    original_servo_off_ = nullptr;

    void* hook_torque_    = nullptr;
    void* hook_viscosity_ = nullptr;
    void* hook_vibration_ = nullptr;
    void* hook_servo_on_  = nullptr;
    void* hook_servo_off_ = nullptr;

    bool installed_ = false;
    bool minhook_ready_ = false;

    // ---- detour 写入的共享状态 ----
    // 全部是整数/原子, 不需要锁。double 时间戳只在 detour 里写一次,
    // 读取端容忍轻微撕裂(只用于超时判定)。
    struct Shared {
        std::atomic<int32_t>  torque_dir{0};
        std::atomic<int32_t>  torque_mag{0};
        std::atomic<double>   torque_at{0.0};

        std::atomic<int32_t>  visc_a{0};
        std::atomic<int32_t>  visc_b{0};
        std::atomic<double>   visc_at{0.0};

        std::atomic<int32_t>  vib_a{0};
        std::atomic<int32_t>  vib_b{0};
        std::atomic<double>   vib_at{0.0};

        std::atomic<bool>     servo_on{false};
        std::atomic<double>   servo_at{0.0};

        std::atomic<uint64_t> n_torque{0};
        std::atomic<uint64_t> n_visc{0};
        std::atomic<uint64_t> n_vib{0};
        std::atomic<uint64_t> n_servo_on{0};
        std::atomic<uint64_t> n_servo_off{0};

        std::atomic<int32_t>  mag_min{INT32_MAX};
        std::atomic<int32_t>  mag_max{INT32_MIN};
        std::atomic<int32_t>  dir_min{INT32_MAX};
        std::atomic<int32_t>  dir_max{INT32_MIN};
        std::atomic<int32_t>  vib_min{INT32_MAX};
        std::atomic<int32_t>  vib_max{INT32_MIN};
        std::atomic<int32_t>  visc_min{INT32_MAX};
        std::atomic<int32_t>  visc_max{INT32_MIN};

        std::atomic<bool>     ever{false};
    };
    Shared shared_{};
};

}  // namespace dglab_idac
