// ---------------------------------------------------------------------------
// bisect_logger — 二分定位用的最小日志 + 真实异常捕获
//
// 为什么需要它 (而不是继续用 UE4SS 的日志):
//   * UE4SS 的 Output::send 会落进 UE4SS.log, 但崩溃时最后几行可能还没刷盘。
//     这里每条都 fflush, 保证"最后一行"就是真实的最后一步。
//   * LocalRadio 自带的崩溃报告器在崩溃时自己也被破坏了 —— 实测它吐出来的
//     fault 地址是 0x6575 (= UTF-16 "ue") 和 0x6700610069 (= "gai"), 即 CPU
//     在"执行"普通字符串数据, 说明它读到的是被踩坏的栈。它的输出不可信,
//     所以我们要抢在它前面拿到原始异常。
//
// 关键设计:
//   1) 在 DllMain(PROCESS_ATTACH) 里就注册 VEH —— 这是本 DLL 最早能跑代码的
//      地方, 早于 start_mod()/on_program_start()。
//   2) 记录 ExceptionCode / ExceptionAddress / RIP / RSP / RBP 以及模块归属,
//      并倒出栈上的原始 8 字节字 (崩溃瞬间的原始 memory, 不做任何解释)。
//   3) 日志直接写 repo 外的固定路径, 不依赖 UE4SS 的日志设施。
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>

namespace dglab_idac {

// 打开日志文件 (追加) 并注册 VEH。可重复调用, 只有第一次生效。
// 返回日志文件路径; 失败返回空串。
void bisect_init();

// 注册 VEH, 但不动日志文件 (给 DllMain 用, 此时还不知道配置路径)。
void bisect_install_veh();

// 写一个检查点。/ 每条都会 fflush, 所以崩溃前的最后一条就是真实进度。
void bisect_note(const char* fmt, ...);

// 当前日志文件路径 (bisect_init 之后有效)。
const char* bisect_log_path();

// 关掉日志。
void bisect_shutdown();

}  // namespace dglab_idac
