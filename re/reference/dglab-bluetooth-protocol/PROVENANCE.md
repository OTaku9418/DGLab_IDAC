# 来源与校验 —— DG-LAB 官方蓝牙协议文档

本目录下的 Markdown 文件是 **DG-LAB 官方协议文档的逐字节副本**, 不是我们的改写。
放一份在本仓库里, 是为了让 `src/transport/transport.cpp` 里那些编码常量和
`tools/protocol_check.cpp` 的断言有一个**离线、可回溯**的依据 —— 不用联网也能核对。

## 上游

| | |
|---|---|
| 仓库 | [`dungeonlab-open/dglab-bluetooth-protocol`](https://github.com/dungeonlab-open/dglab-bluetooth-protocol) |
| 分支 | `main` |
| 快照提交 | `045720d5c5ee4879e25b7bad555c962f97ed423f` (2026-10-16, `fix: markdown formatting`) |
| 抓取日期 | 2026-10-17 |

上游 `README.md` 写明: 这些协议面向爱好者公开, **商业用途需事先授权**;
技术咨询走 QQ `3849540080`。上游**没有 LICENSE 文件**, 所以这里不能套用某个 SPDX 标识 ——
本目录的文件沿用上游的授权条件, 不受本仓库根目录 MIT LICENSE 覆盖。

## 本地副本对照表

| 本地文件 | 上游路径 | 字节 | sha256 (前 16) |
|---|---|---|---|
| `root-README.md` | `README.md` | 2399 | `ae59674f32d04857` |
| `root-README_CN.md` | `README_CN.md` | 2205 | `da161485da8ccebb` |
| `coyote-v3-README.md` | `coyote/v3/README.md` | 16196 | `d52479df26707d46` |
| `coyote-v3-README_CN.md` | `coyote/v3/README_CN.md` | 16747 | `b398c91c4c2078fb` |
| `coyote-pulse-README.md` | `coyote/README.md` | 5552 | `f053c7948ec26692` |
| `coyote-pulse-README_CN.md` | `coyote/README_CN.md` | 5148 | `bdf9028d386f306b` |

根 `README.md` 也存了一份, 因为 `docs/research-dglab-transport.md` 与
`docs/research-coyote-protocol.md` 里引用了它的 `#L13` / `#L15` (公开路径声明与商用授权声明),
不存就得联网才能核对那两行。

英文版与中文版**都在**, 因为上游 README 的更新日志明确记着
`2026/10/15 -> 修正灵猫、郊狼和爪印的协议文档及示例` (中) /
`2026/10/15: Corrected errors in the Civet, Coyote, and Pawprint protocol documentation and examples.` (英)。
两份都留着, 出现分歧时可以直接对照是"翻译不一致"还是"真的改了语义"。

## 怎么重新校验

```bash
# 本地副本有没有被人手改过
cd re/reference/dglab-bluetooth-protocol
sha256sum *.md

# 和上游比 (需要能访问 GitHub)
git clone --depth 1 --filter=blob:none --sparse \
    https://github.com/dungeonlab-open/dglab-bluetooth-protocol.git /tmp/dglab-proto
cd /tmp/dglab-proto && git sparse-checkout set coyote
diff /tmp/dglab-proto/coyote/v3/README.md <本目录>/coyote-v3-README.md && echo "未改动"
```

`.gitattributes` 里对本目录打了 `-text`, 禁止 git 做行尾转换 ——
否则 checkout 时 CRLF 会被换成 LF, 上面的 `diff` 立刻报"整份文件都不同",
把真正的语义改动淹掉。**上游文件是 CRLF, 请保持原样。**

## 改代码前先看这里

协议里有几处**极易读反**的地方, 我们的实现曾经真的读反过 (见 `src/transport/transport.hpp`
里 `StrengthMode` 的注释):

1. **强度解读方式的位序**: 官方正文写"高两位代表 A 通道, 低两位代表 B 通道", 但
   `coyote-v3-README.md` 里那 6 个例子全部是 **A = bits 3-2, B = bits 1-0**。**以例子为准。**
2. **`0b11` 才是"绝对设置"**, `0b00` 是"不改动, 忽略强度字节"。这两个写反的后果是
   设备对强度指令毫无反应, 而且编译、运行、自检全都正常。
3. **模式位是"每通道 2 bit"**, 不是每通道 4 bit。整个 byte[1] 的高 4 bit 是序列号。
4. 波形频率 10–240 送入设备后**不是线性对应到电气频率**; 数值越大, 单个脉冲的时长越长,
   电气频率反而越低。换算表在 `coyote-v3-README.md` 的 Pulse Frequency 一节。

改动编码器之后请务必跑 `tools/protocol_check.exe` (54 项断言, 基准就是这几份官方文档)。
