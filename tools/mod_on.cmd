@echo off
REM Re-enable DGLab_IDAC mod.
setlocal
set "MODDIR=%~1"
if "%MODDIR%"=="" set "MODDIR=D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\Mods\DGLab_IDAC"
if exist "%MODDIR%\dlls.off" (
    if exist "%MODDIR%\dlls" rmdir /s /q "%MODDIR%\dlls"
    move /y "%MODDIR%\dlls.off" "%MODDIR%\dlls" >nul
    echo [mod_on] dlls.off -^> dlls : mod is now ENABLED
) else (
    echo [mod_on] nothing to restore (dlls.off not found)
)
endlocal
