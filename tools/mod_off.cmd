@echo off
REM Disable DGLab_IDAC mod by renaming its dlls folder. UE4SS will fail to find
REM dlls\main.dll and simply skip the mod. Nothing else is touched.
setlocal
set "MODDIR=%~1"
if "%MODDIR%"=="" set "MODDIR=D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\Mods\DGLab_IDAC"
if exist "%MODDIR%\dlls" (
    if exist "%MODDIR%\dlls.off" rmdir /s /q "%MODDIR%\dlls.off"
    move /y "%MODDIR%\dlls" "%MODDIR%\dlls.off" >nul
    echo [mod_off] dlls -^> dlls.off : mod is now DISABLED
) else (
    echo [mod_off] already disabled or path wrong: %MODDIR%\dlls not found
)
endlocal
