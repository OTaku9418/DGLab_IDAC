#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# run_cmd.sh -- 调用 .cmd/.bat，绕开 Git Bash 的两大坑
#
# 为什么需要这个包装:
#  1. `cmd //c "a && b"` 里 bash 会先做自己的分词, 引号很容易被吃掉一层。
#     正确形式是 `cmd //c 'a && b'`(单引号), 或者干脆写成 .cmd 文件再调用。
#  2. 更阴险的是 MSYS 的路径改写: cmd 命令里出现的 `/xxx:yyy` 形参会
#     被 MSYS 当成 unix 路径处理 —— 冒号被剥掉, 剩下的部分变成裸词。
#     实测 `lib ... /out:D:\...\UE4SS.lib` 里的 `/out:` 被改写成 `\UE4SS.lib`,
#     cmd 把裸词 `\UE4SS.lib` 当成**输出重定向目标**, 于是 lib.exe 的横幅
#     被写进了 .lib 文件(得到一个 10 字节、内容为 "    OK -" 的假 .lib)。
#     MSYS2_ARG_CONV_EXCL='*' 可以关掉这个改写。
#
# 用法: tools/run_cmd.sh <script.cmd> [args...]
# ---------------------------------------------------------------------------
set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "usage: $0 <script.cmd> [args...]" >&2
    exit 2
fi

SCRIPT="$1"; shift

# 绝对化, 并转成 Windows 反斜杠形式给 cmd.exe
case "$SCRIPT" in
    /*|[A-Za-z]:*) ;;
    *) SCRIPT="$PWD/$SCRIPT" ;;
esac
WIN_SCRIPT="$(cygpath -w "$SCRIPT" 2>/dev/null || echo "$SCRIPT")"

# MSYS2_ARG_CONV_EXCL='*' -> 不要再改写任何 /xxx 形参
export MSYS2_ARG_CONV_EXCL='*'
export MSYS_NO_PATHCONV=1

if [[ $# -gt 0 ]]; then
    exec cmd //c "$WIN_SCRIPT" "$@"
else
    exec cmd //c "$WIN_SCRIPT"
fi
