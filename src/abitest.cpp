// ---------------------------------------------------------------------------
// abitest — 最小 ABI 探针 mod
//
// 目的: 把"我们的代码有问题"和"UE4SS 版本/ABI 不匹配"这两件事彻底分开。
//
// 这个 mod:
//   * 与 DGLab_IDAC 用**完全相同**的编译设置 / 同一份 UE4SS 头文件 /
//     同一个导入库 / 同一个 CRT(/MD Release)。
//   * 生命周期函数全部为空 —— 不读文件、不起线程、不调 UE4SS 的日志、
//     不挂钩、不分配任何我们自己管理的内存。
//   * 构造函数只做基类默认构造, 一个字都不写。
//
// 判读方法:
//   * 装上它游戏仍崩  -> 与我们的业务代码无关, 是 **UE4SS 头文件/二进制
//                        版本不匹配**(ABI 层面)。解决办法是换 UE4SS 版本
//                        并重新生成导入库 + 重编。
//   * 装上它游戏正常  -> ABI 没问题, 是我们的某个具体动作触发的, 继续二分。
//
// 单独的 DLL 名(abitest.dll), 目的是**证明两个 C++ mod 能否共存** ——
// 这本身就是一个独立假设: UE4SS 的 mod 注册表里是否会因为文件名/模块名
// 相同而互相踩踏。
// ---------------------------------------------------------------------------
#include <Mod/CppUserModBase.hpp>

#include <windows.h>

namespace {

// 直接在 DllMain 里写一行文件, 确认 DLL 确实被加载了 (不依赖 UE4SS 日志)。
void write_trace(const char* text) {
    HMODULE self = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&write_trace), &self)) {
        return;
    }
    wchar_t path[MAX_PATH]{};
    if (!::GetModuleFileNameW(self, path, MAX_PATH)) return;
    if (wchar_t* slash = wcsrchr(path, L'\\')) *(slash + 1) = L'\0';
    wcscat_s(path, L"abitest.log");

    FILE* f = nullptr;
    if (::_wfopen_s(&f, path, L"ab") != 0 || !f) return;
    fputs(text, f);
    fputc('\n', f);
    fflush(f);
    fclose(f);
}

}  // namespace

namespace dglab_idac {

class AbiTestMod final : public RC::CppUserModBase {
public:
    AbiTestMod() {
        // ★ 刻意一个字都不写。
        //   连 ModName = STR("...") 都不做 —— 基类里那 5 个 std::wstring
        //   成员是"跨 DLL 边界写内存"的最小测试点, 我们把它留空, 好让
        //   与 DGLab_IDAC 的差异只剩"我们的代码"这一个变量。
        //   (如果这里写 ModName 就崩, 说明连基类布局都对不上。)
        write_trace("[abitest] ctor 进入 (基类已构造, 我们没写任何成员)");
    }

    ~AbiTestMod() override {
        write_trace("[abitest] dtor");
    }

    auto on_update() -> void override {
        // 什么都不做。故意不调 UE4SS 的日志。
    }

    auto on_unreal_init() -> void override {}
    auto on_program_start() -> void override {}
    auto on_dll_load(std::wstring_view) -> void override {}
};

}  // namespace dglab_idac

#define ABITEST_API __declspec(dllexport)
extern "C" {
ABITEST_API RC::CppUserModBase* start_mod() {
    write_trace("[abitest] start_mod 被调用, 即将 new");
    auto* mod = new dglab_idac::AbiTestMod();
    write_trace("[abitest] start_mod 返回");
    return mod;
}

ABITEST_API void uninstall_mod(RC::CppUserModBase* mod) {
    write_trace("[abitest] uninstall_mod");
    delete mod;
}
}

// DllMain —— 最早能跑我们代码的地方。用来区分"DLL 根本没被加载"和
// "加载了但 UE4SS 调 start_mod 时崩"。
BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        write_trace("[abitest] DllMain PROCESS_ATTACH");
    } else if (reason == DLL_PROCESS_DETACH) {
        write_trace("[abitest] DllMain PROCESS_DETACH");
    }
    return TRUE;
}
