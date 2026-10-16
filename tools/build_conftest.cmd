@echo off
REM ---------------------------------------------------------------------------
REM 构建 conftest.exe —— 离线校验 DGLab_IDAC.ini 的解析结果。
REM
REM 只编 config.cpp + 一个 logger 的替身, 不链接 UE4SS, 不进游戏。
REM 生成物落在 tools\conftest.exe。
REM
REM 注意: 本脚本在写盘后必须转成 CRLF 行尾, cmd.exe 对 LF 行尾的 if 块
REM 解析会错乱 (表现为 "xxx is not recognized as an internal or external
REM command")。第一次就是这么挂的, 改动后记得:
REM   python -c "p='build_conftest.cmd';d=open(p,'rb').read();open(p,'wb').write(d.replace(b'\n',b'\r\n'))"
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
cl /nologo /std:c++17 /EHsc /O2 /W3 /utf-8 /I"..\src" conftest.cpp "..\src\config.cpp" /Fe:conftest.exe
if errorlevel 1 (
    echo [x] compile failed
    exit /b 1
)

del /q *.obj 2>nul
echo [ok] tools\conftest.exe
