// ---------------------------------------------------------------------------
// DGLab_IDAC — 诊断版 mod（只在 -Diagnostic 构建里编入，替代 dllmain.cpp）
//
// 目的: 游戏一启动就 0xC0000005 崩掉，日志停在我们 on_program_start() 的
//       最后一行之后，看不出到底死在哪一句。这一版把**全部功能**换成
//       “每句一行、逐行 fflush”的检查点日志，用来把崩溃点夹逼到单条语句。
//
// 设计要点（都是为了“崩溃后还能留下证据”）:
//   * 不用 logger.cpp —— 它有自己的缓冲逻辑，崩溃时可能丢尾部。这里直接
//     CreateFileW + WriteFile + FlushFileBuffers，每行落盘。
//   * 不用 std::thread / MinHook / 配置解析 —— 全都先拿掉，一次只验证一件事。
//   * 用 SetUnhandledExceptionFilter 记最后一道防线（若崩溃发生在主线程）。
//   * 唯一副作用是写文件，除此之外不碰进程任何状态。
//
// 如果这一版**仍然崩**，看日志最后一行的编号就知道死在哪;
// 如果这一版**不崩**，说明问题在真实 mod 的某个具体步骤里 —— 逐项加回去即可。
// ---------------------------------------------------------------------------
#include <Mod/CppUserModBase.hpp>

#include <windows.h>
#include <psapi.h>   // EnumProcessModules / GetModuleBaseNameA

#include <cstdio>
#include <cstring>
#include <string>
#include <cstdarg>

namespace {

// --- 极简落盘日志（无缓冲、无 CRT 依赖、每行 flush） -----------------------
HANDLE g_log = INVALID_HANDLE_VALUE;
DWORD g_tid_at_load = 0;

void dlog_init() {
    if (g_log != INVALID_HANDLE_VALUE) return;
    // 固定路径：诊断阶段不猜工作目录，直接写死。
    const wchar_t* path =
        L"D:\\INDAC\\MODAC321\\WindowsNoEditor\\GameProject\\Binaries\\Win64"
        L"\\ue4ss\\Mods\\DGLab_IDAC\\diag.log";
    g_log = ::CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void dlog(const char* fmt, ...) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) n = static_cast<int>(strlen(body));

    char line[1200];
    int m = _snprintf_s(line, sizeof(line), _TRUNCATE, "[diag t=%lu] %s\r\n",
                        ::GetCurrentThreadId(), body);
    if (m < 0) m = static_cast<int>(strlen(line));

    DWORD written = 0;
    ::WriteFile(g_log, line, static_cast<DWORD>(m), &written, nullptr);
    ::FlushFileBuffers(g_log);
}

// --- 最后一道防线：崩溃时把异常码记下来 ------------------------------------
LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    const ULONG_PTR addr = (ep && ep->ExceptionRecord &&
                            ep->ExceptionRecord->NumberParameters >= 2)
                               ? ep->ExceptionRecord->ExceptionInformation[1]
                               : 0;
    dlog("!!!! UNHANDLED EXCEPTION code=0x%08lX faultaddr=0x%llX rip=0x%llX "
         "tid=%lu !!!!",
         code, static_cast<unsigned long long>(addr),
         ep && ep->ContextRecord ? static_cast<unsigned long long>(ep->ContextRecord->Rip) : 0ULL,
         ::GetCurrentThreadId());
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

// ===========================================================================
// 诊断用 mod：结构与本体的 vtable 布局完全一致，只是不干事
// ===========================================================================
class DiagMod final : public RC::CppUserModBase {
public:
    DiagMod() {
        dlog("C1  ctor 进入");
        ModName = STR("DGLab_IDAC_diag");
        ModVersion = DGLAB_VERSION_DIAG;  // 版本号只有 CMake 里那一处
        ModDescription = STR("diagnostic build");
        ModAuthors = STR("DGLab_IDAC");
        ModIntendedSDKVersion = STR("3.0.1");
        dlog("C2  ctor 元数据写完, 即将返回");
    }

    ~DiagMod() override { dlog("D1  dtor 进入"); }

    auto on_program_start() -> void override {
        dlog("P0  on_program_start 进入 (tid=%lu)", ::GetCurrentThreadId());

        dlog("P1  取 GetModuleHandleW(NULL)");
        HMODULE exe = ::GetModuleHandleW(nullptr);
        dlog("P2  exe=0x%llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(exe)));

        // 引擎模块（游戏 exe 自己）的信息 —— 纯读
        char exe_path[MAX_PATH] = {0};
        ::GetModuleFileNameA(exe, exe_path, sizeof(exe_path));
        dlog("P3  exe_path=%s", exe_path);

        dlog("P4  枚举已加载模块");
        HMODULE mods[512];
        DWORD needed = 0;
        if (::EnumProcessModules(::GetCurrentProcess(), mods, sizeof(mods), &needed)) {
            const size_t n = needed / sizeof(HMODULE);
            dlog("P5  模块数=%zu", n);
            for (size_t i = 0; i < n && i < 512; ++i) {
                char nm[MAX_PATH] = {0};
                if (::GetModuleBaseNameA(::GetCurrentProcess(), mods[i], nm, sizeof(nm))) {
                    dlog("P6    [%3zu] 0x%llX %s", i,
                         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mods[i])), nm);
                }
            }
        } else {
            dlog("P5  EnumProcessModules 失败 err=%lu", ::GetLastError());
        }

        dlog("P7  注册 unhandled filter");
        ::SetUnhandledExceptionFilter(&unhandled_filter);

        dlog("P8  建一个探测线程");
        HANDLE th = ::CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            dlog("T1  子线程启动 (tid=%lu)", ::GetCurrentThreadId());
            for (int i = 0; i < 5; ++i) {
                dlog("T2  子线程 tick %d", i);
                ::Sleep(200);
            }
            dlog("T3  子线程正常结束");
            return 0;
        }, nullptr, 0, nullptr);
        dlog("P9  CreateThread -> 0x%llX err=%lu",
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(th)), ::GetLastError());
        if (th) ::CloseHandle(th);

        dlog("PA  on_program_start 正常返回");
    }

    auto on_unreal_init() -> void override { dlog("U1  on_unreal_init"); }

    auto on_dll_load(std::wstring_view name) -> void override {
        dlog("L1  on_dll_load len=%zu", name.size());
    }

    auto on_update() -> void override {
        static int calls = 0;
        if (++calls <= 3 || calls % 300 == 0) dlog("X1  on_update call#%d", calls);
    }
};

// ===========================================================================
// 导出（与本体一致）
// ===========================================================================
#define DGLAB_DIAG_API __declspec(dllexport)

extern "C" {

DGLAB_DIAG_API RC::CppUserModBase* start_mod() {
    dlog_init();
    g_tid_at_load = ::GetCurrentThreadId();
    dlog("===== DGLab_IDAC 诊断构建 =====");
    dlog("A0  DLL 静态初始化完成, start_mod 被调用 (tid=%lu)", g_tid_at_load);
    auto* m = new DiagMod();
    dlog("A1  new DiagMod 返回 0x%llX",
         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(m)));
    return m;
}

DGLAB_DIAG_API void uninstall_mod(RC::CppUserModBase* mod) {
    dlog("A2  uninstall_mod(0x%llX)",
         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mod)));
    delete mod;
    dlog("A3  uninstall_mod 结束");
    if (g_log != INVALID_HANDLE_VALUE) {
        ::CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    }
}

}  // extern "C"
