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
//     * CppUserModBase 的 vtable 布局必须完全一致 —— 所以我们锁死的是
//       _ue4ss_ref/ue4ss_0196ef29 这份对齐头文件(commit 0196ef294f852), 它
//       不是上游 tag v3.0.1(d935b5b), 两者相差 944 个提交、vtable 槽位不同。
//   下面的 static_assert 与运行期自检就是为了让"配错了"变成可见的失败,
//   而不是随机崩溃。
// ---------------------------------------------------------------------------
#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <DynamicOutput/OutputDevice.hpp>

#include <windows.h>
#include <psapi.h>   // EnumProcessModules / GetModuleBaseNameA

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "bisect_logger.hpp"
#include "config.hpp"
#include "ffb_gap_stats.hpp"
#include "logger.hpp"
#include "pipeline.hpp"
#include "source/indrun_hook_source.hpp"
#include "transport/transport.hpp"

// ===========================================================================
// DllMain —— 整个 mod 里最早能跑我们自己代码的地方
//
// 只做一件事: 注册 Vectored Exception Handler。这样无论后面在哪一步崩,
// 我们都能把**原始异常记录**先写进 bisect.log, 而不是只能看 LocalRadio 那个
// 自己也被踩坏的报告器吐出来的假地址 (它报的 0x6575 / 0x6700610069 其实是
// UTF-16 字符串 "ue" / "gai", 即 CPU 在执行数据 —— 那是栈被踩坏的产物)。
//
// 不做任何其它事: DllMain 里持有 loader lock, 分配内存/建线程都可能死锁。
// ===========================================================================
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        dglab_idac::bisect_install_veh();
    }
    return TRUE;
}

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

// ---------------------------------------------------------------------------
// 启动分级 (bisect)
//
// 症状: 禁用本 mod -> 游戏一路跑完正常退出; 启用 -> UE4SS 初始化早期就
//       0xC0000005, 而且 LocalRadio 的报告器给出的 fault 地址是字符串数据。
// 也就是**我们做的某件具体的事**把 UE4SS 的状态踩坏了。
//
// 所以把 on_program_start() 拆成可以逐级打开的阶梯, 用 bisect.txt 里的
// 一个数字控制。这样不用反复重编译就能定位到"从哪一级开始崩"。
//
//    0  = 什么都不做 (只写检查点), 验证"光加载这个 DLL"是否安全
//    1  = 只用我们自己的 bisect 日志
//    2  = 额外调一次 UE4SS 的 Output::send (验证跨 DLL 的日志设施)
//    3  = 额外开 logger.cpp 的文件日志 (验证 fwrite/路径推导)
//    4  = 额外读/写 INI 配置 (验证 GetPrivateProfile*)
//    5  = 额外起一个什么都不做的后台线程 (验证 std::thread)
//   10  = 加传输层 (干跑 NullTransport)
//   11  = 全部打开 (挂钩 + 传输 + 管线线程)
//
// 改法: 编辑 <mod>\dlls\bisect.txt, 里面只写一个数字。
// 不想分级调试就把 bisect.txt 删掉 —— 缺省按全开(kStageFull)跑, 不是按 0 跑。
// ---------------------------------------------------------------------------
constexpr int kStageFull = 11;

int read_bisect_stage() {
    // 先看环境变量 (调试时更方便), 再看文件。
    if (const char* e = std::getenv("DGLAB_BISECT_STAGE")) {
        return std::atoi(e);
    }

    // 文件在 DLL 自己的目录里, 跟 bisect.log / DGLab_IDAC.ini 放一起。
    // 路径由 DLL 地址反查, 不依赖工作目录。
    HMODULE self = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&read_bisect_stage), &self)) {
        return -1;
    }
    wchar_t path[MAX_PATH]{};
    if (!::GetModuleFileNameW(self, path, MAX_PATH)) return -1;
    if (wchar_t* slash = wcsrchr(path, L'\\')) *(slash + 1) = L'\0';
    wcscat_s(path, L"bisect.txt");

    FILE* f = nullptr;
    if (::_wfopen_s(&f, path, L"rb") != 0 || !f) return -1;
    char buf[64]{};
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    return std::atoi(buf);
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
        ModVersion = DGLAB_VERSION_W;  // 版本号只有 CMake 里那一处
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
        // ===================================================================
        // 分级启动 —— 每跨一级都写一条检查点, 崩溃时 bisect.log 的最后一条
        // 就是"已经成功走到了这一步"。
        //
        // 全部包在 try/catch 里: 这个函数跑在 UE4SS 的初始化路径上, 任何
        // 异常逃出去都会连带毁掉整个初始化流程。
        // ===================================================================
        try {
            start_staged();
        } catch (const std::exception& e) {
            dglab_idac::bisect_note("!!! on_program_start 抛异常: %s", e.what());
        } catch (...) {
            dglab_idac::bisect_note("!!! on_program_start 抛了未知异常");
        }
        dglab_idac::bisect_note("ZZ  on_program_start 返回 (stage=%d)", stage_);
    }

private:
    void start_staged() {
        const int stage_raw = dglab_idac::read_bisect_stage();
        dglab_idac::bisect_init();

        // ★ 缺省即全开, **不能**缺省即"什么都不做"。
        //
        // 2026-10-17 踩的坑: bisect.txt 被当成日志文件一起归档掉了, 于是
        // read_bisect_stage() 返回 0, mod 加载成功、检查点齐全、零崩溃 ——
        // 却什么都初始化了(transport_ 根本没建), 整轮真机测试白跑。
        // 静默降级成"不工作"是最坏的默认值: 表面上一切正常, 没有任何报错。
        // 所以文件/环境变量都不在时, 明确按全开跑, 并在两个日志里各留一条。
        if (stage_raw < 0) {
            stage_ = kStageFull;
            stage_inferred_ = true;
            dglab_idac::bisect_note("!!  没有找到 bisect.txt / DGLAB_BISECT_STAGE, "
                                    "按全开运行 (stage=%d)", stage_);
        } else {
            stage_ = stage_raw;
        }

        dglab_idac::bisect_note("A0  进入 on_program_start, stage=%d", stage_);
        dglab_idac::bisect_note("A0  bisect.log = %s", dglab_idac::bisect_log_path());

        const std::string ini = dglab_idac::resolve_config_path();
        dglab_idac::bisect_note("A1  resolve_config_path -> %s", ini.c_str());

        if (stage_ < 1) {
            dglab_idac::bisect_note("A9  stage<1: 什么都不做, 直接返回");
            return;
        }

        // ---- stage>=1: 我们自己的文件日志 ----
        dglab_idac::log_init();
        // 这条要等 log_init() 之后才能写 —— 之前调 log_* 时文件还没打开,
        // 消息会被直接丢掉(见 logger.cpp 的 emit(): g_file 为空就 no-op)。
        if (stage_inferred_) {
            dglab_idac::log_warn("没有找到 bisect.txt / DGLAB_BISECT_STAGE —— 按全开运行 "
                                 "(stage=%d)。想分级调试就放一个只写数字的 bisect.txt。",
                                 stage_);
        }
        dglab_idac::bisect_note("A2  logger.cpp 文件日志已开");

        if (stage_ < 2) { dglab_idac::bisect_note("A9  stage<2: 到此为止"); return; }

        // ---- stage>=2: 跨 DLL 调一次 UE4SS 自己的日志设施 ----
        // 这一步验证的是"从我们的 DLL 调进 UE4SS.dll 导出的 Output::send"
        // 是否安全。C++ mod 的 ABI 隐患主要就在这里。
        RC::Output::send<RC::LogLevel::Normal>(
            STR("[DGLab_IDAC] bisect stage=%d, 正在验证跨 DLL 日志调用\n"), stage_);
        dglab_idac::bisect_note("A3  UE4SS Output::send 调用成功");

        if (stage_ < 3) { dglab_idac::bisect_note("A9  stage<3: 到此为止"); return; }

        dglab_idac::log_info("==== DGLab_IDAC %s 启动 (bisect stage=%d) ====", DGLAB_VERSION_A, stage_);
        dglab_idac::log_info("日志文件: %s", dglab_idac::log_path());
        dglab_idac::log_info("配置文件: %s", ini.c_str());
        dglab_idac::bisect_note("A4  logger 头几行已写");

        if (stage_ < 4) { dglab_idac::bisect_note("A9  stage<4: 到此为止"); return; }

        // ---- stage>=4: INI 读写 ----
        if (ini.empty()) {
            dglab_idac::log_warn("无法推导配置路径, 全部使用内置默认值");
        } else if (::GetFileAttributesA(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
            dglab_idac::write_default_config(ini);
            dglab_idac::bisect_note("A5  写了一份默认配置");
        }
        cfg_ = dglab_idac::load_config(ini);
        dglab_idac::bisect_note("A6  配置读完: enable=%d hook=%d link=%s dry_run=%d",
                                cfg_.enabled ? 1 : 0, cfg_.hook ? 1 : 0,
                                dglab_idac::to_string(cfg_.link), cfg_.dry_run ? 1 : 0);
        dglab_idac::log_info("配置: enable=%d verbose=%d link=%s protocol=%s dry_run=%d",
                             cfg_.enabled ? 1 : 0, cfg_.verbose ? 1 : 0,
                             dglab_idac::to_string(cfg_.link),
                             dglab_idac::to_string(cfg_.protocol),
                             cfg_.dry_run ? 1 : 0);

        if (stage_ < 5) { dglab_idac::bisect_note("A9  stage<5: 到此为止"); return; }

        // ---- stage>=10: 传输层 (必须在启动后台线程之前!) ----
        //
        // ⚠ 顺序很关键。2026-10-16 在这里踩过一次坑:
        //   原先的次序是 [起线程] -> [建传输层] -> [建 source_]。
        //   而 thread_main() 开头有一句 `if (!source_) break;`, 工作线程
        //   在 source_ 被赋值之前(实测早 15ms)就检查了它, 于是整个循环
        //   直接跳出 —— install() 一次都没被调用过, 日志表现为
        //   `hooks=FAIL base=0` 但模块清单里 IndRun.dll 明明在。
        //   现在改成"启动阶段需要的对象全部建好, 最后一步才起线程"。
        if (stage_ >= 10) {
            transport_ = dglab_idac::create_transport(cfg_);
            if (transport_ && !transport_->start()) {
                dglab_idac::log_error("传输启动失败, 降级到空输出");
                dglab_idac::destroy_transport(transport_);
                transport_ = nullptr;
            }
            dglab_idac::bisect_note("A8  传输层就绪: %s", transport_ ? transport_->name() : "none");
        }

        // ---- stage>=11: 挂钩源 (同样必须在起线程之前) ----
        // ★ 这里只是"建对象", 绝不在本函数里做任何等待 ★
        //
        // 血的教训: on_program_start() 跑在**游戏主线程**上。上一版在 install()
        // 里放了一句 wait_for_module(15000) —— 主线程被硬等 15 秒, 游戏看门狗
        // 超时、UE4SS 自己的初始化卡死, 最后整个进程 0xC0000005 崩掉。
        // 挂钩(包括那句 15 秒等待)是后台线程的事, 主线程一步都不能停。
        if (stage_ >= 11) {
            if (cfg_.hook) {
                source_ = std::make_unique<dglab_idac::IndRunHookSource>();
            } else {
                dglab_idac::log_warn("[debug] hook=0 —— 故意不挂 IndRun.dll（诊断用）");
            }
            pipeline_ = std::make_unique<dglab_idac::Pipeline>(cfg_);
            dglab_idac::bisect_note("A9  stage=11 全部就绪, 即将起线程");
        }

        // ---- stage>=5: 后台线程 (整个启动流程的最后一步) ----
        running_ = true;
        worker_ = std::thread([this] { thread_main(); });
        dglab_idac::bisect_note("A7  后台线程已创建");

        if (stage_ < 10) { dglab_idac::bisect_note("A9  stage<10: 到此为止"); return; }
        if (stage_ < 11) { dglab_idac::bisect_note("A9  stage<11: 到此为止"); return; }

        dglab_idac::log_info("已启动。输出=%s  真实输出=%s",
                             transport_ ? transport_->name() : "none",
                             cfg_.dry_run ? "否(dry_run=1 硬开关)" : "是");
        dglab_idac::log_info("提示: 进游戏后方向盘一动, 日志里就该出现 torque/vibration 计数增长");
    }

public:

    auto on_unreal_init() -> void override {
        dglab_idac::log_info("on_unreal_init: UE4 引擎已就绪");
    }

    // -----------------------------------------------------------------------
    // IndRun.dll 迟到加载的补救路径
    //
    // UE4SS 会给 LoadLibraryA/W 和 LoadLibraryExA/W 挂钩, 每次加载都回调到这里
    // (它**不**挂 LdrLoadDll, 但 UE4 插件走的正是 LoadLibraryW)。
    // 所以即使前面 10 秒的重试窗口错过了, 这里还能补挂上。
    // -----------------------------------------------------------------------
    auto on_dll_load(std::wstring_view dll_name) -> void override {
        // ★ 这里**只举手, 不干活** ★
        //
        // on_dll_load 是从 UE4SS 的 LoadLibrary 钩子里回调出来的 —— 那一刻
        // 进程正握着 loader lock。在这种情况下调 MinHook、建线程、或任何
        // 分配内存的操作都可能死锁或崩掉。所以这里只置一个标志位, 真正的
        // 挂钩交给后台线程在 loader lock 释放之后去做。
        if (!source_ || source_->installed()) return;

        auto lower = [](wchar_t c) -> wchar_t {
            return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c;
        };
        bool is_indrun = false;
        constexpr wchar_t kNeedle[] = L"indrun";
        if (dll_name.size() >= 6) {
            for (size_t i = 0; i + 6 <= dll_name.size() && !is_indrun; ++i) {
                bool hit = true;
                for (size_t k = 0; k < 6; ++k) {
                    if (lower(dll_name[i + k]) != kNeedle[k]) { hit = false; break; }
                }
                is_indrun = hit;
            }
        }
        if (!is_indrun) return;

        indrun_seen_ = true;   // 后台线程会看到并接手
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

        // -------------------------------------------------------------------
        // [bisect stage 5] 轻量线程存活测试
        //
        // 为什么需要这一段: stage 5 以下 source_ 还是空的, 直接走下面的循环会
        // 立刻 break —— 那样"stage 5 通过"其实什么都没验证, 白跑一次游戏。
        //
        // 这里真的让线程活几秒: 打心跳、sleep、再打心跳, 覆盖
        //   * std::thread 能正常起
        //   * <chrono> sleep_for 正常
        //   * 跨线程调用 bisect_note()/log_info() 不炸
        //    * running_ 置 false 后线程能自然退出、join 不卡
        // 只读 running_, 不碰任何游戏/UE4SS 对象, 所以不会引入新的崩溃面。
        // -------------------------------------------------------------------
        if (stage_ == 5) {
            dglab_idac::bisect_note("B0  后台线程启动, 本线程 tid=%lu",
                                    static_cast<unsigned long>(::GetCurrentThreadId()));
            for (int i = 0; i < 5 && running_; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                if (!running_) break;
                dglab_idac::bisect_note("B1  心跳 %d/5 (tid=%lu)", i + 1,
                                        static_cast<unsigned long>(::GetCurrentThreadId()));
            }
            dglab_idac::bisect_note("B2  线程存活测试结束 (running_=%d)", running_ ? 1 : 0);
            return;
        }

        // -------------------------------------------------------------------
        // 等 IndRun.dll 出现 —— **只在这个后台线程里等**
        //
        // IndRun.dll 是 UE4 插件, 加载得比本 mod 晚。上一版把这个等待放在
        // on_program_start()(游戏主线程)里, 直接等崩了整个进程。
        //
        // 现在: 本线程自查 GetModuleHandleW, 看到模块才调 install(); install()
        // 自己那句 15 秒等待因此几乎不会真的用满。整个过程中游戏主线程没有任何
        // 阻塞, UE4SS 的启动流程也不会被拖住。
        // -------------------------------------------------------------------
        for (int i = 0; i < 300 && running_; ++i) {
            if (!source_) break;                       // [debug] hook=0 或已放弃
            if (source_->installed()) break;
            // on_dll_load 举了旗, 或者模块已经在了 —— 两种都算"可以挂了"
            if (indrun_seen_ || ::GetModuleHandleW(L"IndRun.dll")) {
                if (source_->install()) {
                    dglab_idac::log_info("力反馈源就绪: %s", source_->status_report().c_str());
                    dglab_idac::bisect_note("H1  挂钩成功: %s",
                                            source_->status_report().c_str());
                    break;
                }
                // install() 失败(例如特征码不匹配)时不要死循环重试:
                // 换个构建的 IndRun.dll 不会因为我们多试几次就变得匹配。
                dglab_idac::bisect_note("H1  挂钩失败: %s",
                                        source_->status_report().c_str());
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // 还是没挂上? 把进程里所有模块名倒出来, 免得只能靠猜。
        // UE4 插件不一定叫 IndRun.dll —— 名字一变, 特征码校验也救不了。
        if (!source_) {
            // [debug] hook=0 —— 用户故意关掉挂钩做 A/B 对照
        } else if (!source_->installed()) {
            dglab_idac::log_error("重试窗口结束仍未挂上 IndRun.dll —— 已加载模块清单:");
            HMODULE mods[512];
            DWORD needed = 0;
            if (::EnumProcessModules(::GetCurrentProcess(), mods, sizeof(mods), &needed)) {
                const size_t n = needed / sizeof(HMODULE);
                for (size_t i = 0; i < n && i < 512; ++i) {
                    char name[MAX_PATH] = {0};
                    if (::GetModuleBaseNameA(::GetCurrentProcess(), mods[i], name, sizeof(name))) {
                        dglab_idac::log_error("    [%3zu] %s", i, name);
                    }
                }
            }
            dglab_idac::log_error("如果清单里根本没有 IndRun.dll, 说明这个游戏构建"
                                  "没走 UE4 插件路径 —— 需要换挂钩目标");
        }

        const auto period = std::chrono::milliseconds(
            cfg_.frame_ms > 0 ? cfg_.frame_ms : 100);

        auto next = clock::now();
        double last_heartbeat = 0.0;
        // [tx] 另一条独立的 10 秒计时: 心跳是每秒一次的, 传输层状态没必要跟着刷。
        double last_tx_report = 0.0;
        // "一直没有力反馈回调"告警的限流状态 (T13b): 第一次超 20 s 打一次,
        // 之后每 60 s 最多一次; warned_no_ffb 用来判断稍后要不要打"首次收到"。
        bool warned_no_ffb = false;
        bool logged_first_ffb = false;
        double last_noffb_warn = 0.0;
        double start_time = now_seconds();

        uint64_t frames = 0;
        uint64_t prev_torque = 0, prev_visc = 0, prev_vib = 0;

        // [ffbgap] (T9): 60 秒一行。故意比心跳(1 s)稀疏得多 —— 这是长期统计,
        // 不是实时状态, 打太密只会让日志变贵。
        //
        // T14a: 再接一份 viscosity 的间隔统计。理由: T17(阻尼保持超时淡出)的
        // 阈值要由"比赛中 viscosity 的最大回调间隔"决定, 而这个问题只有
        // 实机数据能回答。两份统计各打一行, 用 src= 区分, 分桶口径完全相同。
        dglab_idac::FfbGapStats gap_stats;
        dglab_idac::FfbGapStats visc_gap_stats;
        double last_gap_report = now_seconds();
        constexpr double kGapReportSeconds = 60.0;
        // T18: 打完不清"上一次回调的时刻", 只清计数 —— 跨过 60 s 边界的间隔
        // 才是真正想抓的那一类(菜单里停更), 清掉时间戳等于把它整段丢掉。
        auto report_gap = [](const char* src, double t, double start, dglab_idac::FfbGapStats& g) {
            const uint64_t total = g.n_lt_05 + g.n_05_2 + g.n_2_5 + g.n_gt_5;
            dglab_idac::log_info(
                "[ffbgap] src=%s t=%.0fs intervals=%llu <0.5s=%llu 0.5-2s=%llu "
                "2-5s=%llu >5s=%llu max=%.2fs open=%.2fs zero_stops=%llu",
                src, t - start, static_cast<unsigned long long>(total),
                static_cast<unsigned long long>(g.n_lt_05),
                static_cast<unsigned long long>(g.n_05_2),
                static_cast<unsigned long long>(g.n_2_5),
                static_cast<unsigned long long>(g.n_gt_5), g.max_gap,
                g.open_seconds(t), static_cast<unsigned long long>(g.stops));
            g.reset_counts();
        };

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
                // ---- [ffbgap] 诊断: 只在"有新回调"时记一次 ----
                // 用 torque_calls 的增量判断, 而不是每 tick 都记 —— 否则
                // 采样频率就变成了主循环频率, 统计出来的东西没有意义。
                if (ffb.torque_calls != prev_torque) {
                    gap_stats.on_torque(t, ffb.torque_magnitude);
                }
                // viscosity 用 b 参数当"幅值"(a 是符号, 见 ffb_types.hpp):
                // b>0 才算"在施阻尼", 归 0 时该段区间结束。
                if (ffb.viscosity_calls != prev_visc) {
                    visc_gap_stats.on_torque(t, ffb.viscosity_b);
                }
                if (t - last_gap_report >= kGapReportSeconds) {
                    last_gap_report = t;
                    report_gap("torque", t, start_time, gap_stats);
                    report_gap("visc", t, start_time, visc_gap_stats);
                }

                prev_torque = ffb.torque_calls;
                prev_visc = ffb.viscosity_calls;
                prev_vib = ffb.vibration_calls;

                // ---- 周期心跳: 这是判断"游戏到底发不发 FFB"的关键证据 ----
                // T14b: 末尾再补一段"当前状态"。原因是上面那些 mag/b 都只是
                // 累计 min/max, 菜单里阻尼停住时根本看不出"现在是多少"; 而
                // [probe] 只在回调到来时打, 停住时也没有。
                // age 的时钟已核对过: FfbState::*_at 与这里的 now_seconds()
                // 都是 steady_clock 的 time_since_epoch 秒, 同一个起点。
                if (t - last_heartbeat >= kHeartbeatSeconds) {
                    last_heartbeat = t;
                    const dglab_idac::FfbState s = source_->snapshot();
                    auto age_of = [t](double at) { return at > 0.0 ? (t - at) : -1.0; };
                    auto max_pulse = [](const dglab_idac::ChannelFrame& ch) {
                        int m = 0;
                        for (int i = 0; i < dglab_idac::kSlotsPerFrame; ++i) {
                            if (static_cast<int>(ch.slots[i].pulse_intensity) > m) {
                                m = static_cast<int>(ch.slots[i].pulse_intensity);
                            }
                        }
                        return m;
                    };
                    const int out_a_s = have_frame ? frame.a.strength : -1;
                    const int out_b_s = have_frame ? frame.b.strength : -1;
                    const int out_a_p = have_frame ? max_pulse(frame.a) : -1;
                    const int out_b_p = have_frame ? max_pulse(frame.b) : -1;
                    dglab_idac::log_info(
                        "[hb] t=%.0fs frames=%llu | torque=%llu mag[%d..%d] dir[%d..%d] | "
                        "visc=%llu b[%d..%d] | vib=%llu b[%d..%d] | servo on/off=%llu/%llu=%d | "
                        "%s | %s | now: tq_m=%d visc_b=%d vib_b=%d "
                        "age(tq/visc/vib)=%.1f/%.1f/%.1fs servo=%d out A=%d/%d B=%d/%d",
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
                        source_->status_report().c_str(),
                        pipeline_ ? pipeline_->gain_summary().c_str() : "pipeline=null",
                        s.torque_magnitude, s.viscosity_b, s.vibration_b,
                        age_of(s.torque_at), age_of(s.viscosity_at), age_of(s.vibration_at),
                        s.servo_on ? 1 : 0, out_a_s, out_a_p, out_b_s, out_b_p);

                    // ---- 传输层状态: 每 10 秒一行(不是每秒, 否则日志本身就成了负担) ----
                    // 为什么要单独打这个: 传输层的计数(submitted/repeated/overwritten)
                    // 从来没进过周期日志, 而它正是"管线与传输两个线程的相位对不对"
                    // 的唯一证据。repeated 高说明传输比管线快、在重发旧帧; overwritten
                    // 高说明传输跟不上、帧被丢弃。两者都高就是相位打架。
                    if (transport_ != nullptr && t - last_tx_report >= 10.0) {
                        last_tx_report = t;
                        dglab_idac::log_info("[tx] %s", transport_->status().c_str());
                    }

                    // 明确告警: 挂了 hook 但一次回调都没有 —— 说明要么还没进
                    // 游戏, 要么力反馈通路在 mode=xinput 下根本不走。
                    // ★ 限流 (T13b) ★ 这条告警在"一直停在菜单里"时会持续成立,
                    // 每秒一条会把日志冲干净。改成: 第一次超过 20 s 打一次, 之后
                    // 每 60 s 最多一次; 一旦真收到回调, 再打一条 INFO 收尾。
                    if (!s.ever_updated && (t - start_time) > 20.0) {
                        if (!warned_no_ffb || (t - last_noffb_warn) >= 60.0) {
                            warned_no_ffb = true;
                            last_noffb_warn = t;
                            dglab_idac::log_warn(
                                "[hb] 已经 %.0f 秒没有任何力反馈回调。可能原因: "
                                "(a) 还没进游戏; (b) 游戏在 xinput 模式下不生成力反馈; "
                                "(c) 增益 [manager+0x4E4] 恒为 0(见 ffb_types.hpp 的说明)。"
                                "(之后每 60 s 提醒一次)",
                                t - start_time);
                        }
                    } else if (warned_no_ffb && !logged_first_ffb && s.ever_updated) {
                        // 这条 INFO 很重要: 它把"告警是在等什么"闭合掉, 否则日志里
                        // 只有一串告警、看不出后来到底通没通。
                        logged_first_ffb = true;
                        dglab_idac::log_info("首次收到力反馈回调 (t=%.0fs)", t - start_time);
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
    int stage_ = 0;                        // bisect 分级, 见 read_bisect_stage()
    bool stage_inferred_ = false;          // stage 不是从 bisect.txt 读来的, 而是缺省补的
    std::unique_ptr<dglab_idac::IndRunHookSource> source_;
    std::unique_ptr<dglab_idac::Pipeline> pipeline_;
    dglab_idac::ITransport* transport_ = nullptr;

    std::atomic<bool> running_{false};
    // on_dll_load 里只置这个标志, 由后台线程接手挂钩（loader lock 里不能挂钩）
    std::atomic<bool> indrun_seen_{false};
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
                "DGLab_IDAC " DGLAB_VERSION_A " (probe stage) log=%s", dglab_idac::log_path());
    return buf;
}

}  // extern "C"
