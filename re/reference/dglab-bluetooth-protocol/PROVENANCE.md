# 来源与校验 —— DG-LAB 官方蓝牙协议文档

**本目录不再包含上游文档的副本。** 只保留这份说明: 上游 URL、快照提交号、每份文件的
字节数与 sha256 前缀, 以及带提交号的直链。

以前这里放过逐字节副本(便于离线核对), 但上游没有 LICENSE 文件, 把别人的文档再分发一遍
责任不清, 所以在 v0.2.1 里删掉了, 改成"链接 + 校验和"。协议事实本身没有变, 只是获取方式
从"看本仓库"变成"看上游同一提交"。

## 上游

| | |
|---|---|
| 仓库 | [`dungeonlab-open/dglab-bluetooth-protocol`](https://github.com/dungeonlab-open/dglab-bluetooth-protocol) |
| 分支 | `main` |
| 快照提交 | [`045720d5c5ee4879e25b7bad555c962f97ed423f`](https://github.com/dungeonlab-open/dglab-bluetooth-protocol/tree/045720d5c5ee4879e25b7bad555c962f97ed423f) (2026-10-16, `fix: markdown formatting`) |
| 抓取日期 | 2026-10-17 |

上游 `README.md` 写明: 这些协议面向爱好者公开, **商业用途需事先授权**;
技术咨询走 QQ `3849540080`。上游**没有 LICENSE 文件**, 所以引用它不套用本仓库根目录的
MIT LICENSE。

## 文档对照表

下表里的链接都钉在上面的快照提交上, 以后上游改文件也不会让链接失效。
sha256 是 2026-10-17 抓下来的内容(CRLF 原样)的哈希, 用来判断"上游这份文件后来有没有变"。

| 上游路径 | 字节 | sha256 (前 16) |
|---|---|---|
| [`README.md`](https://github.com/dungeonlab-open/dglab-bluetooth-protocol/blob/045720d5c5ee4879e25b7bad555c962f97ed423f/README.md) | 2399 | `ae59674f32d04857` |
| [`README_CN.md`](https://github.com/dungeonlab-open/dglab-bluetooth-protocol/blob/045720d5c5ee4879e25b7bad555c962f97ed423f/README_CN.md) | 2205 | `da161485da8ccebb` |
| [`coyote/v3/README.md`](https://github.com/dungeonlab-open/dglab-bluetooth-protocol/blob/045720d5c5ee4879e25b7bad555c962f97ed423f/coyote/v3/README.md) | 16196 | `d52479df26707d46` |
| [`coyote/v3/README_CN.md`](https://github.com/dungeonlab-open/dglab-bluetooth-protocol/blob/045720d5c5ee4879e25b7bad555c962f97ed423f/coyote/v3/README_CN.md) | 16747 | `b398c91c4c2078fb` |
| [`coyote/README.md`](https://github.com/dungeonlab-open/dglab-bluetooth-protocol/blob/045720d5c5ee4879e25b7bad555c962f97ed423f/coyote/README.md) | 5552 | `f053c7948ec26692` |
| [`coyote/README_CN.md`](https://github.com/dungeonlab-open/dglab-bluetooth-protocol/blob/045720d5c5ee4879e25b7bad555c962f97ed423f/coyote/README_CN.md) | 5148 | `bdf9028d386f306b` |

根 `README.md` 也被引用: `docs/research-dglab-transport.md` 与 `docs/research-coyote-protocol.md`
里提到它的 `#L13` / `#L15`(公开路径声明与商用授权声明)。

英文版与中文版都在表里, 因为上游更新日志记着 `2026/10/15: Corrected errors in the Civet,
Coyote, and Pawprint protocol documentation and examples.` —— 两份都看才能分辨
"翻译不一致"与"真的改了语义"。

## 怎么重新校验

```bash
# 干净地取一份上游快照(不改动本仓库)
git clone https://github.com/dungeonlab-open/dglab-bluetooth-protocol.git /tmp/dglab-proto
cd /tmp/dglab-proto && git checkout 045720d5c5ee4879e25b7bad555c962f97ed423f

# 和上表的 sha256 对一下(注意上游是 CRLF, 不要先做行尾转换)
sha256sum README.md README_CN.md coyote/v3/README.md coyote/v3/README_CN.md \
          coyote/README.md coyote/README_CN.md
```

要按行号读某一节, 直接用上表里的 GitHub 链接(带提交号), 或者在 clone 出来的目录里
`grep -n` —— 本仓库 `docs/` 与源码注释里的 `#Lnnn` 都是相对那份快照的。

## 改代码前先看这里

协议里有几处**极易读反**的地方, 我们的实现曾经真的读反过 (见 `src/transport/transport.hpp`
里 `StrengthMode` 的注释):

1. **强度解读方式的位序**: 官方正文写"高两位代表 A 通道, 低两位代表 B 通道", 但
   `coyote/v3/README.md` 里那 6 个例子全部是 **A = bits 3-2, B = bits 1-0**。**以例子为准。**
2. **`0b11` 才是"绝对设置"**, `0b00` 是"不改动, 忽略强度字节"。这两个写反的后果是
   设备对强度指令毫无反应, 而且编译、运行、自检全都正常。
3. **模式位是"每通道 2 bit"**, 不是每通道 4 bit。整个 byte[1] 的高 4 bit 是序列号。
4. 波形频率 10–240 送入设备后**不是线性对应到电气频率**; 数值越大, 单个脉冲的时长越长,
   电气频率反而越低。换算表在 `coyote/v3/README.md` 的 Pulse Frequency 一节。

改动编码器之后请务必跑 `tools/protocol_check.exe` (54 项断言, 基准就是这份上游文档)。
