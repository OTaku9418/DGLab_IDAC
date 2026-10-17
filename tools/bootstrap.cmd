@echo off
REM ===========================================================================
REM  bootstrap.cmd -- prepare third-party dependencies (run once after clone)
REM
REM  Three steps:
REM    1. verify the ABI-pinned UE4SS headers + fmt 11.2.0 are present
REM       (both are IN the repo; normally this is a no-op)
REM    2. init the MinHook submodule
REM    3. generate the UE4SS import library from the checked-in UE4SS.def
REM       (**no game installation and no UE4SS.dll required**)
REM
REM  NOTE: this file is deliberately ASCII-only. cmd.exe parses .cmd files with
REM  the system ANSI codepage, so UTF-8 (especially CJK) turns into mojibake and
REM  can swallow the following character. Same rule as tools\build.cmd. The
REM  Chinese explanation lives in README.md and re\indrun_hook_mechanics.md.
REM
REM  WHY THE HEADERS ARE IN THE REPO
REM    _ue4ss_ref\ue4ss_0196ef29\ and _ue4ss_ref\ue4ss_0196ef29_deps\ are
REM    committed because they must be commit 0196ef29, NOT upstream tag v3.0.1.
REM    CppUserModBase's vtable layout differs between the two (v3.0.1 lacks
REM    on_ui_init and on_cpp_mods_loaded, and the on_lua_start/on_lua_stop
REM    overloads sit at different slots). Building against the wrong one makes
REM    the mod crash inside UE4SS when a virtual is dispatched -- this project
REM    has already been bitten by exactly that.
REM
REM  WHY fmt IS IN THE REPO
REM    _ue4ss_ref\third_party\fmt is fmt 11.2.0 (the version 0196ef29 pins).
REM    DynamicOutput's headers #include <fmt/core.h>, and the fmt statically
REM    linked into UE4SS.dll is NOT exported, so the symbols must come from our
REM    own copy. The version must match exactly. See PROVENANCE.md there.
REM
REM  WHY THE IMPORT LIB COMES FROM A .def
REM    It used to require dumpbin against the real UE4SS.dll, i.e. "you cannot
REM    build without the game installed". Now UE4SS.def (4067 export lines,
REM    342 KB of text) is committed, and lib.exe turns it into UE4SS.lib
REM    directly. Only when changing the UE4SS version do you need to re-dump
REM    with tools\make_ue4ss_implib.cmd. A checked-in .lib would be a binary
REM    blob that also depends on the MSVC version, hence the .def.
REM
REM  WHY NOT add_subdirectory(RE-UE4SS)
REM    That rebuilds UE4SS.dll itself and drags in FetchContent plus a pile of
REM    git@github.com: SSH submodules (corrosion / Rust toolchain): ~20 minutes
REM    and very failure-prone. We only link against the UE4SS.dll already
REM    running in the game, so the version matches by construction and the
REM    build takes seconds.
REM
REM  Usage:  tools\bootstrap.cmd  [path\to\UE4SS.dll]
REM          The path is only needed to re-generate the .def; a normal run
REM          never touches it. Without an argument it is derived from
REM          %DLAB_DEPLOY_DIR% and finally from the default game path below.
REM ===========================================================================
setlocal EnableDelayedExpansion

cd /d "%~dp0.."
set "REPO_ROOT=%CD%"

set "GIT=C:\Program Files\Git\cmd\git.exe"
if not exist "%GIT%" set "GIT=git"

REM Game mod dir. On another machine set DLAB_DEPLOY_DIR, or pass the UE4SS.dll
REM path as argument 1. Only used to re-generate the .def.
if "%DLAB_DEPLOY_DIR%"=="" set "DLAB_DEPLOY_DIR=D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\Mods\DGLab_IDAC"

set "UE4SS_DLL=%~1"
if "%UE4SS_DLL%"=="" (
    REM ...\ue4ss\Mods\DGLab_IDAC -> two levels up is ...\ue4ss
    for %%I in ("%DLAB_DEPLOY_DIR%\..\..") do set "UE4SS_DLL=%%~fI\UE4SS.dll"
)

echo [*] repo root : %REPO_ROOT%
echo.

REM ---------------------------------------------------------------------------
REM 1. ABI-pinned UE4SS headers + fmt (both committed)
REM ---------------------------------------------------------------------------
set "HDR=%REPO_ROOT%\_ue4ss_ref\ue4ss_0196ef29"
set "DEPS=%REPO_ROOT%\_ue4ss_ref\ue4ss_0196ef29_deps"
set "FMT=%REPO_ROOT%\_ue4ss_ref\third_party\fmt"

if not exist "%HDR%\Mod\CppUserModBase.hpp" goto :nohdr
if not exist "%DEPS%\DynamicOutput\DynamicOutput\Output.hpp" goto :nohdr
echo [1/3] UE4SS headers present (0196ef29)
if not exist "%FMT%\include\fmt\core.h" goto :nofmt
if not exist "%FMT%\src\format.cc" goto :nofmt
echo       fmt 11.2.0 present
goto :hdr_done

:nofmt
echo     ERROR: fmt headers not found.
echo.
echo     Expected: %FMT%
echo               (include\fmt\core.h and src\format.cc)
echo.
echo     fmt IS part of this repository. Restore it with:
echo         git checkout -- _ue4ss_ref/third_party/fmt
echo.
echo     It cannot be skipped: DynamicOutput\DynamicOutput\Output.hpp does
echo     #include ^<fmt/core.h^>, and the fmt that UE4SS.dll statically linked
echo     is not exported (8 missing symbols otherwise). The version must also
echo     match UE4SS exactly. See PROVENANCE.md in that directory.
goto :fail

:nohdr
echo     ERROR: ABI-pinned UE4SS headers not found.
echo.
echo     Expected: %HDR%
echo               %DEPS%
echo.
echo     They ARE part of this repository, so a normal git clone already has
echo     them. Restore with:
echo         git checkout -- _ue4ss_ref
echo     or re-clone.
echo.
echo     DO NOT substitute upstream tag v3.0.1 headers:
echo       that version's CppUserModBase vtable differs (no on_ui_init, no
echo       on_cpp_mods_loaded, on_lua_start/on_lua_stop at other slots) and the
echo       resulting mod crashes inside UE4SS on the first virtual dispatch.
echo       CMakeLists.txt keeps a fallback for deliberate experiments and prints
echo       a WARNING when it takes it.
goto :fail

:hdr_done
echo.

REM ---------------------------------------------------------------------------
REM 2. MinHook
REM ---------------------------------------------------------------------------
if exist "%REPO_ROOT%\third_party\minhook\include\MinHook.h" (
    echo [2/3] MinHook already present, skipped
) else (
    echo [2/3] initialising MinHook submodule...
    "%GIT%" submodule update --init --recursive
    if errorlevel 1 goto :fail
)
if not exist "%REPO_ROOT%\third_party\minhook\include\MinHook.h" (
    echo     ERROR: MinHook still missing
    goto :fail
)
echo       OK = third_party\minhook
echo.

REM ---------------------------------------------------------------------------
REM 3. UE4SS import library -- prefer the checked-in .def, no UE4SS.dll needed
REM ---------------------------------------------------------------------------
set "IMPLIB_DIR=%REPO_ROOT%\third_party\importlib"
set "DEF=%IMPLIB_DIR%\UE4SS.def"
REM Do NOT name this variable LIB: vcvars64.bat sets the MSVC LIB search-path
REM variable, so a variable called LIB silently turns into a semicolon list of
REM directories and lib.exe fails with LNK1104 on a bogus path. (Learned the
REM hard way: it worked on the dev machine only because the .lib was cached.)
set "IMPLIB=%IMPLIB_DIR%\UE4SS.lib"

if exist "%IMPLIB%" (
    echo [3/3] import library already present, skipped
    echo       OK = third_party\importlib\UE4SS.lib
    goto :lib_done
)

if not exist "%DEF%" (
    echo [3/3] no UE4SS.def in the repo, falling back to dumpbin...
    if not exist "%UE4SS_DLL%" (
        echo     ERROR: neither %DEF%
        echo     nor UE4SS.dll at: %UE4SS_DLL%
        echo.
        echo     A normal checkout does not end up here -- UE4SS.def is part of
        echo     the repository. Try:
        echo         git checkout -- third_party/importlib/UE4SS.def
        echo     To regenerate it:
        echo         tools\make_ue4ss_implib.cmd "D:\...\ue4ss\UE4SS.dll"
        goto :fail
    )
    call "%REPO_ROOT%\tools\make_ue4ss_implib.cmd" "%UE4SS_DLL%"
    if errorlevel 1 goto :fail
    goto :lib_done
)

echo [3/3] generating import library from UE4SS.def ^(no game install needed^)...
REM One switch per line and the /out value quoted as a whole -- see the comment
REM in make_ue4ss_implib.cmd (MSYS rewrites arguments that start with "/", which
REM makes lib.exe write its banner into the .lib).
call "E:\VS\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto :novcvars
lib /nologo /def:"%DEF%" /machine:x64 "/out:%IMPLIB%"
if errorlevel 1 goto :fail
REM Size guard: a 10-byte "OK -"-style artefact must never get through. It has
REM happened before, which is why this check exists.
for %%S in ("%IMPLIB%") do set "IMPLIB_SZ=%%~zS"
if not defined IMPLIB_SZ goto :libbad
if !IMPLIB_SZ! LSS 100000 goto :libbad
echo       OK = third_party\importlib\UE4SS.lib ^(!IMPLIB_SZ! bytes^)
goto :lib_done

:novcvars
echo     ERROR: MSVC toolchain not found ^(vcvars64.bat failed^)
echo     lib.exe needs it. This machine has Build Tools at E:\VS\BuildTools;
echo     on another machine edit the vcvars path above or set VCVARS.
goto :fail

:libbad
echo     ERROR: generated UE4SS.lib is not a valid import library
goto :fail

:lib_done
echo.

echo [ok] bootstrap done -- next:  tools\build.cmd -Deploy
exit /b 0

:fail
echo.
echo [!!] bootstrap failed
exit /b 1
