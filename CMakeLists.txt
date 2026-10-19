# ---------------------------------------------------------------------------
# DGLab_IDAC — 力反馈 -> 郊狼(DG-LAB) 波形输出 / UE4SS C++ mod
#
# 关键构建约束(全部来自实测):
#   * 必须 x64 + Release。UE4SS.dll 是 Game__Shipping__Win64 (MSVC)。
#   * 必须 /MD (MultiThreadedDLL)。UE4SS.dll 导入 msvcp140/vcruntime140,
#     且它自己是 Release 构建 —— Debug CRT 或 /MT 会跨 DLL 破坏堆。
#   * 不要用 add_subdirectory(RE-UE4SS)。那条路会连 UE4SS 本体一起重编
#     (glaze / ImGui / zydis / corrosion 全部 FetchContent, 还要 Rust 工具链),
#     而游戏目录里已经有一个能用的 UE4SS.dll。
#     我们改为**直接链接那个 DLL** —— MSVC 的 link.exe 能直接对 DLL 建导入库。
#     顺带的好处: 链接的就是实际在跑的那个二进制, 不会版本错配。
#
# 配置与构建(E: 盘装了 MSVC 14.44 + Windows SDK 10.0.26100):
#   tools\build.cmd                  —— 推荐, 内部自动 call vcvars64.bat
#   tools\build.cmd -Deploy          —— 额外把产物铺到游戏 Mods 目录
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.18)
project(DGLab_IDAC VERSION 0.2.1 LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "只能构建 x64 —— 游戏是 64 位进程。")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 单配置生成器时强制 Release。Visual Studio 生成器是多配置的, 交给命令行。
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# 依赖 1: MinHook —— 自带 hook 引擎。UE4SS 不导出它内部的 PolyHook2, 所以
# 一个 inline hook 都要自己带。third_party/minhook 是 git submodule。
# ---------------------------------------------------------------------------
set(MINHOOK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook")
if(NOT EXISTS "${MINHOOK_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "找不到 MinHook。执行:\n"
        "  git submodule update --init --recursive\n"
        "或把 MinHook 源码放到 ${MINHOOK_DIR}")
endif()
set(MINHOOK_BUILD_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
set(MINHOOK_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
add_subdirectory("${MINHOOK_DIR}" EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# 依赖 2: UE4SS 头文件 + 已构建的 UE4SS.dll
#
# 真正用于编译的是下面 DGLAB_HEADERS_PINNED 指向的 _ue4ss_ref/ue4ss_0196ef29
# (已提交进仓库, 与游戏里的 UE4SS.dll 逐提交对齐)。
#
# UE4SS_DIR 只是**回退**方案: 指向一份旧的 v3.0.1 检出, 只在没有对齐头文件
# 时用, 且 ABI 很可能对不上。tools/bootstrap.cmd 会检查对齐头文件是否在位,
# 但它**不会**去检出 RE-UE4SS —— 这个目录在 .gitignore 里, 新 clone 里不存在,
# 所以这里不能因为它缺席就 FATAL(否则"clone 下来直接构建"必然失败)。
# ---------------------------------------------------------------------------
set(UE4SS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/_ue4ss_ref/RE-UE4SS" CACHE PATH "RE-UE4SS 源码根(可选回退)")

# 已构建的 UE4SS.dll。链接它而不是重编它。
#
# ⚠ 路径一律用正斜杠 —— CMake 里反斜杠是转义字符, "D:\INDAC\..." 里的 \I 会被吞掉。
#
# 解析顺序(先到先用):
#   1. -DUE4SS_DLL=<绝对路径>          直接指定, 最高优先级
#   2. -DDGLAB_DEPLOY_DIR=<...\Mods\DGLab_IDAC>   往上两级就是 ue4ss 目录
#   3. -DDGLAB_GAME_UE4SS_DIR=<...\ue4ss>
#   4. 下面那个默认值 —— 只是开发机上的路径, 别的机器上一定不对
set(DGLAB_GAME_UE4SS_DIR ""
    CACHE PATH "游戏的 ue4ss 目录(含 UE4SS.dll), 用于推导 UE4SS_DLL")

set(UE4SS_DLL "" CACHE FILEPATH "游戏目录里的 UE4SS.dll(留空则自动推导)")

# 有人显式给了线索却没找到, 就别再假装用那个默认值 —— 那会让日志里出现一条
# 与本机无关的 D:\INDAC 路径, 看起来"找到了"其实什么都没验证(排查 ABI 问题时
# 这种假信息最毒)。所以: 给过线索就以那条线索为准, 找不到就明说, 不往下走。
set(_hinted FALSE)
if(NOT UE4SS_DLL)
    if(DGLAB_DEPLOY_DIR)
        set(_hinted TRUE)
        cmake_path(GET DGLAB_DEPLOY_DIR PARENT_PATH _mods_dir)
        cmake_path(GET _mods_dir PARENT_PATH _ue4ss_dir)
        set(_cand "${_ue4ss_dir}/UE4SS.dll")
        if(EXISTS "${_cand}")
            set(UE4SS_DLL "${_cand}")
        else()
            message(WARNING
                "DGLAB_DEPLOY_DIR 指向 ${DGLAB_DEPLOY_DIR}, 由此推出的\n"
                "  UE4SS.dll 不存在: ${_cand}\n"
                "  没关系 —— 链接用的是 third_party/importlib/UE4SS.lib,\n"
                "  不需要 UE4SS.dll。只是不能再帮你核对版本了。")
        endif()
    endif()
endif()
if(NOT UE4SS_DLL AND NOT _hinted AND DGLAB_GAME_UE4SS_DIR)
    set(_hinted TRUE)
    set(_cand "${DGLAB_GAME_UE4SS_DIR}/UE4SS.dll")
    if(EXISTS "${_cand}")
        set(UE4SS_DLL "${_cand}")
    else()
        message(WARNING "DGLAB_GAME_UE4SS_DIR 下没有 UE4SS.dll: ${_cand}")
    endif()
endif()
if(NOT UE4SS_DLL AND NOT _hinted)
    # 开发机默认值。别人拿到这个仓库时上面几条都没给, 会落到这里; 路径不存在
    # 就直接留空, 什么都不报 —— 这是预期情况, 不是错误。
    set(_dev_default
        "D:/INDAC/MODAC321/WindowsNoEditor/GameProject/Binaries/Win64/ue4ss/UE4SS.dll")
    if(EXISTS "${_dev_default}")
        set(UE4SS_DLL "${_dev_default}")
    endif()
endif()

if(NOT EXISTS "${UE4SS_DLL}")
    # 注意: 这里**不是** FATAL。链接用的是 third_party/importlib/UE4SS.lib,
    # UE4SS.dll 只在"重新生成导入库"时才需要。
    # 连那个导入库也不必提交二进制: 仓库里带着 UE4SS.def, tools\bootstrap.cmd
    # 用 lib.exe 直接吃 .def 就能生成 .lib —— 所以"没装游戏也能编译"。
    # 真的什么导入库都没有时, 下面链接段会给出能照着做的指令。
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/importlib/UE4SS.lib")
        message(STATUS "DGLab_IDAC: 未找到 UE4SS.dll, 但导入库已存在 —— 编译不受影响")
    else()
        message(STATUS "DGLab_IDAC: 未找到 UE4SS.dll 与导入库 —— 先跑 tools\\bootstrap.cmd")
    endif()
endif()
if(UE4SS_DLL)
    message(STATUS "DGLab_IDAC: UE4SS.dll = ${UE4SS_DLL}")
else()
    message(STATUS "DGLab_IDAC: UE4SS.dll = (未找到, 编译只用导入库)")
endif()

# ---------------------------------------------------------------------------
# mod 本体
# ---------------------------------------------------------------------------
# ⚠ ble_winrt.cpp 占用同一个符号 create_ble_transport(), 所以两者互斥。
# 优先用真正的 WinRT 实现; 它还没落地时退回到返回 nullptr 的占位实现,
# 这样即使传输层没写完, hook + 管线也能先编译、先验证。
set(DGLAB_BLE_SRC "src/transport/ble_winrt_stub.cpp")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/transport/ble_winrt.cpp")
    set(DGLAB_BLE_SRC "src/transport/ble_winrt.cpp")
    message(STATUS "DGLab_IDAC: BLE 传输 = 真实 WinRT 实现")
else()
    message(WARNING "DGLab_IDAC: src/transport/ble_winrt.cpp 不存在, 使用占位实现(只能干跑)")
endif()

# ---------------------------------------------------------------------------
# ★ 诊断构建 ★  -DDGLAB_DIAGNOSTIC=ON
#
# 单独一个“除了写日志什么都不做”的 mod，用来把游戏启动崩溃夹逼到单条语句:
# 它不做 hook、不建工作线程、不解析配置, 只在每一步前面写一行
# (CreateFileW + WriteFile + FlushFileBuffers, 崩溃也不丢尾巴)。
# 与本体互斥 —— 两者都实现 start_mod/uninstall_mod, 同时编会重复定义。
# ---------------------------------------------------------------------------
option(DGLAB_DIAGNOSTIC "只构建诊断版 mod(逐句检查点日志, 无任何功能)" OFF)

if(DGLAB_DIAGNOSTIC)
    set(DGLAB_SOURCES src/diag_mod.cpp)
    message(WARNING "DGLab_IDAC: ★ 诊断构建 —— 产物不挂钩、不输出波形, 只写 diag.log")
else()
    set(DGLAB_SOURCES
        src/dllmain.cpp
        src/bisect_logger.cpp
        src/logger.cpp
        src/config.cpp
        src/pipeline.cpp
        src/source/indrun_hook_source.cpp
        src/transport/transport.cpp
        src/transport/transport_factory.cpp
        src/transport/null_transport.cpp
        ${DGLAB_BLE_SRC}
    )
endif()

# ---------------------------------------------------------------------------
# fmt 实现 —— 必须编进来
#
# DynamicOutput/Output.hpp 里的 Output::send 是模板, 会在**我们的** TU 里
# 展开成对 fmt::vformat / dragonbox 等非模板符号的调用。UE4SS.dll 静态链了
# fmt 但**没有导出**它, 所以这些符号在我们这边无法解析:
#     LNK2019: fmt::v11::report_error / dragonbox::to_decimal<float> 等 8 个
# 因此把 fmt 11.2.0 的 format.cc + os.cc 直接编进本 DLL。
#
# ABI 一致性: UE4SS 用 fmt 11.2.0 (deps/third/CMakeLists.txt 的 FetchContent
# GIT_TAG), 我们这里也是 11.2.0 —— 版本必须一致, 因为 `receive()` 的
# std::string/std::wstring 参数跨了 DLL 边界, 而 fmt 对象是在我们这边构造、
# 进到 UE4SS 的装置里被消费的。
# ---------------------------------------------------------------------------
set(DGLAB_FMT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/_ue4ss_ref/third_party/fmt")
if(NOT EXISTS "${DGLAB_FMT_DIR}/src/format.cc" OR NOT EXISTS "${DGLAB_FMT_DIR}/src/os.cc")
    # 必须 FATAL。以前写成 if(IS_DIRECTORY ...) 只 list(APPEND), 文件夹不在时
    # 一句都不报, 一路编到 DynamicOutput/Output.hpp 才报
    #     fatal error C1083: 无法打开包含文件: "fmt/core.h"
    # 把真正原因(这个目录在干净 clone 里被 .gitignore 排掉了)藏到后面。
    message(FATAL_ERROR
        "缺少 fmt 11.2.0 源文件: ${DGLAB_FMT_DIR}/src/{format.cc,os.cc}\n"
        "  它已提交进仓库(见该目录下的 PROVENANCE.md)。恢复:\n"
        "      git checkout -- _ue4ss_ref/third_party/fmt\n"
        "  不能省: DynamicOutput 的头会 #include <fmt/core.h>, 而 UE4SS.dll 静态\n"
        "  链的 fmt 没有导出, 版本还必须与 0196ef29 的 11.2.0 完全一致。")
endif()
list(APPEND DGLAB_SOURCES
    "${DGLAB_FMT_DIR}/src/format.cc"
    "${DGLAB_FMT_DIR}/src/os.cc")

add_library(${PROJECT_NAME} SHARED ${DGLAB_SOURCES})

# ===========================================================================
# ★★★ UE4SS 头文件必须与目标 UE4SS.dll **逐 commit 对齐** ★★★
#
# 为什么 (踩过的坑, 别重犯):
#   本机装的 UE4SS.dll 自报 `v3.0.1 Beta #0 - Git SHA #0196ef29`, 对应官方
#   experimental 分支持久化下来的
#       UE4SS_v3.0.1-944-g0196ef29.zip   (commit 0196ef294f852)
#   它不是 tag v3.0.1 (d935b5b, 2024-02-14), 两者相差 944 个提交。
#
#   一开始我们拿 v3.0.1 的头文件编, 虚表 ABI 完全对不上 —— 现象是
#   `on_program_start` 竟然跑起来了, 函数返回后 15 ms 在 UE4SS 自己的
#   mod 遍历里 `call qword ptr [rax+0x78]` 跳到垃圾地址 0x6575 崩掉。
#   原因: 0196ef29 在 on_unreal_init 与 on_program_start 之间插入了
#   on_ui_init, 且在末尾还有 4 个新的 on_lua_start/stop(Lua*) 重载,
#   整个虚表从 on_program_start 起全体错位。
#
#   后来我又手工写过一版 shim "猜"顺序, 仍然崩 —— 因为真实顺序是
#       on_update / on_unreal_init / on_ui_init / on_program_start /
#       on_lua_start(弃用,vector&) x2 / on_lua_stop(弃用,vector&) x2 /
#       on_dll_load / render_tab /
#       on_lua_start(Lua*) x2 / on_lua_stop(Lua*) x2 / on_cpp_mods_loaded
#   光靠导出序号是推不出来的。**结论: 不要猜, 直接逐个 commit 对齐头文件。**
#
# 因此: 默认优先使用 _ue4ss_ref/ue4ss_0196ef29 (从 0196ef29 检出固化的真头文件),
#       回退到 UE4SS_DIR (旧的 v3.0.1 参考检出, 只在不关心 ABI 时用)。
# 升级 UE4SS 时: 换掉 ue4ss_0196ef29/ 与 ue4ss_0196ef29_deps/, 重新生成导入库, 重编。
# ===========================================================================
set(DGLAB_HEADERS_PINNED
    "${CMAKE_CURRENT_SOURCE_DIR}/_ue4ss_ref/ue4ss_0196ef29")
set(DGLAB_DEPS_PINNED
    "${CMAKE_CURRENT_SOURCE_DIR}/_ue4ss_ref/ue4ss_0196ef29_deps")

if(IS_DIRECTORY "${DGLAB_HEADERS_PINNED}")
    set(DGLAB_UE4SS_INCLUDES "${DGLAB_HEADERS_PINNED}")
    file(GLOB DGLAB_DEP_INCLUDE_DIRS LIST_DIRECTORIES true
        "${DGLAB_DEPS_PINNED}/*")
    message(STATUS "DGLab_IDAC: 使用★已对齐★的 UE4SS 头文件 = ${DGLAB_HEADERS_PINNED}")
else()
    # 回退路径: 没有对齐头文件, 只能用 UE4SS_DIR 那份旧检出。
    # 这里才是"RE-UE4SS 到底在不在"该检查的地方 —— 放在文件开头会让
    # 干净 clone(该目录被 .gitignore 排除)连配置都过不去。
    if(NOT EXISTS "${UE4SS_DIR}/UE4SS/include/Mod/CppUserModBase.hpp")
        message(FATAL_ERROR
            "既没有对齐的头文件 (${DGLAB_HEADERS_PINNED}), 也没有回退用的 RE-UE4SS 检出 "
            "(${UE4SS_DIR}/UE4SS/include)。\n"
            "  正常情况下不需要回退: 对齐头文件已随仓库提交, 用\n"
            "      git checkout -- _ue4ss_ref\n"
            "  取回来即可。tools\\bootstrap.cmd 会检查它是否在位。\n"
            "  回退用的 RE-UE4SS 是 v3.0.1 (commit d935b5b), 与本机 UE4SS.dll\n"
            "  (v3.0.1 Beta #0196ef29) 相差 944 个提交, vtable 布局不同 ——\n"
            "  真拿它编, mod 会在 UE4SS 调用虚函数时崩, 所以只在不关心 ABI 时用。")
    endif()
    set(DGLAB_UE4SS_INCLUDES "${UE4SS_DIR}/UE4SS/include")
    file(GLOB DGLAB_DEP_INCLUDE_DIRS LIST_DIRECTORIES true
        "${UE4SS_DIR}/deps/first/*/include")
    message(WARNING
        "DGLab_IDAC: 未找到对齐的头文件 (${DGLAB_HEADERS_PINNED}), "
        "回退到 ${UE4SS_DIR} —— 若不是刻意为之, ABI 很可能对不上!")
endif()

# UE4SS 是多个独立 CMake 子工程拼起来的, 每个子工程的头文件在
# <dep>/include/<Dep>/... 下, 所以 include 根必须是各子工程的 include/ 目录
# 本身, 而不是 deps/first。少一个就会报 “无法打开包括文件: File/Macros.hpp”。
foreach(_inc IN LISTS DGLAB_DEP_INCLUDE_DIRS)
    if(IS_DIRECTORY "${_inc}")
        list(APPEND DGLAB_UE4SS_INCLUDES "${_inc}")
    endif()
endforeach()

# fmt —— DynamicOutput/Output.hpp 会 #include <fmt/core.h>.
# 版本必须与 UE4SS 在 0196ef29 用的完全一致: fmt 11.2.0 (见 deps/third/CMakeLists.txt
# 里的 FetchContent GIT_TAG)。UE4SS 会给它打一个 "check 宏冲突" 补丁
# (cmake/modules/MacroPatch.cmake -> fmt/ranges.h), 我们已经在固化的头文件里打过。
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/_ue4ss_ref/third_party/fmt/include/fmt/core.h")
    # 同样必须 FATAL —— 否则报的是一句 C1083, 看不出是"仓库里的 fmt 被排掉了"。
    message(FATAL_ERROR
        "缺少 fmt 头文件: _ue4ss_ref/third_party/fmt/include/fmt/core.h\n"
        "  恢复:  git checkout -- _ue4ss_ref/third_party/fmt")
endif()
list(APPEND DGLAB_UE4SS_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/_ue4ss_ref/third_party/fmt/include")

list(REMOVE_DUPLICATES DGLAB_UE4SS_INCLUDES)

target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/shim"   # 必须排在 UE4SS 之前, 见 shim/GUI/GUITab.hpp
    ${DGLAB_UE4SS_INCLUDES}
    "${MINHOOK_DIR}/include"
)

# CRT 必须与 UE4SS.dll 一致
set_property(TARGET ${PROJECT_NAME} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /utf-8            # 源码里有中文注释, 不加会按 ANSI 解析报 C4819
        /W4
        /permissive-
        /Zc:__cplusplus
        /EHsc
        /MP
        /wd4100           # 未引用的形参(detour 里刻意占位)
        /Zi               # 生成 PDB: 崩溃报告(bisect.log)里的地址要能查符号
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
        # 版本号只有一个来源: 上面 project(... VERSION ...)。
        # 代码里不许再写版本字面量 —— 以前 dllmain.cpp 有 3 处 "0.1.0",
        # 改版本时漏一处就会出现"日志说 0.1.1、ModVersion 说 0.1.0"。
        # 两个变体的原因: UE4SS 的 StringType 是 std::basic_string<CharType>,
        # 而 CharType 由 StringType.hpp 里的 FORCE_U16 决定 —— 当前构建没定义它,
        # 所以是 wchar_t, 前缀必须是 L(不是 u)。对了错都只是编译错误, 不会静默。
        "DGLAB_VERSION_W=L\"${PROJECT_VERSION}\""
        "DGLAB_VERSION_A=\"${PROJECT_VERSION}\""
        "DGLAB_VERSION_DIAG=L\"${PROJECT_VERSION}-diag\""
    )
endif()

# ---------------------------------------------------------------------------
# 链接
#
# ⚠ 不能直接把 UE4SS.dll 交给 link.exe —— 会报
#     fatal error LNK1107: 文件无效或损坏: 无法在 0x330 处读取
#   link.exe 只接受 .lib/.obj, 不认得 PE 的 DLL 导出表。
# 正确做法是先用 tools\make_ue4ss_implib.cmd 生成导入库:
#     dumpbin /exports UE4SS.dll -> UE4SS.def -> lib /def /machine:x64 -> UE4SS.lib
# 那个脚本对任意 UE4SS.dll 都可重跑(换游戏版本/UE4SS 版本时重跑一次即可)。
#
# 我们真正需要它导入的符号很少: CppUserModBase 的 ctor/dtor, 以及那些我们
# **没有覆盖**的虚函数 —— 它们都是 inline 定义, 但 UE4SS 把它们也导出了,
# 编译器可能生成对 UE4SS.dll 的引用。实测导出表里有
#   ??0CppUserModBase@RC@@QEAA@XZ
#   ??1CppUserModBase@RC@@UEAA@XZ
#   ?on_update@... / ?on_unreal_init@... / ?on_program_start@... 等 (全部指向 001A4E50)
# ---------------------------------------------------------------------------
set(UE4SS_IMPLIB "${CMAKE_CURRENT_SOURCE_DIR}/third_party/importlib/UE4SS.lib")
if(NOT EXISTS "${UE4SS_IMPLIB}")
    message(FATAL_ERROR
        "缺少 UE4SS 导入库: ${UE4SS_IMPLIB}\n"
        "  跑一次:  tools\\bootstrap.cmd\n"
        "  它会用仓库里已提交的 third_party/importlib/UE4SS.def 生成 .lib ——\n"
        "  **不需要游戏安装, 也不需要 UE4SS.dll**。")
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    minhook
    "${UE4SS_IMPLIB}"
    windowsapp.lib      # WinRT 激活 (RoActivateInstance / IBluetoothLE*)
    runtimeobject.lib
    ole32.lib
    setupapi.lib
    bcrypt.lib
    psapi.lib           # EnumProcessModules / GetModuleBaseNameA (挂不上时倒模块清单)
    shell32.lib         # CommandLineToArgvW / 杂项
)

if(MSVC)
    # /DEBUG + /Zi: 让 make_release.cmd 能把 PDB 与 DLL 一起归档。
    # /Brepro: 不把当前时间写进 PE 头 —— 否则同样的源码每次构建出来的 DLL
    #   有 4 个字节不同(实测: 文件头 TimeDateStamp 与 CodeView 记录各一处),
    #   md5 就失去"验证同一份代码"的意义。
    # /PDBALTPATH: DLL 里只记文件名, 不把构建机路径(D:\INDAC\...)写进发布产物。
    # /OPT:REF /OPT:ICF 必须显式写出: 链接器在 /DEBUG 下默认关掉它们(即使 Release 配置),
    # 会把没被引用的函数全塞进镜像 —— 实测不加这两项, DLL 从 268 KB 涨到 1.18 MB。
    target_link_options(${PROJECT_NAME} PRIVATE
        /DEBUG /Brepro /OPT:REF /OPT:ICF "/PDBALTPATH:DGLab_IDAC.pdb")
endif()

# 输出名: UE4SS 只找 <mod>/dlls/main.dll, 所以交付时改名。
# 构建产物仍叫 DGLab_IDAC.dll 以免和别的 mod 的 main.dll 混淆。
set_target_properties(${PROJECT_NAME} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "DGLab_IDAC"
)

# 打印**实际生效**的那个头文件目录, 而不是回退目录 —— 以前这里打的是
# UE4SS_DIR, 于是日志里永远显示 v3.0.1 路径, 而实际编进去的是对齐头文件,
# 排查 ABI 问题时这个假信息非常致命。
list(GET DGLAB_UE4SS_INCLUDES 0 _dglab_first_inc)
message(STATUS "DGLab_IDAC: 生效的 UE4SS 头文件 = ${_dglab_first_inc}")
message(STATUS "DGLab_IDAC: 链接 UE4SS 导入库 = ${UE4SS_IMPLIB}")
if(UE4SS_DLL)
    message(STATUS "DGLab_IDAC: UE4SS.dll 版本参照 = ${UE4SS_DLL}")
else()
    message(STATUS "DGLab_IDAC: UE4SS.dll 版本参照 = (无 —— 只靠导入库, 不核对版本)")
endif()
message(STATUS "DGLab_IDAC: MinHook     = ${MINHOOK_DIR}")

# ---------------------------------------------------------------------------
# 部署: 铺成 UE4SS mod 的样子 (<mod>/dlls/main.dll + enabled.txt)
# ---------------------------------------------------------------------------
set(DGLAB_DEPLOY_DIR "" CACHE PATH "游戏 ue4ss/Mods/DGLab_IDAC 目录(留空则不自动部署)")
if(DGLAB_DEPLOY_DIR)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DGLAB_DEPLOY_DIR}/dlls"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:${PROJECT_NAME}>" "${DGLAB_DEPLOY_DIR}/dlls/main.dll"
        COMMAND ${CMAKE_COMMAND} -E touch "${DGLAB_DEPLOY_DIR}/enabled.txt"
        COMMENT "部署 -> ${DGLAB_DEPLOY_DIR}/dlls/main.dll"
    )
endif()

# ---------------------------------------------------------------------------
# abitest —— 最小 ABI 探针 mod (见 src/abitest.cpp 顶部的说明)
#
# 与主 target 共享**完全相同**的编译选项 / UE4SS 头文件 / 导入库 / CRT,
# 但源代码里一个 UE4SS 符号都不主动调用, 生命周期函数全空。
#
# 用途: 判定"游戏启动崩溃"到底是我们代码的问题, 还是 UE4SS 头文件与
# 已安装 UE4SS.dll 之间的 ABI 不匹配。
#
# 构建:  cmake --build build --config Release --target abitest
# 部署:  拷到 <ue4ss>\Mods\AbiTest\dlls\main.dll + 建空 enabled.txt
# ---------------------------------------------------------------------------
add_library(abitest SHARED src/abitest.cpp)

target_include_directories(abitest PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/shim"
    ${DGLAB_UE4SS_INCLUDES}
)

set_property(TARGET abitest PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
target_compile_options(abitest PRIVATE
    /utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc /MP /wd4100
)
target_compile_definitions(abitest PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
)

# 只链 UE4SS 导入库 —— 刻意不链 minhook / WinRT, 少一个变量。
target_link_libraries(abitest PRIVATE "${UE4SS_IMPLIB}")

set_target_properties(abitest PROPERTIES
    PREFIX ""
    OUTPUT_NAME "abitest"
)

# ---------------------------------------------------------------------------
# protocol_check —— B0/BF 编码自检 (普通控制台程序, 不进游戏)
#
# 存在的理由: StrengthMode 的枚举值曾经与官方文档**完全相反** ——
# Absolute 写成 0b00、Unchanged 写成 0b11。游戏正常运行、日志全绿、编码
# 自检也通过, 唯一的表现是"设备完全不动", 因为设备把"绝对设置"当成
# "不变"。这类错误只有逐位对着协议核对才能发现。
#
# 基准是官方文档的例子, 不是我们自己的实现 —— 详见 tools/protocol_check.cpp
# 顶部。官方文档按快照提交号引用, 链接与每份文件的 sha256 记在
# re/reference/dglab-bluetooth-protocol/PROVENANCE.md (仓库不再包含副本)。
#
# 构建:  cmake --build build --config Release --target protocol_check
# 运行:  build\Release\protocol_check.exe    (全通过返回 0)
# ---------------------------------------------------------------------------
set(DGLAB_PROTOCOL_SRC "src/transport/transport.cpp")
# 以前这里会静默回退到 src/transport/transport_stub.cpp —— 那个文件根本不存在,
# 所以回退分支只会在"transport.cpp 被改名"时把一个更隐蔽的错误丢给编译器。
# 缺文件就直接报错。
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${DGLAB_PROTOCOL_SRC}")
    message(FATAL_ERROR "缺少 ${DGLAB_PROTOCOL_SRC} —— protocol_check 需要它来验证 B0/BF 编码")
endif()

add_executable(protocol_check
    tools/protocol_check.cpp
    ${DGLAB_PROTOCOL_SRC}
    src/logger.cpp          # transport.cpp 里的 log_error/log_warn 要用
)

target_include_directories(protocol_check PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# 刻意不链 UE4SS / minhook / WinRT —— 这个工具必须能在任何机器上跑起来。
set_property(TARGET protocol_check PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
if(MSVC)
    target_compile_options(protocol_check PRIVATE
        /utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc /MP
    )
    target_compile_definitions(protocol_check PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
    )
endif()

# ---------------------------------------------------------------------------
# logic_check —— 信箱/看门狗/静默帧/管线的离线逻辑自检 (普通控制台程序)
#
# 存在的理由: 看门狗只在**出故障的时候**才生效, 平时跑一万次也看不出问题。
# 等真出事(游戏卡死 2 秒)才发现它没接上, 代价是设备在错误强度上一直输出。
# 所以判定逻辑必须能离线测 —— FrameMailbox 的时间由调用方传入, 不依赖真时钟。
#
# 与 protocol_check 的分工: protocol_check 管"字节编码对不对", logic_check
# 管"什么时候该发什么"。两者都不链 UE4SS / WinRT / minhook。
#
# 构建:  cmake --build build --config Release --target logic_check
# 运行:  build\Release\logic_check.exe    (全通过返回 0)
# ---------------------------------------------------------------------------
add_executable(logic_check
    tools/logic_check.cpp
    ${DGLAB_PROTOCOL_SRC}
    src/pipeline.cpp
    src/config.cpp          # is_held_risk_source() 在这里, 告警名单也要被离线覆盖
    src/logger.cpp
)

target_include_directories(logic_check PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

set_property(TARGET logic_check PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
if(MSVC)
    target_compile_options(logic_check PRIVATE
        /utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc /MP /wd4100
    )
    target_compile_definitions(logic_check PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
    )
endif()

# T23: 防漂移测试要读发布包模板。用编译期路径而不是"运行时猜", 这样在别的
# 机器上也能找到; 文件缺失时测试直接 FAIL, 不许跳过。不依赖 MSVC, 故放外面。
target_compile_definitions(logic_check PRIVATE
    "DGLAB_TEMPLATE_FILE=\"${CMAKE_CURRENT_SOURCE_DIR}/tools/DGLab_IDAC.ini.template\""
)
