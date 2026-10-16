# 初期调研：IDAC 方向盘力反馈 → 郊狼（DG-LAB）波形输出

日期：调研阶段 · 状态：**已完成可行性论证，待用户确认方案与动作**

---

## 0. 一句话结论

**可行。** 力反馈的黄金挂载点在**游戏进程内**的 `IndRun.dll`（一个 UE4 插件模块）里，是 7 个 `std::function` 回调槽位；而它正好在 **UE4SS 的作用域**内（UE4SS 已在这个游戏上跑起来了），所以**不需要注入 `idachook.dll` 所在的 AMDaemon 进程**。

---

## 1. 现场勘查（已实测，非推测）

### 1.1 游戏进程

| 项 | 值 |
|---|---|
| 可执行文件 | `MODAC321/WindowsNoEditor/GameProject/Binaries/Win64/GameProject-Win64-Shipping.exe` |
| 大小 | 69,627,904 B |
| 引擎 | **UE 4.24**（由 UE4SS 扫描日志确认） |
| UE4SS 日志 | `.../Binaries/Win64/ue4ss/UE4SS.log`，最后运行 2026-10-13 21:15 |

### 1.2 UE4SS 已部署且可用 —— 已有 C++ mod 先例

```
UE4SS - v3.0.1 Beta #0 - Git SHA #0196ef29
UE4SS Build Configuration: Game__Shipping__Win64 (MSVC)
EngineVersion: 4.24
GUObjectArray: 0x7ff7868eb7a0
```

注入方式：**`dwmapi.dll` 代理**（61,952 B，位于游戏 exe 同目录）。其字符串含 `UE4SS.dll`、`ue4ss`、`LoadLibraryW`，PDB 路径为
`D:\a\RE-UE4SS\RE-UE4SS\build_cmake_msvc_Game__Shipping__Win64\Game__Shipping__Win64\bin\dwmapi.pdb`
—— 即 **RE-UE4SS 官方 CMake 工具链构建的官方 proxy**，非魔改。

已装载的 mod（`Mods/mods.json` / `mods.txt`）：

```
CheatManagerEnablerMod : 1        SplitScreenMod : 0
ConsoleCommandsMod     : 1        LineTraceMod   : 0
ConsoleEnablerMod      : 1        BPML_GenericFunctions : 1
BPModLoaderMod         : 1        Keybinds : 1
LocalRadio             : 有 enabled.txt → 启用
```

**关键先例：`Mods/LocalRadio/dlls/main.dll`（1,915,392 B）+ `main.pdb`。**
这正是 UE4SS 的 **C++ mod 标准布局**（`Mods/<ModName>/dlls/main.dll`），说明**该环境已经成功加载过第三方 C++ mod**，我们要做的东西有现成的落地模板。

`Mods/shared/UEHelpers/UEHelpers.lua` 也在（Lua 侧共享库）。

### 1.3 UE4SS 设置要点

`ue4ss/UE4SS-settings.ini`（非注释项）：

```ini
EnableHotReloadSystem = 0
UseCache = 1
InvalidateCacheIfDLLDiffers = 1
SecondsToScanBeforeGivingUp = 30
bUseUObjectArrayCache = false
DoEarlyScan = 0
DefaultExecuteInGameThreadMethod = EngineTick
DefaultFNameToStringMethod = Scan
[Debug] ConsoleEnabled = 0, GuiConsoleEnabled = 0, GuiConsoleVisible = 0
```

→ 热重载关闭。调试期建议开 `GuiConsoleEnabled`/`GuiConsoleVisible` 看输出。
→ `EnableHotReloadSystem = 0` 意味着改完 DLL 必须重启游戏进程。

### 1.4 引擎目录

`WindowsNoEditor/Engine/` 只有 `Binaries/ThirdParty/*` 和 `Extras/Redist/`，**没有 UE4 编辑器**，纯分发版。第三方库（PhysX3、Vorbis、Ogg、libsndfile、NVaftermath、dbghelp）都是 VS2015 时代的 Win64 库。

### 1.5 `idachook.dll` 的位置

```
find MODAC321 -iname "idachook*"   →   只有 ./AMDaemon/idachook.dll
```

**游戏目录里没有 `idachook.dll`**。它是给 **`AMDaemon.exe` 进程**用的,不在游戏进程里。

---

## 2. 数据链：谁产生力反馈，谁发送力反馈

这是本次调研最重要的发现 —— **它推翻了"在 idachook 里挂 FFB"这个初始假设**。

```
┌─────────────────────────────── 游戏进程 GameProject-Win64-Shipping.exe ───────────────────────────────┐
│                                                                                                        │
│   IndRun.dll  (UE4 插件, 运行时由 LoadLibraryW 动态加载)                                                │
│      │                                                                                                 │
│      │ ① 游戏每个 tick 算出力反馈语义量                                                                  │
│      │ ② 调用 7 个 std::function 槽位  ← ★★★ 黄金挂载点 ★★★                                          │
│      ▼                                                                                                 │
│   全局单例 *(0x180241890)，槽位 g+0x08/0x48/0x88/0xC8/0x108/0x148/0x188，步长 0x40                     │
│      │                                                                                                 │
│      ▼  ③ EXE 侧注册的回调（GameProject-Win64-Shipping.exe）                                            │
│   ┌──────────────────────────────────────────────────────────────┐                                     │
│   │ /Script/SerialPort  (UE4 模块, ESerialPortError::*)          │                                     │
│   │ 导入 CreateFileW / WriteFile / ReadFile / GetCommState /    │                                     │
│   │     SetCommState / MapViewOfFile  → 串口 / 共享内存           │                                     │
│   │ /Script/AMDaemon    (AMDaemonAccessCode / AMDaemonAimeId /   │                                     │
│   │     EAMDaemonAimeCommand …)  → 与 AMDaemon 进程通信的插件      │                                     │
│   └──────────────────────────────────────────────────────────────┘                                     │
└────────────────────────────────────────────────────────────────────────────────────────────────────────┘
                                        │ 进程间（串口 / 共享内存 / IPC）
                                        ▼
                         AMDaemon.exe  →  idachook.dll  →  真实串口/机台 I/O
```

**`IndRun.dll` 只算力，不发送。** 硬证据：

- 它的 kernel32 导入只有进程/线程/TLS/内存/计时 —— **没有 `CreateFileW`、`WriteFile`、`DeviceIoControl`、socket**。
- 导入 winmm 的 `timeGetTime`/`timeBeginPeriod`/`timeEndPerio`，但 **没有 `CreateThread`、`Sleep`、`timeSetEvent`**（所以它连自己的定时器都没有，节奏由游戏 tick 驱动）。
- 唯一对外出口是那 7 个 `std::function`。

**EXE 里 `idac`/`IDAC`/`idachook` 字符串计数 = 0。** 所以游戏和 `idachook.dll` 之间**没有任何符号级链接**。

---

## 3. `IndRun.dll` 的力反馈 API（已反出）

路径：`MODAC321/WindowsNoEditor/GameProject/Plugins/IndRun/Binaries/Win64/IndRun.dll`（2,634,240 B，123 个 C++ mangled 导出）。

游戏通过 **mangled 名字符串 + `GetProcAddress`** 绑定（EXE 的 `.rdata` 在 VA `0x143924BF1` 存有名字表）：

```cpp
// enum nsFeedbackDefine::eTrqueDirection : int   （W4 = 4 字节底层类型）
void BindServoOn          (std::function<void()>);
void BindServoOff         (std::function<void()>);
void BindTorqueFunction   (std::function<void(nsFeedbackDefine::eTrqueDirection, int)>);
void BindViscosityFunction(std::function<void(int, int)>);
void BindVibrationFunction(std::function<void(int, int)>);
void UnBindAllFB          ();
```

**已确证的枚举值：`-1 (0xFFFFFFFF)`**。`IndRun.dll` RVA `0x6D19` 处 `mov dword ptr [rsp+0x28], 0xffffffff` 后以 `rdx=&该值`、`r8=&(int)0` 调用扭矩槽 → `torque(-1, 0)` = 归零。RVA `0x2BCD6` 处扭矩 > 0 时也把 `ecx` 设为 `0xffffffff`。

> ⚠️ **静态不可恢复**：枚举成员名。`IndRun.dll` 里既无该枚举的字符串也无 RTTI，只有 mangled 类型名。所以**只能拿到数值，拿不到 `Left`/`Right` 这类名字**。需运行时观测确定 `-1/0/+1` 的物理含义。

**决定性否定证据**：`?BindCentering` 在 EXE 里出现 **0 次** → 发售版游戏**从不绑定回正力**，那两个槽位永远是 null。回正效果应该是被折算进扭矩通道了。

### 推荐挂钩目标（RVA in `IndRun.dll`）

| RVA | 签名 | 说明 |
|---|---|---|
| `0x21C0` | `(int direction, int magnitude)` | 扭矩，槽 `g+0x108`，调用点 `0x2BCDB` |
| `0x2160` | `()` | servo-on，槽 `g+0x08` |
| `0x21A0` | `()` | servo-off，槽 `g+0x48` |
| `0x2200` | `(int, int)` | viscosity，槽 `g+0x148` |
| `0x2240` | `(int, int)` | vibration，槽 `g+0x188` |
| `0x2020` | `()` | `UnBindAllFB` |

**不要**去挂那个 100 KB 巨函数：FFB 更新被内联在 RVA `0x148A0`–`0x2D274` 的单个函数里，无法作为整体挂钩。

跳板内部：读取槽的 `_Ptr`，以 `(rdx=&direction, r8=&magnitude)` 调用 vtable`[0x10]`。
MSVC `std::function` = 0x40 字节，`_Ptr` 在 `+0x38`。

---

## 4. 方案对比

### 方案 A（推荐）：UE4SS C++ mod，挂 `IndRun.dll` 跳板

- ✅ 满足"接入 UE4SS 统一 hook 管理"的要求
- ✅ 有现成先例（`LocalRadio/dlls/main.dll`），落地路径已验证
- ✅ 在**信号源头**取数，语义完整（扭矩方向/幅值、粘度、振动、servo 状态全都有）
- ✅ DXGI/Win32 hook 都不需要；只做内存 hook
- ✅ 与 `idachook.dll` 完全解耦 —— 升级/换 dump 版不受影响
- ✅ 不受 `idachook.dll` 是否开启自定义 IO 影响
- ⚠️ 需要 MSVC（**当前机器没有，见 §6**）
- ⚠️ 需要 UE4SS 的 hook API 细节（子代理正在查，见 §8）

### 方案 B：在 `AMDaemon.exe` 进程里挂 `idachook.dll`

- ✅ 拿到的是**实际发给机台的字节流**，最贴近"真机力反馈"
- ❌ **该进程里没有 UE4SS**（UE4SS 只注入游戏 exe），需要独立注入器 → 不满足"统一 hook 管理"
- ❌ 需要自己解析 IDAC 协议帧 → 比方案 A 多做一层解码
- ❌ 依赖 `idachook.dll` 内部实现（本机是魔改版，与公开 segatools 有差异）
- ❌ 有"自定义 IO 没开启就啥也收不到"的风险

### 方案 C：EXE 侧 `Bind*` 调用点（方案 A 的补充）

挂在 EXE 里把 `(direction, magnitude)` 变成设备 I/O 的那一层。信息量和方案 A 重叠，但**是验证方案 A 正确性的最佳对照**。建议作为**交叉校验**而非主路径。

**结论：走方案 A，方案 C 用于校验。**

---

## 5. 后续架构（初步）

```
[游戏进程]                                        [本机其它进程 / 外部]
IndRun.dll 跳板 hook
      │  (direction, magnitude) / (a, b) / servo 状态
      ▼
  FFB 采集层 ── 语义化 ── 通道映射 ── 波形生成（重采样到 25 ms 帧）
      │                                        │
      │                                  强度/频率安全限幅
      ▼                                        ▼
  环形缓冲 / IPC  ────────────────────►  郊狼传输层（BLE / WebSocket 中继）
                                               │
                                        DG-LAB 主机（V3 协议）
```

**关键设计约束（来自郊狼协议）**：DG-LAB V3 的 `B0` 强度命令 ≈ **25 ms 量化**；`BF` 波形帧固定 **100 ms**（每帧 8 bit，每 bit 10 ms）。所以：
- 波形通道要用 **100 ms 帧 / 10 ms 位**的表达力，必须把游戏的连续力反馈**离散化**。
- 强度通道（`B0`）每 25 ms 才变一次 → 不能直接贴原始 FFB 采样率。

---

## 6. 环境清单

### 已具备

| 组件 | 状态 |
|---|---|
| UE4SS v3.0.1 Beta | ✅ 已部署于 `.../Binaries/Win64/ue4ss/` |
| `dwmapi.dll` 代理 | ✅ 官方 RE-UE4SS 构建 |
| 游戏 + dump 补丁 | ✅ 可运行，UE4SS 日志证明成功注入 |
| Python 3.12.6 | ✅（逆向/探针脚本用） |
| CMake 3.29.4 | ✅ |
| Git 2.56.0 | ✅（源码仓 `D:\INDAC\DGLab_IDAC`，远端 `github.com/OTaku9418/DGLab_IDAC.git`） |
| 郊狼协议资料 | ✅ `dglab-bluetooth-protocol/` |

### **缺失（阻塞项）**

| 组件 | 说明 |
|---|---|
| **MSVC 编译器** | ❌ **本机没有 VS2022 / VS2019，`vswhere.exe` 不存在，`cl` 不在 PATH。编译 C++ mod 必须装。** |
| UE4SS 头文件/导入库 | ❌ 需要与 `v3.0.1 Beta #0196ef29` 匹配的 RE-UE4SS 源码（`git clone` + 编译其 `UE4SS` target，或只取 `UE4SS/include` 头 + `.lib`） |
| Visual Studio 2022 生成工具 | 建议装 **VS 2022 Community** 或 **Build Tools 2022**，勾选「使用 C++ 的桌面开发」 |
| Windows SDK | 随 VS 安装 |
| （可选）WinDbg / x64dbg | ❌ 未检测到；用于运行时确认探针与校验地址 |
| （可选）蓝牙适配器 | 取决于郊狼连接方案：WebSocket 中继（本机当服务端，手机 APP 连）还是本机 BLE |

### 关于"郊狼连接方式"

需要确认走哪条路：
- **WebSocket 中继**：我们做一个本机 WS 服务端，郊狼 APP/DGLab 客户端连过来；协议用 `dglab-bluetooth-protocol/` 里的定义。**不需要本机蓝牙**。
- **本机 BLE 直连**：需要本机蓝牙适配器 + Windows BLE 栈（WinRT `Windows.Devices.Bluetooth`）。

---

## 7. 安全

用户明示这是**自己的设备**，用于**单人离线**场景，并明确接受安全风险。仍然保留三层保护（可一键关闭）：强度上限 `pc`、斜率限制（避免阶跃冲击）、失联/异常自动归零。这是工程义务，不是道德评判。

---

## 8. 未决问题

1. `r13d` 那个 `eTrqueDirection` 值的确切数值（可能 `0` 或 `+1`）。
2. torque/kickback/viscosity/vibration 的完整数值常量表（标定映射用）。
3. **游戏实际发出力反馈的更新频率** —— 只有运行时探针能测，决定重采样策略。
4. EXE 侧 `Bind*` 调用点（用于方案 C 校验）。
5. 游戏到底通过 `/Script/SerialPort` 还是 `/Script/AMDaemon` 把 FFB 交给 AMDaemon —— 需运行时探针。
6. 郊狼版本（V2 / V3）与连接方式（WS / BLE）。
