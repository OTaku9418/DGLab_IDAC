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
REM   tools\build.cmd -Stage N       deploy AND set the startup bisect stage.
REM                                  N is written to <mods>\DGLab_IDAC\dlls\bisect.txt
REM                                  and the mod is force-enabled. See the stage
REM                                  table in src\dllmain.cpp:
REM                                    0 = load only (do nothing)
REM                                    1 = our own bisect log
REM                                    2 = + UE4SS Output::send
REM                                    3 = + logger file log
REM                                    4 = + INI read/write
REM                                    5 = + worker thread
REM                                   10 = + transport
REM                                   11 = + hooks (everything)
REM                                  Implies -Deploy.
REM ---------------------------------------------------------------------------
setlocal EnableDelayedExpansion

set "REPO=%~dp0.."
pushd "%REPO%" || (echo [x] cannot enter %REPO% & exit /b 1)

REM Toolchain location (measured on this machine: Build Tools live on E:)
if not defined VCVARS set "VCVARS=E:\VS\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

REM Game mod dir. On other machines set DLAB_DEPLOY_DIR to override, or just
REM do not deploy (without -Deploy the output stays in build\Release\).
REM Backslashes here are for cmd; they are converted to forward slashes before
REM being handed to CMake.
if not defined DLAB_DEPLOY_DIR set "DLAB_DEPLOY_DIR=D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\Mods\DGLab_IDAC"
set "GAME_MODS=%DLAB_DEPLOY_DIR%"

set "DO_DEPLOY="
set "DO_CLEAN="
set "DO_CONFIGURE_ONLY="
set "STAGE="
:parse
if "%~1"=="" goto parsed
if /I "%~1"=="-Deploy"     set "DO_DEPLOY=1"
if /I "%~1"=="--deploy"    set "DO_DEPLOY=1"
if /I "%~1"=="-Clean"      set "DO_CLEAN=1"
if /I "%~1"=="--clean"     set "DO_CLEAN=1"
if /I "%~1"=="-Configure"  set "DO_CONFIGURE_ONLY=1"
if /I "%~1"=="--configure" set "DO_CONFIGURE_ONLY=1"
if /I "%~1"=="-Stage" (
    set "STAGE=%~2"
    set "DO_DEPLOY=1"
    shift
)
if /I "%~1"=="--stage" (
    set "STAGE=%~2"
    set "DO_DEPLOY=1"
    shift
)
shift
goto parse
:parsed

REM ---- validate -Stage ----
REM A typo here is silent and expensive: the mod reads bisect.txt with atoi, so
REM a non-numeric stage becomes 0 (= loaded but does nothing) and looks exactly
REM like "the mod is not working".
if defined STAGE (
    echo !STAGE!| findstr /R /C:"^[0-9][0-9]*$" >nul
    if errorlevel 1 (
        echo [x] -Stage needs an integer in 0..11, got "!STAGE!"
        popd & exit /b 1
    )
    if !STAGE! GTR 11 (
        echo [x] -Stage must be in 0..11  ^(0 = load only, 11 = full^), got !STAGE!
        popd & exit /b 1
    )
)

set "BUILD_DIR=build"

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
    echo [*] removing %BUILD_DIR%\
    if exist %BUILD_DIR% rmdir /s /q %BUILD_DIR%
)

REM ---- configure ----
REM Use the MSBuild generator, not Ninja: this Build Tools install has no
REM ninja.exe (it ships with the "C++ CMake tools for Windows" component),
REM while MSBuild is always present.
set "CMAKE_ARGS=-S . -B %BUILD_DIR% -G "Visual Studio 17 2022" -A x64"
if defined DO_DEPLOY (
    REM CMake treats backslash as an escape character, so "D:\INDAC" would lose
    REM the "\I". Convert to forward slashes.
    set "GAME_MODS_FWD=!GAME_MODS:\=/!"
    set "CMAKE_ARGS=!CMAKE_ARGS! -DDGLAB_DEPLOY_DIR=!GAME_MODS_FWD!"
)

echo [*] cmake !CMAKE_ARGS!
cmake !CMAKE_ARGS!
if errorlevel 1 (echo [x] configure failed & popd & exit /b 1)

if defined DO_CONFIGURE_ONLY (
    echo [ok] configure done. Build with: tools\build.cmd
    popd & exit /b 0
)

REM ---- build ----
cmake --build %BUILD_DIR% --config Release
if errorlevel 1 (echo [x] build failed & popd & exit /b 1)

echo.
echo [ok] build complete:
if exist %BUILD_DIR%\Release\DGLab_IDAC.dll echo     %REPO%\%BUILD_DIR%\Release\DGLab_IDAC.dll

if defined DO_DEPLOY (
    echo [ok] deployed to: %GAME_MODS%
    REM Always force-enable the mod here: bisect needs to know FOR SURE
    REM whether our DLL was loaded. If it crashed and you reboot into the
    REM game, you must run build.cmd -Stage 0 to get a safe no-op build in.
    if exist "%GAME_MODS%\..\mods.txt" (
        findstr /C:"DGLab_IDAC : 1" "%GAME_MODS%\..\mods.txt" >nul 2>nul
        if errorlevel 1 (
            echo [*] enabling the mod in Mods\mods.txt ...
            python "%REPO%\re\ab_toggle.py" --on
        )
    )
)

if defined STAGE (
    REM Redirect-into-file order matters: "echo 5> file" makes cmd treat the 5
    REM as a handle number, so nothing useful lands in the file. Put the
    REM redirection first.
    >"%GAME_MODS%\dlls\bisect.txt" echo %STAGE%
    echo [ok] bisect stage set to %STAGE%  ^(bisect.txt written^)
    echo      bisect log will be: %GAME_MODS%\dlls\bisect.log
    if "%STAGE%"=="0" echo      stage 0 = load only, does nothing. Safe.
) else (
    if defined DO_DEPLOY (
        echo.
        echo [i] bisect stage NOT set. Without dlls\bisect.txt the mod runs FULL stage 11.
        echo     To bisect, run: tools\build.cmd -Stage N   ^(0 = load only, 11 = full^)
    )
)

if not defined DO_DEPLOY (
    echo.
    echo Deploy with: tools\build.cmd -Deploy
    echo Manual deploy: copy build\Release\DGLab_IDAC.dll to
    echo   ^<game^>\...\ue4ss\Mods\DGLab_IDAC\dlls\main.dll
)

popd
endlocal
