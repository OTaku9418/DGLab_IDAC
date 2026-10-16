@echo off
REM ===========================================================================
REM  bootstrap.cmd — 重建全部第三方依赖 (克隆仓库后跑这一个就够了)
REM
REM  做三件事:
REM    1. 拉 RE-UE4SS v3.0.1 的**头文件**(稀疏检出, 带 blob 过滤, 约 11 MB)
REM    2. 初始化 MinHook 子模块
REM    3. 从游戏目录里的 UE4SS.dll 生成导入库
REM
REM  为什么不用 add_subdirectory(RE-UE4SS): 那会连 UE4SS.dll 本体一起重编,
REM  要走 FetchContent 拉 Rust 工具链(corrosion)和一堆 git@github.com: 的 SSH
REM  子模块, 约 20 分钟且极易失败。我们只链接游戏里那份**已经在跑的** UE4SS.dll,
REM  版本天生一致, 构建时间从 20 分钟降到几秒。
REM
REM  用法:  tools\bootstrap.cmd  [UE4SS_DLL 路径]
REM         不传参数时用下面默认的那个游戏路径。
REM ===========================================================================
setlocal EnableDelayedExpansion

cd /d "%~dp0.."
set "REPO_ROOT=%CD%"

set "GIT=C:\Program Files\Git\cmd\git.exe"
if not exist "%GIT%" set "GIT=git"

set "UE4SS_TAG=v3.0.1"
set "UE4SS_REF=%REPO_ROOT%\_ue4ss_ref\RE-UE4SS"

set "UE4SS_DLL=%~1"
if "%UE4SS_DLL%"=="" set "UE4SS_DLL=D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\UE4SS.dll"

echo [*] repo root : %REPO_ROOT%
echo [*] UE4SS dll : %UE4SS_DLL%
echo.

REM ---------------------------------------------------------------------------
REM 1. RE-UE4SS 头文件
REM ---------------------------------------------------------------------------
REM 注意: 这里**不**用 --recurse-submodules。整个构建只需要
REM   UE4SS/include/**             -> CppUserModBase.hpp 等
REM   deps/first/*/include/**      -> DynamicOutput / File / GUI ...
REM 两个 gitlink (deps/first/Unreal = UEPseudo, deps/first/patternsleuth) 都
REM **不需要**: UEPseudo 是私有的 EpicGames/UnrealEngine 镜像, 而本 mod 完全不碰
REM Unreal API(只挂非 UE 模块 IndRun.dll 的裸导出)。
if exist "%UE4SS_REF%\.git" (
    echo [=] RE-UE4SS 已存在, 跳过克隆
) else (
    echo [1/3] 克隆 RE-UE4SS %UE4SS_TAG% ^(仅头文件^)...
    "%GIT%" clone --filter=blob:none --sparse --depth 1 --branch %UE4SS_TAG% ^
        https://github.com/UE4SS-RE/RE-UE4SS.git "%UE4SS_REF%"
    if errorlevel 1 goto :fail
    pushd "%UE4SS_REF%"
    "%GIT%" sparse-checkout set UE4SS/include UE4SS/src cmake deps docs
    popd
)

if not exist "%UE4SS_REF%\UE4SS\include\Mod\CppUserModBase.hpp" (
    echo     ERROR: CppUserModBase.hpp 不在位, 稀疏检出不对
    goto :fail
)
if not exist "%UE4SS_REF%\deps\first\DynamicOutput\include\DynamicOutput\Output.hpp" (
    echo     ERROR: DynamicOutput 头文件不在位
    goto :fail
)
echo     OK = %UE4SS_REF%
echo.

REM ---------------------------------------------------------------------------
REM 2. MinHook
REM ---------------------------------------------------------------------------
if exist "%REPO_ROOT%\third_party\minhook\include\MinHook.h" (
    echo [=] MinHook 已存在, 跳过
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
    echo     改成:  tools\bootstrap.cmd "D:\你的路径\ue4ss\UE4SS.dll"
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
