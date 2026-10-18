# `UE4SS.def` 从哪来

这份 `.def` 是 **`UE4SS.dll` 的导出符号表**, 用来在**没有装游戏、也没有 `UE4SS.dll`
的机器上**生成链接用的导入库 `UE4SS.lib`。构建时链接的是导入库, 不是 DLL 本身。

## 为什么提交文本 .def 而不是 2 MB 的 .lib

- `.lib` 是 `lib.exe` 从 `.def` 生成的产物, 大小约 2.1 MB, 且 MSF 头里带时间戳,
  换台机器重新生成 md5 就不一样 —— 提交二进制会把仓库搞脏, 也没法 review。
- `.def` 只有符号名, 纯文本, 342 KB, 任何改动都能在 diff 里看见。
- `tools\bootstrap.cmd` 的第三步会用它现场生成 `third_party/importlib/UE4SS.lib`
  (生成时机: 首次构建前; 已存在则跳过)。实测 `lib /def` 生成的文件与
  "从真实 DLL 导出"得到的 `.lib` **大小完全相同 (2,155,224 字节)**, 链接与两个
  自检程序(protocol_check / logic_check)全部通过。

## 源 `UE4SS.dll` 的身份

| 项 | 值 |
|---|---|
| 路径 | `<游戏>\GameProject\Binaries\Win64\ue4ss\UE4SS.dll` |
| 大小 | 16,259,072 字节 |
| SHA-256 | `DF9E6E9A2280972B1C28CE590700FEACC752B447204F8BAADEB95F5776957055` |
| PE `TimeDateStamp` | `0x69C69775` (2026-03-27T14:43:01Z) |
| 导出函数数 | 4067 |
| UE4SS 版本 | `v3.0.1-944-g0196ef29` —— 来自随包文件名 `UE4SS_v3.0.1-944-g0196ef29.zip`; 对应上游 commit `0196ef294f852` |

配套头文件在 `_ue4ss_ref/ue4ss_0196ef29/` 与 `_ue4ss_ref/ue4ss_0196ef29_deps/`,
是同一个 commit 的快照。**不要用上游 tag v3.0.1 (`d935b5b`) 顶替**: 两者相差 944
个提交, `CppUserModBase` 的虚函数表不同, 混用会编译通过、运行时踩错 vtable 槽。

> 复核记录 (2026-10-18): 第二轮审查计划里写的 `TimeDateStamp 0x69392E86`
> 与本机这份 DLL **不一致**。上面表格里的 `0x69C69775` 是从文件头 `e_lfanew + 8`
> 直接读出来的实际值; 若将来换了 DLL, 请以实际读数为准并同步更新本节。
>
> 来源已查清 (第三轮 T20): `0x69392E86` 是**第一轮报告误记** —— 它是
> `Plugins\IndRun\Binaries\Win64\IndRun.dll`(2,634,240 字节)的 PE 时间戳,
> 不是 `UE4SS.dll` 的; 第二轮计划沿用了这个误记。本机 `UE4SS.dll` 的 SHA-256
> 已用 `certutil -hashfile` 复核, 与上表一致。

## 生成方式

脚本是 `tools\make_ue4ss_implib.cmd` (可以传 DLL 路径, 缺省用开发机路径):

```
dumpbin /exports UE4SS.dll        ->  exports_full.txt
findstr 过滤出 "ordinal hint RVA name" 四列的真实行   ->  exports_raw.txt
写 UE4SS.def: 第一行 LIBRARY UE4SS, 第二行 EXPORTS, 其余每行一个符号名
lib /def:UE4SS.def /machine:x64 /out:UE4SS.lib
```

两个刻意的细节:

- `findstr` 的过滤条件要求 hint 至少 3 位十六进制 —— 因为表头那行是
  `4067 number of functions`, 只有 2 位, 用这个条件正好把它排除掉。
- 用 `cmd //c` 从 Git Bash 调这个脚本时, MSYS 会把 `/out:D:\...` 当成 unix 路径
  重写, 结果 `lib.exe` 的横幅被写进 `.lib`。所以脚本末尾有文件大小护栏。

## 校验

2026-10-18 用 `dumpbin /exports` 对**当前** `UE4SS.dll` 重新导出, 与仓库里这份
`.def` 逐条比对:

```
dumpbin 符号数 4067 / .def 条目数 4067
只在 DLL 里有 0 条, 只在 .def 里有 0 条
```

也就是说这份 `.def` 与上述 SHA-256 的那个 DLL 是严格对应的。

## 许可

- UE4SS (RE-UE4SS) 是 **MIT** 许可: <https://github.com/UE4SS-RE/RE-UE4SS>
- 本文件只含**导出符号名** (函数/变量的修饰名), 不含 UE4SS 的任何实现代码;
  它不适用本仓库根目录的 MIT `LICENSE`, 而适用上游的 MIT。
- 顺带确认 (只检查, 未改动): `_ue4ss_ref/third_party/fmt/LICENSE` 存在,
  1431 字节, 是 {fmt} 11.2.0 的原版 MIT 许可。fmt 的来历详见
  `_ue4ss_ref/third_party/fmt/PROVENANCE.md`。

## ★ 换 UE4SS 版本时必须同时替换两样东西 ★

1. `_ue4ss_ref/ue4ss_0196ef29/` + `_ue4ss_ref/ue4ss_0196ef29_deps/` 里的对齐头文件;
2. 这份 `third_party/importlib/UE4SS.def` (用新 DLL 重新跑
   `tools\make_ue4ss_implib.cmd` 覆盖它)。

只换一样会出现"编译通过、链接通过、进游戏崩"或者"导入了不存在的符号"——
前者是 vtable 槽位错位, 后者是链接期就该报出来但很容易被当成别的问题。
