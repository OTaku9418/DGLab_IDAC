# IndRun / FFB 数据链调研（阶段性）

状态：**阶段性结论**。地址均为 `IndRun.dll` 的 RVA，前缀 `EXE` 的表示 `GameProject-Win64-Shipping.exe` 的 VA。

## 1. 结论先行

**游戏侧（`GameProject-Win64-Shipping.exe`）的 FFB 源头在 `IndRun.dll`，而且 `IndRun.dll` 只负责"算力"，不负责"发"** —— 它没有任何出站 I/O 能力。

- `IndRun.dll` 的 kernel32 导入只有进程/线程/TLS/内存/计时，**没有 `CreateFileW`、没有 `WriteFile`、没有 `DeviceIoControl`、没有 socket**。
- 它导入 winmm 的 `timeGetTime`/`timeBeginPeriod`/`timeEndPeriod`，但**没有 `CreateThread`、没有 `Sleep`、没有 `timeSetEvent`**。
- 它唯一的出站 FFB 路径是调用一个全局单例里的 **7 个 `std::function` 槽位**，位于 `*(0x180241890)`。

所以：**挂钩这 7 个槽位的调用跳板（trampoline），就能 100% 拿到游戏算出来的全部力反馈语义。**

## 2. 动态加载与绑定

- EXE **没有**静态导入 `IndRun.dll`（43 个导入 DLL 里没有它，也没有 ILT 条目）。EXE 导入 `GetProcAddress` + `LoadLibraryW`。
- EXE 的 `.rdata` 在 **VA `0x143924BF1`（file `0x39241F1`）** 存有一个长度前缀的名字表，包含以下精确 mangled 名：

```
?UnBindAllFB@cUE4Interface@@QEAAXXZ
?BindServoOn@cUE4Interface@@QEAAXV?$function@$$A6AXXZ@std@@@Z
?BindServoOff@cUE4Interface@@QEAAXV?$function@$$A6AXXZ@std@@@Z
?BindTorqueFunction@cUE4Interface@@QEAAXV?$function@$$A6AXW4eTrqueDirection@nsFeedbackDefine@@H@Z@std@@@Z
?BindViscosityFunction@cUE4Interface@@QEAAXV?$function@$$A6AXHH@Z@std@@@Z
?BindVibrationFunction@cUE4Interface@@QEAAXV?$function@$$A6AXHH@Z@std@@@Z
```

- 模块名字符串：`IndRun.dll` 在 EXE VA `0x1433784A0`；`/Script/IndRun` 在 `0x142A9A960`。
- **决定性否定证据**：`?BindCentering` 在 EXE 里**出现 0 次** → 发售版游戏从不绑定回正力，那两个槽位永远是 null（`IndRun.dll` 里也确实没有对应的 invoke thunk）。

## 3. 回调签名

- `enum nsFeedbackDefine::eTrqueDirection : int`（mangled 里的 `W4` = 底层类型 4 字节，即 `int`）。
- 扭矩回调类型：`void(nsFeedbackDefine::eTrqueDirection, int)`。
- **已验证的枚举值：`-1` (`0xFFFFFFFF`)**。`IndRun.dll` RVA `0x6D19` 处：`mov dword ptr [rsp+0x28], 0xffffffff` 然后以 `rdx=&该值`、`r8=&(int)0` 调用扭矩槽 → `torque(-1, 0)` = 归零扭矩。RVA `0x2BCD6` 处当算出的扭矩 > 0 时也把 `ecx` 设为 `0xffffffff` 作为方向。
- 另一个值（寄存器 `r13d`，在扭矩 ≤ 0 时使用）尚未确定，可能是 `0` 或 `+1`。
- **静态不可恢复**：枚举成员名。`IndRun.dll` 里没有该枚举的字符串也没有 RTTI，只有 mangled 类型名。所以拿不到 `eTrqueDirection::Left` 这类名字，**只能拿到数值**。
- 注意 EXE 里还有另一个 UE4 侧的枚举 `ESteeringFeedbackMode::AutoCentering|Centering|Torque|Viscosity|Vibration`（EXE `0x2C72DF0`），那是游戏自己的模式选择器，**不是** `eTrqueDirection`。

## 4. 加载形态

`IndRun.dll` 是**独立的 UE4 插件模块**（`MODAC321\WindowsNoEditor\GameProject\Plugins\IndRun\Binaries\Win64\IndRun.dll`），运行时按 UE4 插件机制加载，**不是静态链进 Shipping.exe**。

## 5. 发送侧在 EXE 里，而且和 idachook 没有符号级关联

- EXE 内置 UE4 的 `/Script/SerialPort` 模块（带 `ESerialPortError::FrameError|OverrunError|OverflowError|ParityError`），并导入 `CreateFileW`/`WriteFile`/`ReadFile`/`GetCommState`/`SetCommState`/`MapViewOfFile` → 走 UART/串口到机台 I/O 板。
- EXE 里 **`idac`/`IDAC`/`idachook` 字符串为 0**，所以**不存在**指向 idachook 的直接符号链接。
- EXE 里另有 `/Script/AMDaemon` UE4 模块（`AMDaemonAccessCode`、`AMDaemonAimeId`、`EAMDaemonAimeCommand` 等），即**游戏进程内集成了与 `amdaemon.exe` 通信的插件**。
- `MODAC321/WindowsNoEditor/GameProject/Binaries/Win64/dwmapi.dll`（61,952 B，放在游戏 exe 旁边）是 DLL 劫持代理 —— 尚未逆向，但这是**游戏进程内的注入通道**。

→ **待验证的开放问题**：`AMDaemon`（进程间）↔ `idachook`（IO 层）↔ 游戏串口，这三者到底谁把 FFB 命令写出去。这个问题只有在游戏运行时探针才能定死。

## 6. 推荐挂钩目标

挂钩 **`IndRun.dll` 里的 invoke 跳板**，不要去碰那个 100 KB 的巨型函数（FFB 更新被内联在 RVA `0x148A0`–`0x2D274` 的单个巨函数里，无法作为整体挂钩）：

| 跳板 RVA | 对应槽位 | 签名 |
|---|---|---|
| `0x21C0` | 扭矩，槽 `g+0x108` | `(int direction, int magnitude)` |
| `0x2160` | servo-on，槽 `g+0x08` | `()` |
| `0x21A0` | servo-off，槽 `g+0x48` | `()` |
| `0x2200` | viscosity，槽 `g+0x148` | `(int, int)` |
| `0x2240` | vibration，槽 `g+0x188` | `(int, int)` |
| `0x2020` | `UnBindAllFB` | 遍历全部 7 个槽 |

扭矩的调用点：RVA `0x2BCDB` 的 `call 0x21C0`；跳板 `0x21C0` 读取 `_Ptr`（槽 `g+0x108`），用 `(rdx=&direction, r8=&magnitude)` 调用 vtable`[0x10]`。

单例结构：`g = *(0x180241890)`，在 `IndRun.dll` RVA `0x5E0B7`–`0x5E0C3` 惰性分配。构造函数把 7 个 `std::function` 的 `_Ptr` 字段清零，位置在 `g+0x40/0x80/0xC0/0x100/0x140/0x180/0x1C0`；槽位在 `g+0x08/0x48/0x88/0xC8/0x108/0x148/0x188`，**步长 `0x40`**（MSVC `std::function` = 0x40 字节，`_Ptr` 在 `+0x38`）。

## 7. 仍未完成

- `r13d` 那个枚举的确切数值。
- torque/kickback/viscosity/vibration 的完整数值常量表（用于标定映射）。
- **更新频率的实证**（决定 25 ms 量化器的重采样策略）—— 必须有运行时数据。
- EXE 侧 `Bind*` 的调用点（正在定位）；那是 `(direction, magnitude)` 变成设备 I/O 的地方。
- `dwmapi.dll` 代理的实现。
