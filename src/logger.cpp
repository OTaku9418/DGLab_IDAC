// ---------------------------------------------------------------------------
// DGLab_IDAC — 日志实现
// ---------------------------------------------------------------------------
#include "logger.hpp"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace dglab_idac {

namespace {

std::mutex           g_mutex;
FILE*                g_file = nullptr;
std::string          g_path;
std::atomic<bool>    g_debug_output{true};

// 取本 DLL 所在目录 —— 日志跟着 DLL 走, 不依赖游戏的工作目录。
std::string module_dir() {
    HMODULE self = nullptr;
    if (!::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&module_dir), &self)) {
        return {};
    }
    wchar_t buf[MAX_PATH]{};
    DWORD n = ::GetModuleFileNameW(self, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring w(buf, n);
    const auto slash = w.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    w.resize(slash);
    // 窄化: 只用于 fopen, 路径里出现非 ANSI 字符的概率极低
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back(static_cast<char>(c & 0x7F));
    return s;
}

void emit(const char* tag, const char* fmt, va_list ap) {
    char body[2048];
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);

    const DWORD tid = ::GetCurrentThreadId();

    char line[2304];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[DGLab_IDAC]%s[t%lu] %s\n",
                tag ? tag : " ", static_cast<unsigned long>(tid), body);

    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_file) {
            std::fputs(line, g_file);
            // 探针阶段可能被强杀, 所以每条都 flush, 不能靠退出时的清理
            std::fflush(g_file);
        }
    }

    if (g_debug_output.load(std::memory_order_relaxed)) {
        ::OutputDebugStringA(line);
    }
    std::fputs(line, stdout);
    std::fflush(stdout);
}

}  // namespace

std::string log_init(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_file) return g_path;

    std::string p = path;
    if (p.empty()) {
        const std::string dir = module_dir();
        p = dir.empty() ? std::string("DGLab_IDAC.log") : dir + "\\DGLab_IDAC.log";
    }

    // 注意: fopen_s / _snprintf_s 是 MSVC 的 CRT 扩展, 声明在全局命名空间,
    // **不在** std 里 —— 写 std::fopen_s 会 C2039。
    if (::fopen_s(&g_file, p.c_str(), "wb") != 0 || !g_file) {
        g_file = nullptr;
        g_path.clear();
        ::OutputDebugStringA("[DGLab_IDAC] !! 无法打开日志文件, 只输出到调试器\n");
        return {};
    }
    g_path = p;
    return g_path;
}

void log_shutdown() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_file) {
        std::fputs("[DGLab_IDAC] ==== log closed ====\n", g_file);
        std::fclose(g_file);
        g_file = nullptr;
    }
}

void log_line(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit(" ", fmt, ap);
    va_end(ap);
}

void log_info(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit(" INFO ", fmt, ap);
    va_end(ap);
}

void log_warn(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit(" WARN ", fmt, ap);
    va_end(ap);
}

void log_error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit("ERROR ", fmt, ap);
    va_end(ap);
}

void log_set_debug_output(bool on) { g_debug_output.store(on, std::memory_order_relaxed); }

const char* log_path() { return g_path.c_str(); }

}  // namespace dglab_idac
