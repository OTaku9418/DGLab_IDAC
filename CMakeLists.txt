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
project(DGLab_IDAC VERSION 0.1.0 LANGUAGES CXX)

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
# 头文件来自 v3.0.1 的 sparse checkout(_ue4ss_ref/RE-UE4SS)。只用 include/,
# 因为整个 mod ABI 就是 CppUserModBase 这一个类。
# ---------------------------------------------------------------------------
set(UE4SS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/_ue4ss_ref/RE-UE4SS" CACHE PATH "RE-UE4SS 源码根")
if(NOT EXISTS "${UE4SS_DIR}/UE4SS/include/Mod/CppUserModBase.hpp")
    message(FATAL_ERROR
        "找不到 RE-UE4SS v3.0.1 头文件 (${UE4SS_DIR}/UE4SS/include)。\n"
        "必须用 v3.0.1 —— 与本机 UE4SS.dll (v3.0.1 Beta #0196ef29) 对齐。\n"
        "换版本会改变 CppUserModBase 的 vtable 布局, mod 会在 UE4SS 调用虚函数时崩。")
endif()

# 已构建的 UE4SS.dll。链接它而不是重编它。
# ⚠ 路径一律用正斜杠 —— CMake 里反斜杠是转义字符, "D:\INDAC\..." 里的 \I 会被吞掉。
set(UE4SS_DLL "D:/INDAC/MODAC321/WindowsNoEditor/GameProject/Binaries/Win64/ue4ss/UE4SS.dll"
    CACHE FILEPATH "游戏目录里的 UE4SS.dll")
if(NOT EXISTS "${UE4SS_DLL}")
    message(FATAL_ERROR
        "找不到 UE4SS.dll。用 -DUE4SS_DLL=<路径> 指定, 或者\n"
        "  cmake -S . -B build -DUE4SS_DLL=<path-to>/UE4SS.dll")
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

set(DGLAB_SOURCES
    src/dllmain.cpp
    src/logger.cpp
    src/config.cpp
    src/pipeline.cpp
    src/source/indrun_hook_source.cpp
    src/transport/transport.cpp
    src/transport/transport_factory.cpp
    src/transport/null_transport.cpp
    ${DGLAB_BLE_SRC}
)

add_library(${PROJECT_NAME} SHARED ${DGLAB_SOURCES})

# UE4SS 是多个独立 CMake 子工程拼起来的, 每个子工程的头文件在
# <dep>/include/<Dep>/... 下, 所以 include 根必须是各子工程的 include/ 目录
# 本身, 而不是 deps/first。少一个就会报 “无法打开包括文件: File/Macros.hpp”。
# 用 file(GLOB) 动态收齐, 避免以后 UE4SS 加新 deps 时手改。
set(DGLAB_UE4SS_INCLUDES
    "${UE4SS_DIR}/UE4SS/include"
)
file(GLOB DGLAB_DEP_INCLUDE_DIRS LIST_DIRECTORIES true
    "${UE4SS_DIR}/deps/first/*/include")
foreach(_inc IN LISTS DGLAB_DEP_INCLUDE_DIRS)
    if(IS_DIRECTORY "${_inc}")
        list(APPEND DGLAB_UE4SS_INCLUDES "${_inc}")
    endif()
endforeach()
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
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
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
        "  先跑一次:  tools\\make_ue4ss_implib.cmd")
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    minhook
    "${UE4SS_IMPLIB}"
    windowsapp.lib      # WinRT 激活 (RoActivateInstance / IBluetoothLE*)
    runtimeobject.lib
    ole32.lib
    setupapi.lib
    bcrypt.lib
)

# 输出名: UE4SS 只找 <mod>/dlls/main.dll, 所以交付时改名。
# 构建产物仍叫 DGLab_IDAC.dll 以免和别的 mod 的 main.dll 混淆。
set_target_properties(${PROJECT_NAME} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "DGLab_IDAC"
)

message(STATUS "DGLab_IDAC: UE4SS 头文件 = ${UE4SS_DIR}")
message(STATUS "DGLab_IDAC: 链接 UE4SS.dll = ${UE4SS_DLL}")
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
