# DGLab_IDAC

把《Initial D THE ARCADE》(dump/模拟版) 的方向盘**力反馈**实时转换成
**郊狼 (DG-LAB Coyote)** 主机的输出波形。

游戏由 Unreal Engine 4.24 驱动, 本模块以 **UE4SS C++ mod** 的形式接入,
和 `.../Win64/ue4ss/Mods/` 下已有的 mod 一起由 UE4SS 统一加载管理。

---

## 1. 数据链与挂载点结论

```
游戏逻辑算出扭矩/黏滞/振动意图
  └─ IndRun.dll  (UE4 插件, 跑在游戏进程内)
       └─ 5 个 invoker 跳板 (std::function 槽位的调用点)
            └─ 下游: EXE 内的 UE4 /Script/SerialPort 模块
                     └─ CreateFileW/WriteFile/GetCommState → UART → 机台 I/O 板
                        └─ 方向盘电机
```

**挂载点选在 `IndRun.dll` 的 invoker 跳板**, 理由:

| | 挂 `IndRun.dll` (选定) | 挂 `AMDaemon\idachook.dll` |
|---|---|---|
| 语义 | 力反馈的**语义源头**: `torque(direction, magnitude)` / `viscosity(a,b)` / `vibration(a,b)` / `servo_on` / `servo_off` | 设备编码**之后**的字节流 |
| 进程 | 与 UE4SS **同进程** → 天然满足"接入 UE4SS 统一 hook 管理" | AMDaemon 是独立进程, 得另做注入 |
| 与本游戏的关系 | 直接相关 | 游戏进程里**没有** idachook.dll; EXE 里 `idac`/`IDAC`/`idachook` 字符串出现 **0** 次 |

`idachook.dll` 只存在于 `D:\INDAC\MODAC321\AMDaemon\`, 是 SegaTools 的魔改打包版
(把 `common/board/ffb.c` 的串口 FFB 板钩子 + `games/idachook` 反盗版补丁 +
`games/idacio` 方向盘输入 + aimeio 全塞进一个 DLL)。它确实导出 5 个干净的
`idac_io_ffb_*` 函数, **保留为备选/交叉验证的第二个观测点**, 但不在本模块的主路径上。

### 挂钩的 5 个跳板 (IndRun.dll RVA, ImageBase `0x180000000`)

| 通道 | RVA | 签名 | 对应槽位 |
|---|---|---|---|
| servo-on | `0x2160` | `void(void)` | `g+0x08` |
| servo-off | `0x21A0` | `void(void)` | `g+0x48` |
| **torque** | `0x21C0` | `void(int direction, int magnitude)` | `g+0x108` |
| viscosity | `0x2200` | `void(int, int)` | `g+0x148` |
| vibration | `0x2240` | `void(int, int)` | `g+0x188` |

- 单例指针变量在 IndRun.dll RVA `0x241890` (**单次解引用**), 由 RVA `0x5E0C3` 写一次。
- `DllCharacteristics 0x0060` = `DYNAMIC_BASE|HIGH_ENTROPY_VA` → **必须运行期重定位**,
  绝不能硬编码绝对地址。
- 5 个目标全镜像扫描**零取地址引用**, 函数体后是 `int3` 填充 → 前 5 字节补丁安全。
- ⚠ `0x2160` (servo-on) 的门控用的是 **servo-off 槽** (`[rax+0x80]`), 然后才调
  servo-on 槽 (`[rax+0x40]`); 语义是 `if (g && g->slot48) g->slot08();`。
- ⚠ `direction` **不是 2 态**: 域为 `{-1} ∪ {0} ∪ [0,255]`。`-1` 是"算出的扭矩为正"
  的哨兵 (全二进制唯一的字面量), `0` = 中性/停止, `r13d` = 当前档位索引。
  所以按**有符号 int** 处理。
- ⚠ centering (`g+0x88` / `g+0xC8`) 是**死代码** —— 整个映像里没有 invoker,
  EXE 里 `?BindCentering` 出现 0 次。发售版游戏从不绑定回正力。

---

## 2. 仓库结构

```
CMakeLists.txt                构建定义 (只链游戏里那份 UE4SS.dll, 不重编 UE4SS)
src/
  dllmain.cpp                 UE4SS mod 入口: start_mod / uninstall_mod
  ffb_types.hpp               力反馈状态契约 (FfbState / IFfbSource)
  config.hpp/.cpp             INI 配置
  pipeline.hpp/.cpp           力反馈 → 波形映射 + 安全限流
  logger.hpp/.cpp             独立日志文件 + OutputDebugString
  source/
    indrun_hook_source.hpp/.cpp   ★ IndRun.dll 五跳板 MinHook 挂钩
  transport/
    transport.hpp/.cpp        传输接口
    transport_factory.cpp     按配置选传输
    null_transport.cpp        空实现 (干跑/探针用)
    ble_winrt.cpp             ★ BLE 直连郊狼 (WinRT, 无第三方依赖)
    ble_winrt_stub.cpp        占位; 与 ble_winrt.cpp 二选一 (同符号)
tools/
  bootstrap.cmd               ★ 一键重建全部依赖
  build.cmd                   构建 (+ -Deploy 直接铺进游戏)
  make_ue4ss_implib.cmd       从 UE4SS.dll 生成导入库
  install_msvc.cmd            VS Build Tools 装到自定义盘
docs/
  research-overview.md        总览
  research-indrun-ffb.md      IndRun 力反馈数据链
  research-coyote-protocol.md 郊狼协议 (含 Socket API, 网络来源)
  research-dglab-transport.md 郊狼 BLE 传输 (来自本地协议仓库, 权威)
re/                           逆向脚本与报告 (非构建产物)
```

---

## 3. 构建

### 依赖
| | |
|---|---|
| 编译器 | MSVC v143 x64 (**必须 Release**, 必须 **`/MD`**) |
| CMake | ≥ 3.18 |
| VS 组件 | `Microsoft.VisualStudio.Component.VC.Tools.x86.x64` + Windows SDK |
| 第三方 | MinHook (子模块)、RE-UE4SS **v3.0.1 头文件**、UE4SS 导入库 |

> **为什么必须 `/MD` + Release**: UE4SS 官方文档原话 —— C++ mod 与 UE4SS 必须使用
> **同一个 C 运行时版本和构建配置**, 否则 `std::string`/`std::vector` 跨 DLL 边界就崩。
> 已实测: 游戏里那份 `UE4SS.dll` 导入 `msvcp140.dll` / `vcruntime140.dll` → 动态 CRT。
> 能正常工作的参照 mod `Mods\LocalRadio\dlls\main.dll` 同样是 `/MD`。

### 三步

```cmd
tools\bootstrap.cmd
tools\build.cmd -Deploy
```

`bootstrap.cmd` 做三件事: 拉 RE-UE4SS v3.0.1 的头文件 (稀疏检出, 约 11 MB)、
初始化 MinHook 子模块、从游戏自带的 `UE4SS.dll` 生成导入库。

**不 `add_subdirectory(RE-UE4SS)` 的理由**: 那条路会连 UE4SS 本体一起重编 —— 要对
glaze/ImGui/zydis/PolyHook2/raw_pdb 走 `FetchContent`, 其中多个用 `git@github.com:`
的 SSH URL (无 SSH key 直接失败), 还依赖 **corrosion(Rust 工具链)**, 约 20 分钟且脆弱。
改为直接链接游戏里**正在运行的那一份** `UE4SS.dll`: 版本天生一致, 构建几秒完成。

### 手动等价命令
```cmd
call "E:\VS\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
      -DDGLAB_DEPLOY_DIR="<游戏>\...\ue4ss\Mods\DGLab_IDAC"
cmake --build build --config Release
```

---

## 4. 部署

构建产物是 `build\Release\DGLab_IDAC.dll`。
UE4SS **只认 `<mod>\dlls\main.dll`**, 所以部署时改名:

```
<游戏>\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\Mods\DGLab_IDAC\
    dlls\main.dll        <- build\Release\DGLab_IDAC.dll
    enabled.txt          <- 空文件即可
```

`tools\build.cmd -Deploy` 会自动做这件事。
启用方式二选一 (UE4SS v3.0.1 实测行为):

1. 往 `Mods\mods.txt` 加一行 `DGLab_IDAC : 1` —— **推荐, 有加载顺序**
2. 丢一个空的 `enabled.txt` 进 mod 目录 —— 内容从不被读取, 只看文件存不存在

**不需要改 `UE4SS-settings.ini`。**

> ⚠ 排查问题时值得改的三个键 (当前都是关的, 所以看不到 GUI 控制台):
> `[Debug] ConsoleEnabled=1`、`GuiConsoleEnabled=1`、`GuiConsoleVisible=1`,
> 以及把 `[Debug] GraphicsAPI` 从 `opengl` 改成 `dx11` (游戏是 DX11, opengl 渲染不出来)。
> **热重载对 C++ mod 无效** —— `main.dll` 在整个进程生命周期内保持映射, 每次迭代都要重启游戏。

---

## 5. 配置

部署后首次运行会在 mod 目录生成 `DGLab_IDAC.ini`。**出厂默认是探针模式**:

```ini
[general] enable=1, verbose=1
[link]    mode=disabled      ; 不连郊狼, 只记录
[channel_a] source=torque       map_mode=abs_linear  in_max=127  base_strength=40
[channel_b] source=vibration    map_mode=abs_linear  in_max=127  base_strength=30
[safety]  max_strength=40  clamp_strength=40  slew_per_second=40
          watchdog_ms=500  zero_when_servo_off=1  warmup_ms=1500
          dry_run=1        ; 就算 mode=ble 也只算不发
```

日志 `DGLab_IDAC.log` 写在 DLL 自身目录, 同时经 `OutputDebugStringA` 进 `UE4SS.log`
(UE4SS 会捕获, 前缀 `[DGLab_IDAC]`)。

---

## 6. 当前状态

| 阶段 | 状态 |
|---|---|
| 逆向: 定位挂钩点与 ABI | ✅ 完成 (`re/indrun_hook_mechanics.md`, 745 行) |
| 逆向: 郊狼协议 | ✅ 完成 (`docs/research-*.md`) |
| 构建: 编译 + 链接 + 部署 | ✅ 完成, 产物 161,792 字节 |
| **阶段 1: 探针** | ⏳ **待跑** —— 需确认 `mode=xinput` 下游戏到底有没有发力反馈、值域、更新率 |
| 阶段 2: 郊狼真输出 | 未验证 (BLE 代码已写完但从未连过真设备) |

### 阶段 1 要回答的问题

当前 `segatools.ini` 里 `[io4] mode=xinput` —— 方向盘输入走 XInput 手柄。存在两种可能:

- **A**: 游戏仍按街机逻辑生成力反馈 (回正力/路感/打滑), 经 FFB 板往下发 → 跳板上有数据
- **B**: 游戏检测到"不是方向盘"就不走力反馈分支 → 5 个跳板永远空转, 必须改从引擎侧取数

跑一次游戏记日志即可判定。日志里的 `n_torque` / `n_viscosity` / `n_vibration` 调用计数
为 0 就是 B。

---

## 7. 安全

⚠ **无论游戏请求什么, 自己再设一道硬上限。** 出厂 `max_strength=40` (郊狼上限 200)。

- 电极**绝不能跨过心脏/胸部或越过身体中线**
- 建议 500 Ω 下 r.m.s. 电流远低于 50 mA、单次 ~30 min、变换频率、插入休息期
- 郊狼的 **BF 软上限**是最后一道安全阀 (滚轮和 B0 都无法突破), 但它**断电保存**,
  且每次重连后都必须重发
- 断链时设备对通道强度是保持还是归零**未文档化** —— 按"保持"设计:
  看门狗归零 + BF 软上限 + 手边留硬件开关

---

## 8. 协议要点 (郊狼 V3)

- 服务 `0000180C-…`, 写 `0000150A-…` (≤20 B), 通知 `0000150B-…`, 电量 `0000180A→00001500`
- `B0` 指令 20 字节, **每 100 ms 一帧**: seq(4bit) + A/B 强度解读方式(各 2bit) +
  A/B 强度(1 B each) + A 频率×4 + A 波形强度×4 + B 频率×4 + B 波形强度×4
- 每通道 4 组, **每组 25 ms**; 通道强度 0–200 (越界按 0); 波形频率 10–240; 波形强度 0–100
- **某通道 8 个值里只要有 1 个越界, 该通道 4 组全部丢弃**
- 只输出单通道时给另一通道塞一个非法值 (例如波形强度 101)
- 强度变化时序列号非 0, 且**要等 B1 回来**才能发下一次强度修改
- 文档:**只有 BLE**。本仓库里**没有** WebSocket 传输; Socket API 那条路来自网络资料

---

## 9. 许可与署名

郊狼协议文档对爱好者免费, 但**商用需事先授权**。
本项目的第三方依赖: MinHook (MIT)、RE-UE4SS (MIT)、郊狼协议文档 (见原仓库说明)。
