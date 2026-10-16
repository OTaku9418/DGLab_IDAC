// ---------------------------------------------------------------------------
// bisect_logger 实现 —— 见头文件里的设计说明
// ---------------------------------------------------------------------------
#include "bisect_logger.hpp"

#include <psapi.h>          // EnumProcessModules / GetModuleBaseNameA / GetModuleInformation

#include <cstdarg>
#include <cstdio>
#include <cstdlib>      // wcstombs_s
#include <cstring>
#include <cwchar>

namespace dglab_idac {
namespace {

FILE* g_file = nullptr;
char g_path[MAX_PATH]{};
LONG g_veh_installed = 0;
volatile LONG g_in_handler = 0;

// 已知的几个地址, 用于把崩溃地址归到某个模块 (最有用的一条信息)。
struct KnownModule {
    const char* name;
    uintptr_t base;
    size_t size;
};
KnownModule g_mods[32]{};
volatile LONG g_mod_count = 0;

void add_module(const char* name, uintptr_t base, size_t size) {
    if (!base || g_mod_count >= 32) return;
    const LONG i = InterlockedIncrement(&g_mod_count) - 1;
    if (i >= 32) return;
    g_mods[i].name = name;
    g_mods[i].base = base;
    g_mods[i].size = size;
}

const char* module_of(uintptr_t addr, char* buf, size_t buflen) {
    const LONG n = g_mod_count;
    for (LONG i = 0; i < n && i < 32; ++i) {
        const uintptr_t b = g_mods[i].base;
        if (addr >= b && addr < b + g_mods[i].size) {
            _snprintf_s(buf, buflen, _TRUNCATE, "%s+0x%llX",
                        g_mods[i].name,
                        static_cast<unsigned long long>(addr - b));
            return buf;
        }
    }
    _snprintf_s(buf, buflen, _TRUNCATE, "<unknown>");
    return buf;
}

void raw_write(const char* s, size_t n) {
    if (!g_file) return;
    fwrite(s, 1, n, g_file);
    fflush(g_file);
}

void vnote(const char* fmt, ...) {
    if (!g_file) return;
    char body[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    char line[2240];
    const DWORD tid = GetCurrentThreadId();
    const int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
                              "[%10lu] [t%lu] %s\n",
                              GetTickCount(), tid, body);
    if (n > 0) raw_write(line, static_cast<size_t>(n));
}

// ---------------------------------------------------------------------------
// Vectored Exception Handler —— 抢在其他 handler 之前拿到原始异常
//
// 注意: 这是"第一个被调用"的 handler (AddVectoredExceptionHandler 的第一个
// 参数为 1 时, 后注册的先调用…实际上 Windows 是按注册顺序的逆序调用, 但
// 无论顺序如何, VEH 一定早于任何 __try/SEH/进程级 filter)。所以哪怕
// LocalRadio 的报告器坏了, 我们也已经先把原始数据写进文件了。
// ---------------------------------------------------------------------------
LONG WINAPI on_exception(EXCEPTION_POINTERS* ep) {
    // 防重入: handler 自己再炸就不要再进来了。
    if (InterlockedExchange(&g_in_handler, 1) != 0) return EXCEPTION_CONTINUE_SEARCH;

    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const CONTEXT* ctx = ep->ContextRecord;

    char buf[512];
    vnote("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    vnote("!!! 异常: code=0x%08lX  flags=0x%lX  addr=%p",
          er->ExceptionCode, er->ExceptionFlags,
          reinterpret_cast<void*>(er->ExceptionAddress));
    vnote("!!! 归属: %s", module_of(reinterpret_cast<uintptr_t>(er->ExceptionAddress),
                                    buf, sizeof(buf)));
    vnote("!!! 线程: tid=%lu", GetCurrentThreadId());

    // 常见 code 的中文说明, 省得再去查表
    switch (er->ExceptionCode) {
        case EXCEPTION_ACCESS_VIOLATION:
            vnote("!!! = ACCESS_VIOLATION: 试图 %s 地址 %p",
                  (er->ExceptionInformation[0] == 0 ? "读取" :
                   er->ExceptionInformation[0] == 1 ? "写入" : "执行"),
                  reinterpret_cast<void*>(er->ExceptionInformation[1]));
            break;
        case EXCEPTION_STACK_OVERFLOW:
            vnote("!!! = STACK_OVERFLOW: 栈溢出 (通常是无限递归或超大栈帧)");
            break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            vnote("!!! = ILLEGAL_INSTRUCTION: CPU 在'执行'非代码数据");
            break;
        case EXCEPTION_PRIV_INSTRUCTION:
            vnote("!!! = PRIV_INSTRUCTION");
            break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            vnote("!!! = DIVIDE_BY_ZERO");
            break;
        case STATUS_HEAP_CORRUPTION:
            vnote("!!! = HEAP_CORRUPTION: 堆被踩坏");
            break;
        default:
            vnote("!!! = (未分类) 参数个数=%lu", er->NumberParameters);
            break;
    }
    if (er->NumberParameters >= 2) {
        for (DWORD i = 0; i < er->NumberParameters && i < 4; ++i) {
            vnote("!!!   info[%lu] = 0x%llX", i,
                  static_cast<unsigned long long>(er->ExceptionInformation[i]));
        }
    }

    if (ctx) {
        vnote("!!! RIP=%p  RSP=%p  RBP=%p",
              reinterpret_cast<void*>(ctx->Rip),
              reinterpret_cast<void*>(ctx->Rsp),
              reinterpret_cast<void*>(ctx->Rbp));
        vnote("!!! RIP 归属: %s",
              module_of(static_cast<uintptr_t>(ctx->Rip), buf, sizeof(buf)));
        vnote("!!! RAX=%p RBX=%p RCX=%p RDX=%p",
              reinterpret_cast<void*>(ctx->Rax), reinterpret_cast<void*>(ctx->Rbx),
              reinterpret_cast<void*>(ctx->Rcx), reinterpret_cast<void*>(ctx->Rdx));
        vnote("!!! RSI=%p RDI=%p R8 =%p R9 =%p",
              reinterpret_cast<void*>(ctx->Rsi), reinterpret_cast<void*>(ctx->Rdi),
              reinterpret_cast<void*>(ctx->R8),  reinterpret_cast<void*>(ctx->R9));

        // 把返回地址链上最前面的十几条倒出来 —— 不看符号也能看出"谁调了谁"。
        vnote("!!! 返回地址链 (从 RSP 往上扫, 只留落在已知模块里的):");
        const uintptr_t rsp = static_cast<uintptr_t>(ctx->Rsp);
        int shown = 0;
        for (int i = 0; i < 192 && shown < 20; ++i) {
            uintptr_t v = 0;
            __try {
                v = *reinterpret_cast<const uintptr_t*>(rsp + i * 8);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                break;
            }
            if (!v) continue;
            const char* m = module_of(v, buf, sizeof(buf));
            if (strcmp(m, "<unknown>") == 0) continue;
            vnote("!!!   [rsp+0x%03X] %p  %s", i * 8,
                  reinterpret_cast<void*>(v), buf);
            ++shown;
        }
        if (!shown) vnote("!!!   (栈上没有落在已知模块里的返回地址)");
    }

    vnote("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    InterlockedExchange(&g_in_handler, 0);

    // 交还给系统: 不吞异常, 让 UE4SS/LocalRadio 的处理器照常跑 (我们只要数据)
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void bisect_install_veh() {
    if (InterlockedCompareExchange(&g_veh_installed, 1, 0) != 0) return;
    // 参数 1 = 放在链首 (最先被调用)
    AddVectoredExceptionHandler(1, &on_exception);
}

void bisect_init() {
    bisect_install_veh();

    if (g_file) return;

    // 日志放三个候选位置, 取第一个能写的:
    //   1) 本 DLL 所在目录 (通常就是 mods\DGLab_IDAC\dlls\)
    //   2) %TEMP%
    //   3) 当前工作目录
    char path[MAX_PATH]{};

    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&bisect_init), &self)) {
        wchar_t wdir[MAX_PATH]{};
        if (GetModuleFileNameW(self, wdir, MAX_PATH)) {
            if (wchar_t* slash = wcsrchr(wdir, L'\\')) *slash = L'\0';
            wchar_t wpath[MAX_PATH]{};
            _snwprintf_s(wpath, _TRUNCATE, L"%s\\bisect.log", wdir);
            if (_wfopen_s(&g_file, wpath, L"wb") == 0 && g_file) {
                // 转成窄字符只是给日志里显示用; 打开文件本身走的是宽字符路径,
                // 所以中文路径也能正常写。
                size_t conv = 0;
                wcstombs_s(&conv, path, sizeof(path), wpath, _TRUNCATE);
                strncpy_s(g_path, path, _TRUNCATE);
            }
        }
    }
    if (!g_file) {
        DWORD n = GetTempPathA(MAX_PATH, path);
        if (n) {
            strncat_s(path, "dglab_bisect.log", _TRUNCATE);
            if (fopen_s(&g_file, path, "wb") == 0 && g_file) {
                strncpy_s(g_path, path, _TRUNCATE);
            }
        }
    }
    if (!g_file) {
        if (fopen_s(&g_file, "dglab_bisect.log", "wb") == 0 && g_file) {
            strncpy_s(g_path, "dglab_bisect.log", _TRUNCATE);
        }
    }
    if (g_file) {
        SYSTEM_INFO si{};
        GetNativeSystemInfo(&si);
        char line[512];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "==== bisect logger 启动  pid=%lu pageSize=%lu ====\n",
                    GetCurrentProcessId(), si.dwPageSize);
        raw_write(line, strlen(line));

        // 把此刻已加载的模块记下来, 好把崩溃地址翻译成 "模块+偏移"。
        HMODULE mods[256]{};
        DWORD needed = 0;
        if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
            const DWORD count = needed / sizeof(HMODULE);
            for (DWORD i = 0; i < count && i < 200; ++i) {
                char name[MAX_PATH]{};
                if (!GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, MAX_PATH)) continue;
                // 只记我们关心的几个, 否则一行行太吵
                if (_stricmp(name, "GameProject-Win64-Shipping.exe") == 0 ||
                    _stricmp(name, "UE4SS.dll") == 0 ||
                    _stricmp(name, "main.dll") == 0 ||
                    _stricmp(name, "IndRun.dll") == 0 ||
                    _stricmp(name, "dwmapi.dll") == 0 ||
                    _stricmp(name, "MSVCP140.dll") == 0 ||
                    _stricmp(name, "VCRUNTIME140.dll") == 0 ||
                    _stricmp(name, "VCRUNTIME140_1.dll") == 0 ||
                    _stricmp(name, "KERNEL32.DLL") == 0) {
                    MODULEINFO mi{};
                    if (GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi))) {
                        add_module(name, reinterpret_cast<uintptr_t>(mi.lpBaseOfDll),
                                   static_cast<size_t>(mi.SizeOfImage));
                        _snprintf_s(line, sizeof(line), _TRUNCATE,
                                    "     模块 %-30s base=%p size=0x%lX\n",
                                    name, mi.lpBaseOfDll, mi.SizeOfImage);
                        raw_write(line, strlen(line));
                    }
                }
            }
        }
        static const char kDone[] = "==== 模块表记录完毕 ====\n";
        raw_write(kDone, sizeof(kDone) - 1);
    }
}

void bisect_note(const char* fmt, ...) {
    if (!g_file) return;
    char body[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    char line[2240];
    const int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
                              "[%10lu] [t%lu] %s\n",
                              GetTickCount(), GetCurrentThreadId(), body);
    if (n > 0) raw_write(line, static_cast<size_t>(n));
}

const char* bisect_log_path() { return g_path; }

void bisect_shutdown() {
    if (g_file) {
        static const char kDone[] = "==== bisect 正常关闭 ====\n";
        raw_write(kDone, sizeof(kDone) - 1);
        fclose(g_file);
        g_file = nullptr;
    }
}

}  // namespace dglab_idac
