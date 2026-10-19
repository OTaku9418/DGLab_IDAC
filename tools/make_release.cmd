@echo off
rem ===========================================================================
rem  DGLab_IDAC - make a release zip
rem
rem  Produces  dist\DGLab_IDAC-v<version>-ble.zip  containing only what a user
rem  needs:
rem      DGLab_IDAC\dlls\main.dll
rem      DGLab_IDAC\dlls\DGLab_IDAC.ini
rem      DGLab_IDAC\README.md
rem      DGLab_IDAC\LICENSE
rem      DGLab_IDAC\THIRD_PARTY_NOTICES.txt
rem
rem  The PDB goes next to the zip (dist\), NOT inside it: it is a debugging
rem  aid for crash reports, not something a user should unpack.
rem
rem  usage:  tools\make_release.cmd                real release: always rebuilds
rem          tools\make_release.cmd -TestPackage   reuse build\Release .dll,
rem                                                and mark the zip "-test"
rem
rem  -NoBuild does not exist on purpose. A release artifact has to come from a
rem  build that just ran on a clean tree; otherwise "which binary is in this
rem  zip" has no answer. -TestPackage is the escape hatch, and it says so in
rem  the file name.
rem ===========================================================================
setlocal EnableDelayedExpansion
set "ROOT=%~dp0.."
pushd "%ROOT%" || exit /b 1

set "TESTPKG="
set "SUFFIX="
if /i "%~1"=="-TestPackage" (
    set "TESTPKG=1"
    set "SUFFIX=-test"
) else (
    if not "%~1"=="" (
        echo [x] unknown option "%~1"
        echo     usage: tools\make_release.cmd [-TestPackage]
        popd & exit /b 1
    )
)

rem ---- version from CMakeLists.txt -------------------------------------
set "VER="
for /f "usebackq tokens=3" %%A in (`findstr /r /c:"^project(DGLab_IDAC VERSION" CMakeLists.txt`) do set "VER=%%A"
if not defined VER (
    echo [x] cannot read the version from CMakeLists.txt
    popd & exit /b 1
)

rem ---- working tree must be clean --------------------------------------
set "DIRTY="
for /f "usebackq delims=" %%A in (`git status --porcelain`) do set "DIRTY=1"
if defined DIRTY (
    echo [x] working tree is dirty - commit or stash first, then re-run:
    git status --short
    popd & exit /b 1
)
for /f "usebackq tokens=*" %%A in (`git rev-parse --short HEAD`) do set "SHA=%%A"
for /f "usebackq tokens=*" %%A in (`git describe --tags --always --dirty`) do set "DESC=%%A"

echo [info] version = %VER%
echo [info] HEAD    = %SHA%  (%DESC%)
if defined TESTPKG echo [warn] TEST PACKAGE - reusing the existing binary, do not publish this

rem ---- build ------------------------------------------------------------
if not defined TESTPKG (
    call tools\build.cmd -Clean
    if errorlevel 1 (
        echo [x] build failed
        popd & exit /b 1
    )
) else (
    if not exist "build\Release\DGLab_IDAC.dll" (
        echo [x] -TestPackage needs build\Release\DGLab_IDAC.dll - run tools\build.cmd first
        popd & exit /b 1
    )
)

rem ---- stage ------------------------------------------------------------
set "STAGE=build\release_stage"
set "MODDIR=%STAGE%\DGLab_IDAC"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%MODDIR%\dlls" || (echo [x] cannot create stage dir & popd & exit /b 1)

call :copy build\Release\DGLab_IDAC.dll      "%MODDIR%\dlls\main.dll"
call :copy tools\DGLab_IDAC.ini.template     "%MODDIR%\dlls\DGLab_IDAC.ini"
call :copy README.md                         "%MODDIR%\README.md"
call :copy LICENSE                           "%MODDIR%\LICENSE"
call :copy THIRD_PARTY_NOTICES.txt           "%MODDIR%\THIRD_PARTY_NOTICES.txt"

rem enabled.txt: E1-E4 tests (2026-10-19) showed UE4SS loads a mod either
rem from mods.txt or from enabled.txt, never twice, and enabled.txt wins over
rem 'DGLab_IDAC : 0' in mods.txt. Shipping it means "unzip into Mods\ and it
rem just works", no mods.txt edit.
type nul > "%MODDIR%\enabled.txt"
if not exist "%MODDIR%\enabled.txt" (echo [x] cannot create enabled.txt & popd & exit /b 1)

rem The shipped INI must be CRLF. The mod normalises it on first load, but a
rem release should not depend on that: the first run would rewrite a file the
rem user just unpacked. The repo template is LF (*.template eol=lf), so the
rem conversion happens here - and then gets verified, because this step has
rem silently produced LF before.
powershell -NoProfile -ExecutionPolicy Bypass -File "tools/normalize_crlf.ps1" -Path "%MODDIR%\dlls\DGLab_IDAC.ini" >nul || (echo [x] INI line-ending fix failed & popd & exit /b 1)
findstr /c:"[safety]" "%MODDIR%\dlls\DGLab_IDAC.ini" >nul || (echo [x] staged INI looks empty & popd & exit /b 1)
rem DGLab_IDAC.ini.default: the factory config, kept next to dlls\ so a user
rem can restore it after we overwrite their tuned dlls\DGLab_IDAC.ini on
rem upgrade. Must be byte-identical to the shipped INI (checked below).
copy /y "%MODDIR%\dlls\DGLab_IDAC.ini" "%MODDIR%\DGLab_IDAC.ini.default" >nul
if errorlevel 1 (echo [x] cannot create DGLab_IDAC.ini.default & popd & exit /b 1)
set "HINI="
set "HDEF="
for /f "usebackq tokens=*" %%A in (`powershell -NoProfile -Command "(Get-FileHash -Algorithm SHA256 -LiteralPath '%MODDIR%\dlls\DGLab_IDAC.ini').Hash"`) do set "HINI=%%A"
for /f "usebackq tokens=*" %%A in (`powershell -NoProfile -Command "(Get-FileHash -Algorithm SHA256 -LiteralPath '%MODDIR%\DGLab_IDAC.ini.default').Hash"`) do set "HDEF=%%A"
if not defined HINI (echo [x] cannot hash the staged INI & popd & exit /b 1)
if /i not "%HINI%"=="%HDEF%" (echo [x] DGLab_IDAC.ini.default differs from dlls\DGLab_IDAC.ini & popd & exit /b 1)

rem ---- zip + pdb -------------------------------------------------------
if not exist "dist" mkdir "dist"
set "ZIP=dist\DGLab_IDAC-v%VER%-ble%SUFFIX%.zip"
set "PDB=dist\DGLab_IDAC-v%VER%-ble%SUFFIX%.pdb"
if exist "%ZIP%" del /q "%ZIP%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%STAGE%\DGLab_IDAC' -DestinationPath '%ZIP%' -CompressionLevel Optimal -Force"
if errorlevel 1 (
    echo [x] Compress-Archive failed
    popd & exit /b 1
)
if exist "build\Release\DGLab_IDAC.pdb" (
    copy /y "build\Release\DGLab_IDAC.pdb" "%PDB%" >nul
) else (
    echo [warn] build\Release\DGLab_IDAC.pdb not found - crash reports will have no symbols
)

echo.
echo ============================================================
echo  [ok] %ZIP%
echo ============================================================
echo  contents:
call :hash "%MODDIR%\dlls\main.dll"
call :hash "%MODDIR%\dlls\DGLab_IDAC.ini"
call :hash "%MODDIR%\README.md"
call :hash "%MODDIR%\LICENSE"
call :hash "%MODDIR%\THIRD_PARTY_NOTICES.txt"
call :hash "%MODDIR%\enabled.txt"
call :hash "%MODDIR%\DGLab_IDAC.ini.default"
echo.
echo  archive / symbols (not all of these are inside the zip):
call :hash "%ZIP%"
if exist "%PDB%" call :hash "%PDB%"
echo.
echo  built from commit %SHA%  (%DESC%)
if defined TESTPKG echo  ***** TEST PACKAGE - not for release *****
echo ============================================================
popd
exit /b 0

:copy
copy /y %1 %2 >nul
if errorlevel 1 (
    echo [x] copy failed: %1
    popd & exit /b 1
)
exit /b 0

:hash
set "H1="
set "H2="
for /f "usebackq tokens=*" %%A in (`powershell -NoProfile -Command "(Get-FileHash -Algorithm MD5 -LiteralPath '%~f1').Hash.ToLower()"`) do set "H1=%%A"
for /f "usebackq tokens=*" %%A in (`powershell -NoProfile -Command "(Get-FileHash -Algorithm SHA256 -LiteralPath '%~f1').Hash.ToLower()"`) do set "H2=%%A"
for %%S in ("%~f1") do echo   %%~nxS  %%~zS bytes
echo     md5    %H1%
echo     sha256 %H2%
exit /b 0
