@echo off
rem ============================================================================
rem  install_msvc.cmd - install MSVC x64 build tools to a custom drive
rem ----------------------------------------------------------------------------
rem  WHY CMD INSTEAD OF POWERSHELL:
rem    The user's machine has the Microsoft.WinGet.Client module installed
rem    (D:\sys\Documents\PowerShell\Modules\Microsoft.WinGet.Client\1.8.1133).
rem    PowerShell 7 wires it into the "command not found" feedback provider,
rem    which fires an unconditional background "warm up" on every pwsh start.
rem    The warm up calls the WinGet COM server; on this machine that COM
rem    activation fails with 0x800706BA (RPC server unavailable) and the
rem    exception is raised on a thread pool thread with no handler, which
rem    TERMINATES the whole pwsh process. No PowerShell script can defend
rem    against it. cmd.exe has no such code path, so this script uses cmd.
rem
rem  USAGE
rem    install_msvc.cmd                 install for real
rem    install_msvc.cmd --dry-run       print what would happen, change nothing
rem
rem  NOTES
rem    * No admin rights are needed. The VS installer elevates itself via UAC.
rem    * --path cache= and --path shared= can ONLY be set on the FIRST install
rem      of a given installation; afterwards they are baked in. That is why
rem      they are passed here explicitly and why a "repair" cannot relocate
rem      them later.
rem    * Windows SDK headers/libs and the VS Installer itself are MSI-based and
rem      CANNOT be relocated. They always land on C: (~2.5-4 GB).
rem ============================================================================
setlocal EnableDelayedExpansion

set "DRY_RUN="
if /i "%~1"=="--dry-run" set "DRY_RUN=1"

rem --- Target layout (E: was chosen because it has 200 GB free) ---------------
set "VS_DRIVE=E:"
set "VS_ROOT=E:\VS"
set "BOOTSTRAP=%VS_ROOT%\vs_buildtools.exe"
set "VS_INSTALL=C:\Program Files (x86)\Microsoft Visual Studio"   & rem informational only
set "INSTALL_PATH=%VS_ROOT%\BuildTools"
set "CACHE_PATH=%VS_ROOT%\cache"
set "SHARED_PATH=%VS_ROOT%\shared"
set "TEMP_PATH=%VS_ROOT%\temp"
set "VCTOOLS=%INSTALL_PATH%\VC\Tools\MSVC"
set "BOOTSTRAP_URL=https://aka.ms/vs/17/release/vs_BuildTools.exe"
set "MIN_C_FREE_GB=6"

echo.
echo === MSVC x64 build tools - custom-path install ===
echo   install : %INSTALL_PATH%
echo   cache   : %CACHE_PATH%
echo   shared  : %SHARED_PATH%
echo   temp    : %TEMP_PATH%
if defined DRY_RUN echo   MODE    : DRY RUN - nothing will be downloaded or installed
echo.

rem --- 1. Host Windows version (the VS 2022 installer requires 10 1709+) ------
echo [1/7] Checking host Windows version ...
set "WIN_MAJOR="
set "WIN_MINOR="
for %%T in ('ver') do rem placeholder
rem Extract the version number from the FIRST token of `ver` output whose
rem leading character is a digit -- i.e. "10.0.26200.9457" in
rem   Microsoft Windows [Version 10.0.26200.9457]
rem and NOT "Windows" or "[Version" (the bracket is not a delimiter).
set "VERLINE="
set "VERSN="
for /f "delims=" %%L in ('ver') do if not defined VERLINE set "VERLINE=%%L"
for %%T in (%VERLINE%) do if not defined VERSN (
    set "C=%%T"
    if "!C:~0,1!" GEQ "0" if "!C:~0,1!" LEQ "9" set "VERSN=%%T"
)
if not defined VERSN (
    echo       FAILED: could not parse the output of "ver".
    goto :fail
)
for /f "tokens=1,2 delims=." %%a in ("%VERSN%") do (
    set "WIN_MAJOR=%%a"
    set "WIN_MINOR=%%b"
)
if not defined WIN_MAJOR (
    echo       FAILED: could not parse a version number out of "%VERSN%".
    goto :fail
)
if !WIN_MAJOR! LSS 10 (
    echo       FAILED: Windows !WIN_MAJOR!.!WIN_MINOR! is older than Windows 10 1709.
    echo       The VS 2022 bootstrapper will refuse to run.
    goto :fail
)
echo       OK - Windows !WIN_MAJOR!.!WIN_MINOR!.
set "FREEGB="
set "FSF=%TEMP%\_vs_df.txt"
powershell -NoProfile -NonInteractive -Command "$d=Get-CimInstance Win32_LogicalDisk -Filter ('DeviceID=' + [char]39 + '%VS_DRIVE%' + [char]39); if($d){[math]::Floor($d.FreeSpace/1GB)}" > "%FSF%" 2>nul
if exist "%FSF%" for /f "usebackq tokens=*" %%A in ("%FSF%") do if not defined FREEGB set "FREEGB=%%A"
if exist "%FSF%" del "%FSF%" >nul 2>nul
if not defined FREEGB (
    echo       WARNING: could not read free space on %VS_DRIVE% - skipping that check.
) else (
    if !FREEGB! LSS 12 (
        echo       FAILED: %VS_DRIVE% has only !FREEGB! GB free, need about 12 GB.
        goto :fail
    )
    echo       %VS_DRIVE% has !FREEGB! GB free, need about 12 GB - OK.
)
set "FREEGB="
powershell -NoProfile -NonInteractive -Command "$d=Get-CimInstance Win32_LogicalDisk -Filter ('DeviceID=' + [char]39 + 'C:' + [char]39); if($d){[math]::Floor($d.FreeSpace/1GB)}" > "%FSF%" 2>nul
if exist "%FSF%" for /f "usebackq tokens=*" %%A in ("%FSF%") do if not defined FREEGB set "FREEGB=%%A"
if exist "%FSF%" del "%FSF%" >nul 2>nul
if not defined FREEGB (
    echo       WARNING: could not read free space on C: - skipping that check.
) else (
    if !FREEGB! LSS %MIN_C_FREE_GB% (
        echo       FAILED: C: has only !FREEGB! GB free; the Windows SDK is MSI-based,
        echo       cannot be relocated off C:, and needs about %MIN_C_FREE_GB% GB.
        echo       Free some space, or edit MIN_C_FREE_GB at the top of this script.
        goto :fail
    )
    echo       C: has !FREEGB! GB free, need about %MIN_C_FREE_GB% GB for the Windows SDK - OK.
)


rem --- 3. Download the bootstrapper (unless we already have it) --------------
echo [3/7] Bootstrapper: %BOOTSTRAP%
if exist "%BOOTSTRAP%" (
    echo       Already present - reusing it.
    goto :have_bootstrap
)
if defined DRY_RUN (
    echo       [dry-run] would download %BOOTSTRAP_URL%
    goto :have_bootstrap
)
if not exist "%VS_ROOT%" (
    mkdir "%VS_ROOT%" 2>nul
    if errorlevel 1 (
        echo       FAILED: could not create "%VS_ROOT%".
        echo       Create the folder manually, or edit VS_ROOT at the top of this script.
        goto :fail
    )
)
echo       Downloading ...
where curl.exe >nul 2>nul
if not errorlevel 1 (
    curl.exe -L -f --retry 3 --retry-delay 2 -o "%BOOTSTRAP%" "%BOOTSTRAP_URL%"
) else (
    powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command ^
        "$ProgressPreference='SilentlyContinue'; Invoke-WebRequest -UseBasicParsing -Uri '%BOOTSTRAP_URL%' -OutFile '%BOOTSTRAP%'"
)
if errorlevel 1 (
    echo       FAILED: download failed.
    echo       Download it manually from %BOOTSTRAP_URL% and save it as
    echo         %BOOTSTRAP%
    echo       then run this script again.
    goto :fail
)
:have_bootstrap

rem --- 4. Create the target folders ------------------------------------------
echo [4/7] Preparing folders ...
if defined DRY_RUN goto :folders_done
for %%D in ("%INSTALL_PATH%" "%CACHE_PATH%" "%SHARED_PATH%" "%TEMP_PATH%") do (
    if not exist %%D (
        mkdir %%D 2>nul
        if errorlevel 1 (
            echo       FAILED: could not create %%D
            goto :fail
        )
        echo       created %%D
    )
)
:folders_done

rem --- 5. Install ------------------------------------------------------------
echo [5/7] Installing - THE UI MAY NOT APPEAR FOR SEVERAL MINUTES, please wait.
echo       Verbose log: %TEMP%\dd_*.log
if defined DRY_RUN (
    echo       [dry-run] would run:
    echo         "%BOOTSTRAP%" --quiet --wait --norestart --nocache
    echo             --installPath "%INSTALL_PATH%"
    echo             --path cache="%CACHE_PATH%"
    echo             --path shared="%SHARED_PATH%"
    echo             --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64
    echo             --add Microsoft.VisualStudio.Component.VC.Redist.14.Latest
    echo             --add Microsoft.VisualStudio.Component.Windows11SDK.26100
    echo       [dry-run] skipping the actual install.
    goto :verify
    goto :verify
)
set "TMP=%TEMP_PATH%"
set "TEMP=%TEMP_PATH%"
start /wait "" "%BOOTSTRAP%" --quiet --wait --norestart --nocache ^
    --installPath "%INSTALL_PATH%" ^
    --path cache="%CACHE_PATH%" ^
    --path shared="%SHARED_PATH%" ^
    --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
    --add Microsoft.VisualStudio.Component.VC.Redist.14.Latest ^
    --add Microsoft.VisualStudio.Component.Windows11SDK.26100
set "RC=%ERRORLEVEL%"
echo.
echo       Bootstrapper exit code: %RC%
if "%RC%"=="0"    echo       -> success.
if "%RC%"=="3010" echo       -> success, a REBOOT is required to finish.
if "%RC%"=="1641" echo       -> success, a reboot was started automatically.
if "%RC%"=="1602" echo       -> CANCELLED by the user.
if "%RC%"=="5007" echo       -> a package failed to install ^(see %%TEMP%%\dd_installer_*.log^).
if "%RC%"=="-2147023293" echo       -> signature verification failed; re-download the bootstrapper.
if not "%RC%"=="0" if not "%RC%"=="3010" if not "%RC%"=="1641" goto :fail

rem --- 6. Verify cl.exe actually landed -------------------------------------
:verify
echo [6/7] Verifying cl.exe ...
if defined DRY_RUN (
    echo       [dry-run] would search: %VCTOOLS%\*\bin\Hostx64\x64\cl.exe
    goto :done
)
set "CL_FOUND="
for /f "delims=" %%F in ('dir /b /s "%VCTOOLS%\cl.exe" 2^>nul') do (
    echo %%F | findstr /i "\\Hostx64\\x64\\cl.exe" >nul && set "CL_FOUND=%%F"
)
if defined CL_FOUND (
    echo       OK - %CL_FOUND%
) else (
    echo       NOT FOUND under %VCTOOLS%
    echo       The install may have needed a reboot, or a component was skipped.
    goto :fail
)

rem --- 7. Print the one-liner that makes cl usable in a new shell -----------
echo [7/7] Environment for a new shell:
set "VCVARS="
for /f "delims=" %%F in ('dir /b /s "%INSTALL_PATH%\vcvars64.bat" 2^>nul') do set "VCVARS=%%F"
if defined VCVARS (
    echo       call "%VCVARS%"
) else (
    echo       vcvars64.bat was not found - check the install.
)

:done
echo.
if defined DRY_RUN (
    echo === DRY RUN complete - nothing was changed. ===
) else (
    echo === DONE. Build with:  tools\build.cmd ===
)
exit /b 0

rem ===========================================================================
:fail
echo.
echo === FAILED - nothing further was attempted. ===
echo     Full installer logs: %TEMP%\dd_*.log
exit /b 1
