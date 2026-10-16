@echo off
REM ---------------------------------------------------------------------------
REM DGLab_IDAC -- one-shot build script
REM
REM Why this script exists: MSVC is not on PATH. cl.exe / link.exe need
REM INCLUDE / LIB / PATH injected by vcvars64.bat, and vcvars only affects the
REM current cmd session. So we call it once, then drive cmake from here.
REM
REM NOTE: this file is deliberately ASCII-only. cmd.exe parses .cmd files using
REM the system ANSI codepage; UTF-8 (especially CJK) gets mangled and turns into
REM bogus commands. Keep all Chinese text out of .cmd/.bat files.
REM
REM Usage:
REM   tools\build.cmd                build Release
REM   tools\build.cmd -Deploy        also copy into the game's Mods folder
REM   tools\build.cmd -Clean         wipe the build dir first
REM   tools\build.cmd -Configure     configure only, do not compile
REM ---------------------------------------------------------------------------
setlocal EnableDelayedExpansion

set "REPO=%~dp0.."
pushd "%REPO%" || (echo [x] cannot enter %REPO% & exit /b 1)

REM Toolchain location (measured on this machine: Build Tools live on E:)
if not defined VCVARS set "VCVARS=E:\VS\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "GAME_MODS=D:/INDAC/MODAC321/WindowsNoEditor/GameProject/Binaries/Win64/ue4ss/Mods/DGLab_IDAC"

set "DO_DEPLOY="
set "DO_CLEAN="
set "DO_CONFIGURE_ONLY="
:parse
if "%~1"=="" goto parsed
if /I "%~1"=="-Deploy"     set "DO_DEPLOY=1"
if /I "%~1"=="--deploy"    set "DO_DEPLOY=1"
if /I "%~1"=="-Clean"      set "DO_CLEAN=1"
if /I "%~1"=="--clean"     set "DO_CLEAN=1"
if /I "%~1"=="-Configure"  set "DO_CONFIGURE_ONLY=1"
if /I "%~1"=="--configure" set "DO_CONFIGURE_ONLY=1"
shift
goto parse
:parsed

REM ---- MinHook submodule ----
if not exist "third_party\minhook\CMakeLists.txt" (
    echo [*] fetching MinHook submodule...
    git submodule update --init --recursive
    if errorlevel 1 (echo [x] submodule fetch failed & popd & exit /b 1)
)

REM ---- vcvars ----
if not exist "%VCVARS%" (
    echo [x] vcvars64.bat not found: %VCVARS%
    echo     Set VCVARS=^<path^> or install the C++ workload of VS Build Tools.
    popd & exit /b 1
)
echo [*] toolchain: %VCVARS%
call "%VCVARS%" >nul
if errorlevel 1 (echo [x] vcvars64.bat failed & popd & exit /b 1)

where cl.exe >nul 2>nul || (echo [x] vcvars ran but cl.exe is still not on PATH & popd & exit /b 1)

REM ---- clean ----
if defined DO_CLEAN (
    echo [*] removing build\
    if exist build rmdir /s /q build
)

REM ---- configure ----
REM Use the MSBuild generator, not Ninja: this Build Tools install has no
REM ninja.exe (it ships with the "C++ CMake tools for Windows" component),
REM while MSBuild is always present.
set "CMAKE_ARGS=-S . -B build -G "Visual Studio 17 2022" -A x64"
if defined DO_DEPLOY set "CMAKE_ARGS=!CMAKE_ARGS! -DDGLAB_DEPLOY_DIR=%GAME_MODS%"

echo [*] cmake !CMAKE_ARGS!
cmake !CMAKE_ARGS!
if errorlevel 1 (echo [x] configure failed & popd & exit /b 1)

if defined DO_CONFIGURE_ONLY (
    echo [ok] configure done. Build with: tools\build.cmd
    popd & exit /b 0
)

REM ---- build ----
cmake --build build --config Release
if errorlevel 1 (echo [x] build failed & popd & exit /b 1)

echo.
echo [ok] build complete:
if exist build\Release\DGLab_IDAC.dll echo     %REPO%\build\Release\DGLab_IDAC.dll

if defined DO_DEPLOY (
    echo [ok] deployed to: %GAME_MODS%
    echo     Remember to add "DGLab_IDAC : 1" to Mods\mods.txt,
    echo     or drop an empty enabled.txt into Mods\DGLab_IDAC\
) else (
    echo.
    echo Deploy with: tools\build.cmd -Deploy
    echo Manual deploy: copy build\Release\DGLab_IDAC.dll to
    echo   ^<game^>\...\ue4ss\Mods\DGLab_IDAC\dlls\main.dll
)

popd
endlocal
