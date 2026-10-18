@echo off
rem ===========================================================================
rem  DGLab_IDAC - make a release zip
rem
rem  Produces  dist\DGLab_IDAC-v<version>.zip  containing only what a user
rem  needs:
rem      DGLab_IDAC\dlls\main.dll
rem      DGLab_IDAC\dlls\DGLab_IDAC.ini
rem      DGLab_IDAC\README.md
rem      DGLab_IDAC\LICENSE
rem
rem  The version is read from CMakeLists.txt (single source of truth), so the
rem  zip name can never disagree with the string baked into the DLL.
rem
rem  Refuses to run on a dirty working tree: a release must be reproducible
rem  from a commit, otherwise "which code is in this zip" has no answer.
rem
rem  usage:  tools\make_release.cmd [-NoBuild]
rem          -NoBuild   reuse build\Release\DGLab_IDAC.dll (fast re-package)
rem ===========================================================================
setlocal EnableDelayedExpansion
set "ROOT=%~dp0.."
pushd "%ROOT%" || exit /b 1

set "NOBUILD="
if /i "%~1"=="-NoBuild" set "NOBUILD=1"

rem ---- version from CMakeLists.txt -------------------------------------
set "VER="
for /f "usebackq tokens=3" %%A in (`findstr /r /c:"^project(DGLab_IDAC VERSION" CMakeLists.txt`) do set "VER=%%A"
if not defined VER (
    echo [x] cannot read the version from CMakeLists.txt
    popd & exit /b 1
)
echo [info] version = %VER%

rem ---- working tree must be clean --------------------------------------
set "DIRTY="
for /f "usebackq delims=" %%A in (`git status --porcelain`) do set "DIRTY=1"
if defined DIRTY (
    echo [x] working tree is dirty - commit or stash first, then re-run:
    git status --short
    popd & exit /b 1
)
for /f "usebackq tokens=*" %%A in (`git rev-parse --short HEAD`) do set "SHA=%%A"
echo [info] HEAD = %SHA%

rem ---- build ------------------------------------------------------------
if not defined NOBUILD (
    call tools\build.cmd -Clean
    if errorlevel 1 (
        echo [x] build failed
        popd & exit /b 1
    )
) else (
    echo [info] -NoBuild: reusing build\Release\DGLab_IDAC.dll
)

if not exist "build\Release\DGLab_IDAC.dll" (
    echo [x] build\Release\DGLab_IDAC.dll not found - run tools\build.cmd first
    popd & exit /b 1
)

rem ---- stage ------------------------------------------------------------
set "STAGE=build\release_stage"
set "MODDIR=%STAGE%\DGLab_IDAC"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%MODDIR%\dlls" || (echo [x] cannot create stage dir & popd & exit /b 1)

copy /y "build\Release\DGLab_IDAC.dll" "%MODDIR%\dlls\main.dll" >nul || (echo [x] copy dll failed & popd & exit /b 1)
copy /y "tools\DGLab_IDAC.ini.template" "%MODDIR%\dlls\DGLab_IDAC.ini" >nul || (echo [x] copy ini failed & popd & exit /b 1)
copy /y "README.md" "%MODDIR%\README.md" >nul || (echo [x] copy README failed & popd & exit /b 1)
copy /y "LICENSE" "%MODDIR%\LICENSE" >nul || (echo [x] copy LICENSE failed & popd & exit /b 1)

rem The shipped INI must be CRLF. The mod normalises it on first load, but a
rem release should not depend on that: the first run would rewrite a file the
rem user just unpacked. The repo template is LF (*.template text eol=lf), so
rem the conversion happens here.
powershell -NoProfile -ExecutionPolicy Bypass -File "tools/normalize_crlf.ps1" -Path "%MODDIR%\dlls\DGLab_IDAC.ini" >nul || (echo [x] INI line-ending fix failed & popd & exit /b 1)
findstr /c:"[safety]" "%MODDIR%\dlls\DGLab_IDAC.ini" >nul || (echo [x] staged INI looks empty & popd & exit /b 1)

rem ---- zip --------------------------------------------------------------
if not exist "dist" mkdir "dist"
set "ZIP=dist\DGLab_IDAC-v%VER%.zip"
if exist "%ZIP%" del /q "%ZIP%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%STAGE%\DGLab_IDAC' -DestinationPath '%ZIP%' -CompressionLevel Optimal -Force"
if errorlevel 1 (
    echo [x] Compress-Archive failed
    popd & exit /b 1
)

echo.
echo ============================================================
echo  [ok] %ZIP%
echo ============================================================
call :showhash "%ZIP%"
call :showhash "%MODDIR%\dlls\main.dll"
call :showhash "%MODDIR%\dlls\DGLab_IDAC.ini"
call :showhash "%MODDIR%\README.md"
call :showhash "%MODDIR%\LICENSE"
echo  built from commit %SHA%
echo ============================================================
popd
exit /b 0

:showhash
set "H="
for /f "usebackq tokens=*" %%A in (`powershell -NoProfile -Command "(Get-FileHash -Algorithm MD5 -LiteralPath '%~f1').Hash.ToLower()"`) do set "H=%%A"
for %%S in ("%~f1") do echo   %H%  %%~nxS  %%~zS bytes
exit /b 0
