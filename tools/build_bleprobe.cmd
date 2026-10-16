@echo off
REM ---------------------------------------------------------------------------
REM 构建 bleprobe.exe —— 脱离游戏单独跑一遍 BLE 链路。
REM
REM 链接的是 src\transport\ble_winrt.cpp 本体, 所以验过的就是游戏里那份代码。
REM 需要 windowsapp.lib (WinRT 激活) 和 runtimeobject.lib。
REM
REM 行尾必须是 CRLF, 否则 cmd.exe 的 if 块会解析错乱。
REM ---------------------------------------------------------------------------
setlocal

set "VS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
if not exist "%VS%\VC\Auxiliary\Build\vcvars64.bat" set "VS=E:\VS\BuildTools"
if not exist "%VS%\VC\Auxiliary\Build\vcvars64.bat" (
    echo [x] cannot find vcvars64.bat
    exit /b 1
)

call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

cd /d "%~dp0"
cl /nologo /std:c++17 /EHsc /O2 /W3 /utf-8 /I"..\src" ^
   bleprobe.cpp ^
   "..\src\logger.cpp" ^
   "..\src\config.cpp" ^
   "..\src\transport\transport.cpp" ^
   "..\src\transport\transport_factory.cpp" ^
   "..\src\transport\null_transport.cpp" ^
   "..\src\transport\ble_winrt.cpp" ^
   /Fe:bleprobe.exe ^
   /link windowsapp.lib runtimeobject.lib ole32.lib setupapi.lib bcrypt.lib psapi.lib shell32.lib
if errorlevel 1 (
    echo [x] compile failed
    exit /b 1
)

del /q *.obj 2>nul
echo [ok] tools\bleprobe.exe
