@echo off
REM Build an import library for UE4SS.dll.
REM link.exe rejects a DLL as a direct input (LNK1107), so we go:
REM   dumpbin /exports  ->  .def  ->  lib /def /machine:x64  ->  UE4SS.lib
REM
REM Usage:  tools\make_ue4ss_implib.cmd  ["D:\path\to\UE4SS.dll"]
REM         (no argument -> the default game path below)
setlocal EnableDelayedExpansion
set "PATH=%SystemRoot%\system32;%SystemRoot%;%SystemRoot%\System32\Wbem"
call "E:\VS\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

REM Repo-root-relative output so the repo can be cloned anywhere.
pushd "%~dp0.."
set "OUT=%CD%\third_party\importlib"
popd

set UE=%~1
if "%UE%"=="" set UE=D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\UE4SS.dll
if not exist "%UE%" (
    echo     ERROR: UE4SS.dll 不存在: "%UE%"
    echo     用法:  tools\make_ue4ss_implib.cmd "D:\path\to\UE4SS.dll"
    exit /b 1
)
if not exist "%OUT%" mkdir "%OUT%"

echo [1/4] dumpbin /exports
dumpbin /nologo /exports "%UE%" > "%OUT%\exports_full.txt"
if errorlevel 1 goto :fail

echo [2/4] filter export lines
REM Four fields: "      ordinal  hint  RVA  name". The hint column is what
REM disambiguates real rows from the header, which reads
REM "  4067 number of functions" -- only 2 digits there, while a real hint is
REM at least 3 wide in this dump. Using 3+ digits for RH2 rejects the header.
REM The last field must start with a non-space so nameless rows are dropped.
findstr /r /c:"^ *[0-9][0-9]*  *[0-9A-F][0-9A-F]*  *[0-9A-F][0-9A-F][0-9A-F]*  *[^ ]" ^
        "%OUT%\exports_full.txt" > "%OUT%\exports_raw.txt"

echo [3/4] write UE4SS.def
> "%OUT%\UE4SS.def" echo LIBRARY UE4SS
>>"%OUT%\UE4SS.def" echo EXPORTS
REM Field 4 onward is the export name. Mangled names never contain spaces,
REM but data exports and the "= forwarder" forms are still safe to pass through.
for /f "tokens=4*" %%A in (%OUT%\exports_raw.txt) do >>"%OUT%\UE4SS.def" echo %%A %%B

for /f %%C in ('find /c /v "" ^< "%OUT%\exports_raw.txt"') do echo     export lines: %%C

echo [4/4] lib /def /machine:x64
REM Pass the flags on separate lines to keep this line short. Two traps here:
REM   * lib.exe wants a single argv entry "/out:<path>" -- the colon must be
REM     inside the same token as /out, so the quotes wrap the whole switch.
REM   * Running this script from Git Bash with `cmd //c` lets MSYS rewrite
REM     "/out:D:\..." -- MSYS treats "/out:" as a unix path prefix, strips the
REM     colon, and leaves "\UE4SS.lib" as a bare word that cmd then reads as a
REM     REDIRECTION. That silently writes lib.exe's banner into the .lib
REM     (observed: a 10-byte file containing "    OK -"). The size guard below
REM     exists to catch exactly that class of failure.
lib /nologo ^
    /def:"%OUT%\UE4SS.def" ^
    /machine:x64 ^
    "/out:%OUT%\UE4SS.lib"
set LIB_RC=%errorlevel%
for %%S in ("%OUT%\UE4SS.lib") do set LIB_SZ=%%~zS
REM Delayed expansion (!LIB_SZ!) is REQUIRED: variables assigned inside a FOR
REM body are not visible via %LIB_SZ% later in the same block/session, because
REM %..% is expanded when the line is parsed. Without this, the comparison below
REM silently evaluates as garbage.
if not "%LIB_RC%"=="0" goto :fail
if not defined LIB_SZ goto :small
if !LIB_SZ! LSS 100000 goto :small
REM ---------------------------------------------------------------------------
REM !! DO NOT write "echo ... -> ..." in this script !!
REM cmd.exe parses "->" followed by a quoted path as REDIRECTION: the text
REM `echo     OK -> "%OUT%\UE4SS.lib"` is read as
REM     echo     OK -   >>  <path-to-UE4SS.lib>
REM and the success message therefore APPENDS itself into the import library,
REM leaving a 10-byte file containing "    OK -". That is exactly the bug this
REM script had. Use a plain '=' or a comma instead of an arrow.
REM ---------------------------------------------------------------------------
echo     OK = "%OUT%\UE4SS.lib" ^(!LIB_SZ! bytes^)
exit /b 0

:small
echo     ERROR: "%OUT%\UE4SS.lib" is only !LIB_SZ! bytes - generation failed
goto :fail

:fail
echo FAILED
exit /b 1
