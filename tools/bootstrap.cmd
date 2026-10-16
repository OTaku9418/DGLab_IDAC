@echo off
REM ===========================================================================
REM  bootstrap.cmd — 补齐第三方依赖 (克隆仓库后跑这一个就够了)
REM
REM  做三件事:
REM    1. 确认 ABI 对齐的 UE4SS 头文件在位(它们在仓库里, 正常情况下跳过)
REM    2. 初始化 MinHook 子模块
REM    3. 从游戏目录里的 UE4SS.dll 生成导入库
REM
REM  ★ 关于 UE4SS 头文件 ★
REM  _ue4ss_ref\ue4ss_0196ef29\ 与 _ue4ss_ref\ue4ss_0196ef29_deps\ 是**提交进
REM  仓库**的(约 330 KB), 因为必须是 0196ef29 那个 commit 的版本, 而不是上游
REM  tag v3.0.1。两者之间 CppUserModBase 的虚表布局不同, 用错版本编出来的 mod
REM  会在 UE4SS 调用虚函数时崩 —— 本仓库历史上就栽在这上面。所以它们跟着仓库走,
REM  这个脚本只负责在有人误删时把话说明白。
REM
REM  为什么不用 add_subdirectory(RE-UE4SS): 那会连 UE4SS.dll 本体一起重编,
REM  要走 FetchContent 拉 Rust 工具链(corrosion)和一堆 git@github.com: 的 SSH
REM  子模块, 约 20 分钟且极易失败。我们只链接游戏里那份**已经在跑的** UE4SS.dll,
REM  版本天生一致, 构建时间从 20 分钟降到几秒。
REM
REM  用法:  tools\bootstrap.cmd  [UE4SS.dll 路径]
REM         不传参数时按 %DLAB_DEPLOY_DIR% 推导, 再回退到下面那个默认游戏路径。
REM ===========================================================================
setlocal EnableDelayedExpansion

cd /d "%~dp0.."
set "REPO_ROOT=%CD%"

set "GIT=C:\Program Files\Git\cmd\git.exe"
if not exist "%GIT%" set "GIT=git"

REM 游戏目录。其它机器上请设 DLAB_DEPLOY_DIR (指向 ...\ue4ss\Mods\DGLab_IDAC),
REM 或直接把 UE4SS.dll 的路径当第一个参数传进来。
if "%DLAB_DEPLOY_DIR%"=="" set "DLAB_DEPLOY_DIR=D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\Mods\DGLab_IDAC"

set "UE4SS_DLL=%~1"
if "%UE4SS_DLL%"=="" (
    REM 从 ...\ue4ss\Mods\DGLab_IDAC 往上两级就是 ...\ue4ss
    for %%I in ("%DLAB_DEPLOY_DIR%\..\..") do set "UE4SS_DLL=%%~fI\UE4SS.dll"
)

echo [*] repo root : %REPO_ROOT%
echo [*] UE4SS dll : %UE4SS_DLL%
echo.

REM ---------------------------------------------------------------------------
REM 1. ABI 对齐的 UE4SS 头文件(已提交进仓库)
REM ---------------------------------------------------------------------------
set "HDR=%REPO_ROOT%\_ue4ss_ref\ue4ss_0196ef29"
set "DEPS=%REPO_ROOT%\_ue4ss_ref\ue4ss_0196ef29_deps"

if not exist "%HDR%\Mod\CppUserModBase.hpp" goto :nohdr
if not exist "%DEPS%\DynamicOutput\DynamicOutput\Output.hpp" goto :nohdr
echo [1/3] UE4SS 头文件在位 (0196ef29)
echo     OK = _ue4ss_ref\ue4ss_0196ef29
goto :hdr_done

:nohdr
echo     ERROR: 找不到 ABI 对齐的 UE4SS 头文件。
echo.
echo     应该在: %HDR%
echo.          %DEPS%
echo.
echo     它们**是仓库的一部分**, 正常情况下 git clone 下来就在。丢了就恢复:
echo         git checkout -- _ue4ss_ref
echo     或者重新克隆仓库。
echo.
echo     ★ 不要用上游 tag v3.0.1 的头文件顶替 ★
echo       那个版本的 CppUserModBase 虚表布局与 0196ef29 不同(少 on_ui_init 与
echo       on_cpp_mods_loaded, 新旧 on_lua_start/on_lua_stop 重载位置也不一样),
echo       编出来的 mod 会在 UE4SS 调用虚函数时崩。CMakeLists 里为此留了一段
echo       回退逻辑并在配置阶段打 WARNING, 那条路只用于刻意实验。
goto :fail

:hdr_done
echo.

REM ---------------------------------------------------------------------------
REM 2. MinHook
REM ---------------------------------------------------------------------------
if exist "%REPO_ROOT%\third_party\minhook\include\MinHook.h" (
    echo [2/3] MinHook 已存在, 跳过
) else (
    echo [2/3] 初始化 MinHook 子模块...
    "%GIT%" submodule update --init --recursive
    if errorlevel 1 goto :fail
)
if not exist "%REPO_ROOT%\third_party\minhook\include\MinHook.h" (
    echo     ERROR: MinHook 仍不在位
    goto :fail
)
echo     OK = third_party\minhook
echo.

REM ---------------------------------------------------------------------------
REM 3. UE4SS 导入库
REM ---------------------------------------------------------------------------
if not exist "%UE4SS_DLL%" (
    echo     ERROR: 找不到 UE4SS.dll: %UE4SS_DLL%
    echo.
    echo     指定路径的两种办法:
    echo       tools\bootstrap.cmd "D:\你的游戏\...\ue4ss\UE4SS.dll"
    echo       set DLAB_DEPLOY_DIR=D:\你的游戏\...\ue4ss\Mods\DGLab_IDAC
    echo.
    echo     默认值指向的是开发机上的路径, 其它机器上一定不对 —— 这是预期的,
    echo     不是脚本坏了。
    goto :fail
)
echo [3/3] 生成 UE4SS 导入库...
call "%REPO_ROOT%\tools\make_ue4ss_implib.cmd" "%UE4SS_DLL%"
if errorlevel 1 goto :fail
echo.

echo [ok] bootstrap 完成 — 接着跑:  tools\build.cmd -Deploy
exit /b 0

:fail
echo.
echo [!!] bootstrap 失败
exit /b 1
