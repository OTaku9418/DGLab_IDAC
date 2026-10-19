# 郊狼主机 (DG-LAB Coyote) 接入调研

来源：官方仓库 [`dungeonlab-open/dglab-bluetooth-protocol`](https://github.com/dungeonlab-open/dglab-bluetooth-protocol)
（本次分析基于上游快照提交 `045720d`，逐份链接与 sha256 见 `re/reference/dglab-bluetooth-protocol/PROVENANCE.md`；仓库不再包含副本）
+ 官方/社区实现 `hyperzlib/DG-Lab-Coyote-Game-Hub`、`dungeonlab-open/dglab-kit(-python)`、`Ljzd-PRO/PyDGLab-WS`。
证据标注：**[L]** = 官方仓库原文事实，**[W]** = 网上确认，**[I]** = 推断/待硬件验证。

---

## 1. 两条传输路径

### 路径 A：PC 直接做 BLE 中央设备（推荐，无需手机）

第三方直连 BLE 是官方文档明确支持的路径（协议全文公开，仅商用需授权，`README.md:15` [L]）。

**V3（郊狼 3.0，蓝牙名 `47L121000`；无线传感器 `47L120100`）**
基础 UUID：`0000xxxx-0000-1000-8000-00805f9b34fb`

| 服务 | 特性 | 属性 | 长度 | 用途 |
|---|---|---|---|---|
| `0x180C` | `0x150A` | 写 | ≤20 B | **所有指令写入此处**（B0 / BF） |
| `0x180C` | `0x150B` | 通知 | ≤20 B | 所有回应（B1） |
| `0x180A` | `0x1500` | 读/通知 | 1 B | 电量 0–100 |

**V2（蓝牙名 `D-LAB ESTIM01`）** 基础 UUID `955Axxxx-0FE2-F5AA-A094-84B8D4F3E8AD`

| 服务 | 特性 | 属性 | 长度 | 用途 |
|---|---|---|---|---|
| `955a180a-…` | `955a1500-…` | 读/通知 | 1 B | 电量 |
| `955a180b-…` | `955a1504-…` | 读/写/通知 | 3 B | `PWM_AB2` A/B 强度 S |
| `955a180b-…` | `955a1505-…` | 读/写 | 3 B | `PWM_A34` 波形 |
| `955a180b-…` | `955a1506-…` | 读/写 | 3 B | `PWM_B34` 波形 |

V3 指令 ≤20 B，默认 ATT MTU(23) 即可，无需 MTU 协商。Windows 侧可用 WinRT、Python `bleak`（Windows 后端就是 WinRT 投影）、C++ SimpleBLE、Rust `btleplug`。

> ⚠️ **0x1505/0x1506 的通道归属，官方表格自相矛盾**：`README.md:16-17` 把 `PWM_A34`(0x1505) 标为"B 通道波形数据"却在位域里写 `Az/Ay/Ax`；社区参考实现映射为 `0x1505→A`、`0x1506→B`。**必须在硬件上用极低 S + `Z=20` 单通道试出来**，不能照抄文档。[W, UNCONFIRMED]

**结论：目标 V3。** V2 无统一帧、要 3 次写入/100ms、有字节序陷阱和上述标注矛盾。

### 路径 B：官方 Socket API / WebSocket 中继（必须有手机 App）

不存在 `ws://<手机IP>:<端口>` 这种模式——**始终是 App 主动拨号**到你的 WS 服务器，URL 通过二维码下发。

- **V3 世代（App 3.x，规范仓库 `dglab-websocket-simple` 已归档）**：二维码 payload `https://www.dungeon-lab.com/app-download.php#DGLAB-SOCKET#<urlencoded ws url>`；心跳 60 s；消息为扁平 JSON（`type`/`clientId`/`targetId`/`message` 四个非空字段）；字符串 >1950 字符被丢弃（错误 405）；强度指令是字符串 `strength-<通道>+<模式>+<值>`（模式 0=减 1=加 2=设置）；波形 `A:["0A0A0A0A64646464", …]`，**每个 8 字节十六进制帧 = 100 ms**，数组 ≤100 帧，App 队列 ≤500 帧，发包 ≤10/s（超速惩罚默认 1 次 / 5 s）。
- **V4 世代（DG-LAB 4 App）**：服务器 `wss://trex.dungeon-lab.cn/v4`（自建 :9998）；`{type:"hello", clientId}` 握手，App 用 `?tid=` 接入；心跳 30 s；**空闲 5 min 断线**；消息为 `{type:"message", clientId, data}` 信封，设备操作用 `t` 标记：`t:0 AppendPulseData`、`t:3 AddIntensity`、`t:4 SetTempIntensity`、`t:7 SetIntensity`、`device.op.clear`。

---

## 2. 波形数据模型（转换器要产出的东西）

### V3 的 `B0` 指令：20 字节，每 100 ms 一帧

| 偏移 | 位 | 字段 | 范围 |
|---|---|---|---|
| 0 | — | 头 | `0xB0` |
| 1 | 7–4 | 序列号 | `0b0000`–`0b1111`；非 0 则设备回 B1（同序列号） |
| 1 | 3–2 | A 通道强度解读方式 | `00` 不变 / `01` 相对加 / `10` 相对减 / `11` 绝对设置 |
| 1 | 1–0 | B 通道强度解读方式 | 同上 |
| 2 | 0–7 | A 强度设定值 | 0–200，**超范围按 0 处理** |
| 3 | 0–7 | B 强度设定值 | 0–200 |
| 4–7 | — | A 波形频率 ×4 | 各 10–240，**每组代表 25 ms** |
| 8–11 | — | A 波形强度 ×4 | 各 0–100 |
| 12–15 | — | B 波形频率 ×4 | 10–240 |
| 16–19 | — | B 波形强度 ×4 | 0–100 |

规则：
- 某通道 8 个值里**只要有 1 个越界，该通道 4 组全部被丢弃**（该通道无输出）。只输出单通道时，给另一通道塞一个非法值（例如波形强度 101，官方例子就这么干）。
- 非 0 序列号改强度时，**要等 B1 回来再发下一次强度修改**（同一时刻只允许一个未完成项）；期间波形字节照常每 100 ms 流。
- `B1`（在 `0x150B`）：`0xB1` + 序列号 + A 实际强度 + B 实际强度。`BE` 已废弃。

### `BF` 指令：7 字节，**每次重连后必须重发**，立即生效，无回应

| 偏移 | 字段 | 范围 |
|---|---|---|
| 0 | `0xBF` | — |
| 1–2 | A 软上限, B 软上限 | 0–200，超范围则不修改；**断电保存** |
| 3–4 | A/B 波形频率平衡参数 | 0–255，越大低频冲击感越强 |
| 5–6 | A/B 波形强度平衡参数 | 0–255，越大低频刺激越强 |

A-then-B 的字节顺序是按文档措辞推断的 **[I]**，需硬件确认一次。软上限是最后一道安全阀——滚轮和 B0 都无法突破它。

### 频率换算（输入 10–1000 → 线上字节）

```
10..100   -> v              (原样)
101..600  -> (v-100)/5+100
601..1000 -> (v-600)/10+200
else      -> 10
```

**线上字节 = 输出单元时长(ms)**，所以电气脉冲频率 ≈ `1000/value` Hz：10→100 Hz、20→50 Hz、50→20 Hz、100→10 Hz、240≈4.2 Hz。**字节值越小频率越高**，是典型的符号反向 bug 来源。

> 文档承认：当某个脉冲时长超过输出窗口且下一个值不同时，实际输出可能既不像旧值也不像新值。要维持稳定频率就**持续发同一个值**。因此实现要用严格 100 ms 调度器，丢拍时**重发上一帧**而不是跳过。

### V2 的 S / X / Y / Z（仅作参考，不采用）

- **S（强度）** 在 `PWM_AB2`(0x1504) 3 字节：bit 23–22 保留、**21–11 = A 实际强度**、**10–0 = B 实际强度**；范围 0–2047，写入即生效且持久。
- **X/Y/Z（波形）** 在 `PWM_A34`/`PWM_B34`：bit 23–20 保留、**19–15 = Z**、**14–5 = Y**、**4–0 = X**。`Frequency = X + Y`（10–1000 ms 单位），`X = sqrt(Frequency/1000)*15`，`Y = Frequency - X`；X:Y 超过 1:9 体感变弱。`Z` = 脉冲宽度，单位 5 µs（`Z=20` → 100 µs），**Z > 20 会刺痛**。每组 `[X,Y,Z]` 只有效 0.1 s，必须每 100 ms 重发。
- **字节序陷阱**：3 字节特性是**小端**，即 `(Z<<15)|(Y<<5)|X` 的 24 位大端表示要倒过来。V3 文档明说"与 V2 不同，无需大小端转换"。例子：`x=1,y=9,z=0` → `0x000121` → 字节 `21 01 00`。
- 文档里 `example.md` 的 "strength" 指的是**波形强度**（Z，或 V3 的 0–100），不是通道强度 S，别混。
- V2→V3 换算：V3 频率 = 把 V2 的 `(X+Y)` 过上面那张表；V3 波形强度 = `Z * 5`（V2 的 20 ≈ V3 的 100）。

---

## 3. 赛车游戏该长什么样（波形设计）

官方示例给的参考形状 **[L, `coyote/v3/example.md:7-45`]**：

- **呼吸**：固定频率 10（值 `0A`，即 100 Hz 电气 / 10 ms 单元），波形强度爬升 `0,20,40,60,80,100,100,100,0,0,0,0` —— 12 帧 × 100 ms = 1.2 s 一个周期。
- **潮汐**：更慢的涌动，V3 频率从 10 走到 42（值 `0A…2A`），强度 `0,16,33,50,66,83,100,92,84,76,68` 后提高半步重复。
- **教训：频率变化的体感是慢的**（每 100 ms 最多变几个单位），**强度/脉宽的台阶才产生"打击感"**。

推荐设计（强度都是通道幅度；**绝不让游戏数据直接写绝对通道强度**）：

| 信号 | 通道 | 脉冲频率（电气 → 线上字节） | 波形强度 | 通道强度贡献 |
|---|---|---|---|---|
| 过弯负载/抓地（持续） | A | 40–70 Hz → 字节 **25–14**（起步 50 Hz = 20），4 组相同 | 20 → 65，随负载² 线性，上限 70 | 仅慢速微调：用户手动基数 ±0–15，每 100 ms 斜率 ≤1–2 |
| 路肩/颠簸 | B | 15–25 Hz → 字节 **66–40** | 20 → 40，加颤音调制 | 通常不贡献 |
| 撞击/碰撞 | B | 8–12 Hz → 字节 **125–83** | 50 → 85 | 一次性 +3…+8，~1 s 内必须降回 |
| 怠速/滑行 | 双 | 保持上一帧，或 A=20 / B 非法 | A 15，B `{0,0,0,101}`（关闭） | 不变 |

理由与安全整形：过弯能量放在"平滑嗡鸣"频段（≳40 Hz），低频（≤20 Hz）留给离散事件，因为低频会被感知成一下一下的抽动；文档明确说**脉宽越宽越刺痛**（Z>20 / V3 强度 ≳100），且频率平衡参数越大会让低频更强。负载通道用 ≈100–200 ms 起振、300–600 ms 释放的包络；撞击用 ≈250 ms 指数衰减并硬性限时 ≤300 ms；事件通道的贡献要封顶，避免撞车和路肩叠加。已推导的 20 字节示例帧 **[I]**：

```
过弯 : B0 14 02 00 | 14 14 14 14 | 2D 2D 2D 2D | 00 00 00 00 | 00 00 00 65   (B 通道关闭)
       = B0140200141414142D2D2D2D0000000000000065
撞击 : B0 21 00 05 | 14 14 14 14 | 2D 2D 2D 2D | 64 64 64 64 | 46 46 46 46   (B = 10 Hz, 强度 70)
       = B0210005141414142D2D2D2D6464646446464646
```

（byte1 = 序列号<<4 | A方式<<2 | B方式；`0x14` = 序列号 1 + A"相对加"，`0x21` = 序列号 2 + B"相对加"。同一时刻只允许一个非 0 序列号的强度修改在途。）

---

## 4. 延迟与架构

| 传输 | 实际"信号变化 → 体感输出" | 主要来源 |
|---|---|---|
| 直接 BLE（PC 做中央） | 波形内容 **~110–250 ms**；通道强度 **~150–350 ms** | 自己的 100 ms 量化（主导）；BLE 连接间隔 + 主机栈 ~10–40 ms；设备的 25 ms 窗口；强度需要 B0→B1→下一次 B0 握手（≥2 拍） |
| Edge/Chrome 的 Web Bluetooth | 在上一条基础上 +0–30 ms | 浏览器主线程调度；需安全上下文（`https://` 或 `http://localhost`） |
| Socket API，服务器在同一局域网 | **~150–350 ms** | Wi-Fi 局域网 RTT 2–10 ms + App 处理 10–30 ms + App 的 100 ms 调度 + App→设备 BLE |
| Socket API 走官方中继（`ws.dungeon-lab.cn` / `trex.dungeon-lab.cn`） | **~200–500 ms**，移动网络更差 | 公网 RTT + 中继一跳 + App 侧调度 |

Windows **无法**任意调 BLE 连接间隔：WinRT 只提供 `BluetoothLEDevice.RequestPreferredConnectionParameters(...)` 加几个预设（Balanced / ThroughputOptimized / PowerOptimized / AlwaysConnected，Win11）。

**建议架构（直连 BLE）**：

```
游戏 FFB/遥测线程 (60–120 Hz)
   → 每 100 ms 聚合器: 取窗口 max/rms 作为"负载", 事件队列(撞击/路肩), 平滑
       * 起振/释放包络, 斜率限制器(每拍 ≤1–2 强度单位), 硬钳位
   → 帧构造器: 每通道 4×(频率,强度) + 方式/序列号记账(一个在途 B1)
   → BLE 写入器: 100 ms 单调定时器, write-without-response, 20 字节 B0
   → 看门狗: 250 ms 无游戏数据 → 方式 0b1100(设为 0) 配新序列号, 或保持一帧无害值
```

**缓冲/平滑必须放在帧构造器之前的聚合器里，绝不能放在 BLE 写入器里。** 队列不要超过 ~2 帧，排队会把延迟变成"过时的刺激"。强度变化不要放进每帧路径（它需要 B1 握手）；波形内容本身永远是 100 ms 旧的，最坏 ~125 ms 到电极。

---

## 5. 环境依赖

### 方案 A：PC 直接做 BLE 中央（推荐，完全不需要手机）

- Windows 10 1709+（WinRT GATT 客户端），想要 `RequestPreferredConnectionParameters` 则要 Win11；蓝牙 4.0+ 适配器（板载 Intel 即可，部分廉价 USB 网卡 WinRT 栈不稳）。
- Python 3.11+（64 位）+ `pip install bleak`（当前 3.0.2；Windows 后端 = PyWinRT）；可选 `numpy`/`scipy` 做遥测滤波；只有同时暴露 socket 路径才需要 `qrcode`。
- GATT 访问通常**不需要配对**；设备扫不到时切换蓝牙开关、清理陈旧配对记录或给郊狼断电重启。
- 备选技术栈：C++/WinRT 或 SimpleBLE；Rust `btleplug`；Node 走 Edge/Chrome 的 Web Bluetooth（需安全上下文）。
- 权限：BLE 本身无需管理员（装 USB 网卡驱动可能要提权）。

### 方案 B：Socket API（必须有手机）

- Windows：Python 3.11+ + `pip install websockets`（和 `qrcode` 渲染配对二维码），或 `pip install pydglab-ws`（BSD-3，自带服务器 + 二维码），或 Node 18+ 跑官方 `dglab-websocket-server`（Bun，V3 :9999 / V4 :9998）。
- 手机：官方 **DG-LAB** App —— V3 socket 协议用 3.x，V4 用 DG-LAB 4（`dglab-kit`）。App 才是 BLE 中央。
- 网络：手机必须能访问你的服务器 URL（局域网 IP，**不能是 `127.0.0.1`**）；二维码必须扫屏幕，App 不支持手输 URL。
- 权限：无；局域网模式要放行 Windows 防火墙入站端口。

郊狼主机本身不需要安装任何东西，固件升级走 App。

---

## 6. 风险与未知

1. **授权**：协议文档对爱好者免费，但"商用需事先授权"（`README.md:15`）。开源依赖许可也要注意——`dglab-kit` / `dglab-kit-python` / `dglab-websocket-server` 是 **GPL-3.0**（对要分发的 Windows 二进制有传染性）；`PyDGLab-WS` 是 BSD-3。
2. **BLE 单链路**：假定设备只接受一个中央设备，用户必须先断开 App。**第二个连接是被静默拒绝还是踢掉第一个，未确认。**
3. **V2 通道标注矛盾**（0x1505/0x1506 vs 其位域标签）—— 未解决，上硬件前别信任何一边。
4. **降级（failsafe）未文档化**：V2 明说强度 S 写入后"持续生效"，V3 软上限断电保存；但**断链时设备对通道强度是保持还是归零，未确认**。按"保持"设计：看门狗归零 + BF 软上限 + 告诉用户手边留着硬件开关。
5. **频率换算的取整**（`(v-100)/5+100`）在伪代码里是整数除法，floor 还是 round **未确认**。
6. **时序现实**：官方例子都假设完美的 100 ms 栅格，文档自己承认脉冲时长超过输出窗口时输出可能两个值都不像。别追求 sub-100 ms 的响应速度。
7. **V4 栈还在动**：V3 socket 规范仓库已归档，V4 协议藏在 `dglab-kit` 里。锁版本。
8. **Windows BLE 不稳**（适配器相关的扫描/连接失败、GATT 服务缓存、某些设备必须断电重启才广播）是最可能的日常支持负担；Game Hub 的重连逻辑（5 s 重试、GATT 重连后等 1 s、120 s 轮询电量）是不错的模板。
9. **电气安全**：电极绝不能跨过心脏/胸部或越过身体中线；社区安全建议是 500 Ω 下 r.m.s. 电流远低于 50 mA、单次 ~30 min、变换频率、插入休息期，并且"被控设置"里的舒适/强度上限必须在开启远程控制前配好。**无论游戏请求什么，自己再设一道硬上限。**

### 6.1 硬件实测确认（2026-10-17，真机 + 电极）

**通道输出指示灯是电流指示灯，不是状态灯。** 未接电极时，即使我们持续发送强度 15/10、脉冲强度 100/100 的 B0 帧且设备 B1 回执确认已接受，**指示灯依然不亮**；接上电极形成回路后同一个指示通道立即正常工作（A 通道实测有效）。

这否定了此前提出的"无负载保护"假说。核对依据：

| 假说 | 结论 | 依据 |
|---|---|---|
| 未接电极触发无负载保护，设备拒绝输出 | 不成立 | 官方 V3 协议文档（`coyote/v3/README.md`、`coyote/README.md`）中无 LED、负载、开路、保护相关条目；德国 e-stim 论坛多位 3.0 持有者回答电极脱离时 "hat die Kontaktlosigkeit keinen Einfluss auf den Stromfluss"、"Es schaltet nicht ab."。电极脱落数秒后关闭通道是旧型号行为 |
| 指示灯不亮说明输出链路有故障 | 不成立 | 同一二进制、同一 INI 下，接上电极后通道即工作 |

指示灯反映的是流经负载的电流，回路未闭合时读数为零。判断软件链路状态应看 B1 回执（`B1 <seq> <A实际> <B实际>`）与 `[hb]` 心跳里的 `hooks=OK`，两者在本轮测试中均正常。

厂商页转载的硬件说明中唯一相关条目是 "Intelligent Self-Check At Startup"（开机自检，每通道脉冲模块带冗余备份电路），指自检与故障冗余，与负载检测无关。

---

## 7. 现有实现参考

| 项目 | 语言 | 链接 | 备注 |
|---|---|---|---|
| dglab-kit | TS | [dungeonlab-open/dglab-kit](https://github.com/dungeonlab-open/dglab-kit) | **官方**，V4 socket + 设备操作 |
| dglab-kit-python | Python 3.13+ | [dungeonlab-open/dglab-kit-python](https://github.com/dungeonlab-open/dglab-kit-python) | **官方**，`pip install dglab-kit-python`，V3 同步 + V4 异步 |
| dglab-websocket-server | TS/Bun | [dungeonlab-open/dglab-websocket-server](https://github.com/dungeonlab-open/dglab-websocket-server) | **官方**中继（V4 :9998 / V3 :9999） |
| dglab-websocket-simple | JS | [dungeonlab-open/dglab-websocket-simple](https://github.com/dungeonlab-open/dglab-websocket-simple) | V3 socket 规范 + 示例波形（**已归档**） |
| DG-Lab-Coyote-Game-Hub | TS/Vue + Go | [hyperzlib/DG-Lab-Coyote-Game-Hub](https://github.com/hyperzlib/DG-Lab-Coyote-Game-Hub) | **最佳参考**：socket API 与 Web Bluetooth 直连都有，V2+V3，插件 API，Web UI |
| PyDGLab-WS | Python | [Ljzd-PRO/PyDGLab-WS](https://github.com/Ljzd-PRO/PyDGLab-WS) | BSD-3；`DGLabWSServer("0.0.0.0", 5678, 60)` + `get_qrcode()` |
| dglab-v3-python | Python | [phillychi3/dglab-v3-python](https://github.com/phillychi3/dglab-v3-python) | 轻量 V3 socket SDK |
| dungeonctl | Rust | [mebutkinky/dungeonctl](https://github.com/mebutkinky/dungeonctl) | **直连 BLE**（`btleplug`），仅支持郊狼 3 |
