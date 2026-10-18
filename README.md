# 安全与免责声明

项目大量使用DeepSeek V4.1 Flash辅助开发，可能存在未查出的bug，不保证完全稳定运行，使用前请在配置文件中配置好软件下发的主机最大强度。

本项目的用途是把自己游戏设备的力反馈信号转发到郊狼主机上，仅供学习交流用途使用，使用前请自行衡量风险并恰当配置强度。

仓库所有人不对任何使用本仓库带来的后果负责。 

**⚠Use at your own risk⚠**

**输出会真实作用于人体**,
且 `[link] mode=ble` + `[safety] dry_run=0` 时没有任何软件层面的"不会电到人"保证。
第一次接电极前请先读第 7 节, 并考虑先用 `tools\bleprobe.exe <ini> 20` (不加 `--live`,
会强制把两个通道强度压到 0) 确认链路, 再上人。

# DGLab_IDAC

把《Initial D THE ARCADE》的方向盘**力反馈**实时转换成
**郊狼3.0 (DG-LAB Coyote v3)** 主机的输出波形。

游戏由 Unreal Engine 4.24 驱动, 本模块以 **UE4SS C++ mod** 的形式接入,
基于ArcadeZone.cn服务器所提供的3.21游戏版本（启动器V2.11）+自定义BGM Mod（引用自定义BGM Mod自带的UE4SS版本）
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

`idachook.dll` 只存在于 `\AMDaemon\`, 是 SegaTools 的魔改打包版
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
  bisect_logger.hpp/.cpp      崩溃夹逼用的极简日志 (VEH 之前就落盘, 不依赖 CRT)
  diag_mod.cpp                诊断构建的入口 (-DDGLAB_DIAGNOSTIC=ON, 与本体互斥)
  abitest.cpp                 最小 ABI 探针 mod (判定崩溃是代码问题还是头文件 ABI 不匹配)
  source/
    indrun_hook_source.hpp/.cpp   ★ IndRun.dll 五跳板 MinHook 挂钩
  transport/
    transport.hpp/.cpp        传输接口 + B0/BF 协议编码
    transport_factory.cpp     按配置选传输
    null_transport.cpp        空实现 (干跑/探针用)
    ble_winrt.cpp             ★ BLE 直连郊狼 (WinRT, 无第三方依赖)
    ble_winrt_stub.cpp        占位; 与 ble_winrt.cpp 二选一 (同符号)
tools/
  bootstrap.cmd               ★ 补齐依赖 (头文件 + fmt + 导入库, 都在仓库里, 只查在不在)
  build.cmd                   构建 (+ -Deploy 直接铺进游戏, + -Stage N 写 bisect.txt)
  make_ue4ss_implib.cmd       换 UE4SS 版本时重新 dump 导出表 -> UE4SS.def
  install_msvc.cmd            VS Build Tools 装到自定义盘
  mod_on.cmd / mod_off.cmd    把 <mod>\dlls 改名成 dlls.off, 用于 A/B 排查
  protocol_check.cpp          ★ 离线协议自检, 54 项, 基准是官方文档而非本项目实现
  bleprobe.cpp                ★ 脱离游戏单独跑 BLE 链路 ([--live] 才真输出)
  conftest.cpp                ★ 离线校验 INI: "会连真实设备吗 / 设备会有刺激吗"
  DGLab_IDAC.ini.template     带注释的配置模板 (部署脚本铺的就是它)
docs/
  research-overview.md        总览
  research-indrun-ffb.md      IndRun 力反馈数据链
  research-coyote-protocol.md 郊狼协议 (含 Socket API, 网络来源)
  research-dglab-transport.md 郊狼 BLE 传输 (来自本地协议仓库, 权威)
re/                           逆向脚本与报告 (非构建产物)
  indrun_hook_mechanics.md    ★ 挂钩点的完整逆向结论
  indrun_ffb_report.md        ★ 参数语义 (torque/viscosity/vibration 各是什么)
  verify_indrun_rva.py        离线校验 IndRun.dll 的 TimeDateStamp + 5 处特征码
  disasm_indrun.py            capstone 反汇编 5 个 invoker
```

> `_ue4ss_ref/` 下三样东西是**提交进仓库**的第三方依赖, 不是构建产物:
> `ue4ss_0196ef29/` 与 `ue4ss_0196ef29_deps/` 的 ABI 对齐头文件 (约 330 KB)、
> `third_party/fmt/` (fmt 11.2.0, 约 590 KB)。加上 `third_party/importlib/UE4SS.def`,
> 一共约 1.3 MB, 换来"没有游戏安装也能构建"。见第 3 节。

---

## 3. 构建

### 依赖
| | |
|---|---|
| 编译器 | MSVC v143 x64 (**必须 Release**, 必须 **`/MD`**) |
| CMake | ≥ 3.18 |
| VS 组件 | `Microsoft.VisualStudio.Component.VC.Tools.x86.x64` + Windows SDK |
| 第三方 | MinHook (子模块); RE-UE4SS **0196ef29** 头文件 + fmt 11.2.0 + UE4SS.def **都在仓库里** |

> **为什么必须 `/MD` + Release**: UE4SS 官方文档原话 —— C++ mod 与 UE4SS 必须使用
> **同一个 C 运行时版本和构建配置**, 否则 `std::string`/`std::vector` 跨 DLL 边界就崩。
> 已实测: 游戏里那份 `UE4SS.dll` 导入 `msvcp140.dll` / `vcruntime140.dll` → 动态 CRT。
> 能正常工作的参照 mod `Mods\LocalRadio\dlls\main.dll` 同样是 `/MD`。

### 三步

```cmd
tools\bootstrap.cmd
tools\build.cmd -Deploy
```

`bootstrap.cmd` 做三件事: 确认 ABI 对齐的 UE4SS 头文件与 fmt 11.2.0 在位 (两者都在仓库里,
正常跳过)、初始化 MinHook 子模块、用仓库里的 `UE4SS.def` 生成导入库。

**不需要装游戏, 也不需要 `UE4SS.dll`。** 这三样第三方依赖全都在仓库里:

| 依赖 | 位置 | 为什么不能现拉 |
|---|---|---|
| ABI 对齐头文件 | `_ue4ss_ref\ue4ss_0196ef29*` | 必须是 0196ef29 那个 commit, 不能是 tag v3.0.1 (见下) |
| fmt 11.2.0 | `_ue4ss_ref\third_party\fmt` | 版本必须与 UE4SS 静态链的那份一致, 且带 UE4SS 的 check 宏补丁 |
| 导入库来源 | `third_party\importlib\UE4SS.def` | 4067 行导出符号; `lib /def` 几秒生成 `.lib` |

`UE4SS.dll` 只在**换 UE4SS 版本、要重新 dump 导出表**时才需要 (那时跑
`tools\make_ue4ss_implib.cmd`)。构建时它可有可无 —— 找不到就少一条版本核对信息。

**在别的机器上**: 只有 `-Deploy` (往游戏目录铺) 需要知道游戏在哪。

```cmd
set DLAB_DEPLOY_DIR=D:\你的游戏\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\Mods\DGLab_IDAC
tools\build.cmd -Deploy
```

或者 `cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DDGLAB_DEPLOY_DIR="..."`。
`UE4SS.dll` 的路径会从 mod 目录往上两级推导, 也可以用 `-DUE4SS_DLL=` 直接指定。
给的线索找不到时 CMake 会打一条 WARNING 然后只靠导入库继续 (不会偷偷回退到别人的
`D:\INDAC` 路径 —— 那种"看起来找到了"的假信息在排查 ABI 问题时最毒)。
不部署 (不加 `-Deploy`) 时产物就留在 `build\Release\DGLab_IDAC.dll`。

**为什么头文件跟着仓库走**: `_ue4ss_ref\ue4ss_0196ef29\` 不是上游 tag `v3.0.1` 的头文件,
而是 commit `0196ef294f8525d6a492ae0b41b0c18ad5ccd84b` (2026-03-27, `experimental-latest-695-g0196ef2`)
那一版的。两者之间 `CppUserModBase` 的虚表布局**不同**:

| 槽位 | v3.0.1 | 0196ef29 |
|---|---|---|
| `on_ui_init` | 无 | 有 |
| `on_cpp_mods_loaded` | 无 | 有 |
| 新旧 `on_lua_start`/`on_lua_stop` 重载 | 各 2 个 | 各 4 个 (新旧并存) |

用 v3.0.1 的头文件编出来的 mod, UE4SS 调用虚函数时会跳到错误的位置 —— 表现为
**游戏启动即崩, 崩在 `UE4SS.dll` 内部**, 而且日志干净得看不出我们参与了。
本仓库历史上就是栽在这上面: 当时先怀疑是虚表错位, 手工写了一版 shim 去"猜"槽位顺序,
结果崩得更早(`start_mod` 都没跑到)。教训是 **ABI 不能猜, 只能逐 commit 对齐头文件**。
所以这两份头文件 (约 330 KB) 直接提交, 换 UE4SS 版本时必须换掉它们并重编。

**不 `add_subdirectory(RE-UE4SS)` 的理由**: 那条路会连 UE4SS 本体一起重编 —— 要对
glaze/ImGui/zydis/PolyHook2/raw_pdb 走 `FetchContent`, 其中多个用 `git@github.com:`
的 SSH URL (无 SSH key 直接失败), 还依赖 **corrosion(Rust 工具链)**, 约 20 分钟且脆弱。
改为直接链接游戏里**正在运行的那一份** `UE4SS.dll`: 版本天生一致, 构建几秒完成。

**为什么提交 `.def` 而不是 `.lib`**: 导入库是派生数据, 而且绑死 MSVC 版本 —— 换一台
机器或换个 VS 版本, 提交的 `.lib` 就可能链接不上。`.def` 是纯文本 (4067 行), 与编译器
无关, `lib /def:UE4SS.def /machine:x64 /out:UE4SS.lib` 现场生成。实测两条路得到的
`.lib` 大小完全相同 (2,155,224 字节)、导出表相同, 只在 MSF 头的时间戳上有差别。

### 手动等价命令
```cmd
call "<VS构建工具路径>\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
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
(UE4SS 会捕获, 前缀 `[DGLab_IDAC]`)。关心的几类行:

| 前缀 | 频率 | 内容 |
|---|---|---|
| `[hb]` | 1 s | 各来源的调用次数与值域、servo 状态、自检摘要 |
| `[probe]` | 变化时 / 前 10 s 每帧 | 原始 FFB 值 (排查映射问题时看这个) |
| `[tx]` | 10 s | 传输层: 取帧间隔、重复/覆盖计数、看门狗触发次数 |
| `[ffbgap]` | 60 s | **诊断用**: 正扭矩期间相邻两次回调的间隔分布 (`<0.5s/0.5-2s/2-5s/>5s`) 与最长间隔 |

`[ffbgap]` 是用来判断"要不要给力反馈加保持超时"的数据: `>5s` 那档如果长期非零,
说明游戏会长时间不再回调而扭矩值仍停在正值, 这种场景 T1 的看门狗覆盖不到。

---

## 6. 当前状态

| 阶段 | 状态 |
|---|---|
| 逆向: 定位挂钩点与 ABI | ✅ 完成 (`re/indrun_hook_mechanics.md`) |
| 逆向: 郊狼协议 | ✅ 完成 (`docs/research-*.md`, 官方文档副本见 `re/reference/dglab-bluetooth-protocol/`) |
| 构建: 编译 + 链接 + 部署 | ✅ 完成 |
| ABI: 与本机 UE4SS `0196ef29` 对齐 | ✅ 完成 (曾因头文件版本不对而启动即崩) |
| 阶段 1: 探针 | ✅ 完成 —— 5 个跳板**都有数据** |
| 阶段 2: 郊狼真输出 | ✅ **A 通道已在真机上端到端验证通过**; B 通道待实测 |

阶段 1 的结论是当年那两种可能里的 **A**: 即使 `segatools.ini` 里 `[io4] mode=xinput`,
游戏照样按街机逻辑生成力反馈并经 FFB 板下发给 IndRun.dll。一次完整计时赛的实测计数:

```
torque=947 次   mag[0..18]   dir[-1..1]
visc=10868 次   b[2..40]     (几乎每 tick 都有值)
vib=136 次      b[0..29]     (只在事件发生时)
servo on/off = 1/1, state=1
```

阶段 2 的真机验证结果 (设备回的 B1 与请求逐字节一致):

```
请求   B0 1F 0F 0A ...   ->  A=15 B=10
设备回 B1 01 0F 0A       ->  A=15 B=10   ✓
```

**看门狗** (`[safety] watchdog_ms`, 默认 500): 管线侧超过这个毫秒数没有提交新帧,
传输层就改发静默帧 (强度 0); 管线自己停顿超过这个值也会清零强度累积并重新预热。
判据是"管线有没有在提交帧", **不是**"力反馈回调有没有来" —— 回调是边沿触发的,
力不变时不回调属于正常现象, 拿它做判据会在游戏正常运行时误静音。配置加载时会被
抬到至少 `3 * frame_ms`。离线用例见 `tools/logic_check.cpp`。

---

## 7. 安全

⚠ **无论游戏请求什么, 自己再设一道硬上限。** 出厂 `max_strength=40` (郊狼上限 200)。

- 电极**绝不能跨过心脏/胸部或越过身体中线**
- 建议 500 Ω 下 r.m.s. 电流远低于 50 mA、单次 ~30 min、变换频率、插入休息期
- 郊狼的 **BF 软上限**是最后一道安全阀 (滚轮和 B0 都无法突破), 但它**断电保存**,
  且每次重连后都必须重发
- 断链时设备对通道强度是保持还是归零**未文档化** —— 按"保持"设计。实际存在的三道兜底:
  1. **看门狗** (`[safety] watchdog_ms`, 默认 500, 下限 `3*frame_ms`): 管线超过这个时间没提交
     新帧, 传输层就不再发上一帧, 改发**静默帧** (强度字节 0、无脉冲); 管线自身停顿超时也会
     清零强度累积并重新预热, 避免恢复瞬间一帧跳到满力。
  2. **BF 软上限** (`max_strength`), 设备侧硬顶, 滚轮和 B0 都突破不了。
  3. **手边留物理断电开关** —— 前面两条都在软件里, 软件卡死时只有拔电可靠。
  `dry_run=1` 是配置层的硬开关, 走空输出, 连 BLE 都不建立。

---

## 8. 已知风险与使用须知

这一节记的是"模组本身消除不了、但用之前必须知道"的行为。都有实测依据, 不是推测。

1. **游戏整进程卡死时, 没有任何软件能归零输出。** 本作的物理与渲染在同一帧推进: 拖动控制台
   滚动条造成的卡顿会让挂钩、管线、BLE 发送线程一起停, 传输层根本没机会发静默帧。实测一段
   421 s 的对局里两次卡顿 (10874 ms 与 13953 ms), `[tx]` 的 `trips` 全程是 0, 设备保持卡死前
   一帧的通道强度 (实测 25/35)。看门狗只覆盖"游戏还在跑、但管线或传输线程某一边卡住"的情况
   (离线用例 `tools\logic_check.exe`, 实机用例 `tools\bleprobe.exe <ini> 20 --stall=5`)。
   **真正的兜底是手边的物理断电开关。**
2. **卡顿恢复后会短暂掉力。** 管线侧检测到停顿会清零强度累积并重新预热, 所以恢复后的头几帧
   很弱, 约 1 s 后才回到原值。这个行为是刻意的 (避免恢复瞬间一帧跳到满力), 不是缺陷。
3. **退出比赛/回菜单之后的输出由游戏决定, 不由模组决定。** 实测两条退出路径 (自然 retire、
   强制退出比赛) 游戏都会把力反馈值压到接近 0: 强制退出那次 A 的脉冲振幅从 ~45 掉到 4、
   B 从 ~45 掉到 9 (满 100), 但**通道强度字节没有归零**, 仍停在 25/35。也就是说"菜单里没感觉"
   是游戏给的, 不是本程序保证的。若退出恰好落在一次大阻尼采样之后 (实测 78.4–79.1 s 出现过
   `b=40`), 菜单里就会以强度 35、脉冲振幅 60/100 一直输出。**别用"回菜单"代替断电开关。**
4. **`viscosity` 作为输出源带上述风险。** 它的回调是"值变了才回调", 而本程序没有"源数据陈旧"
   判定 (`watchdog_ms` 只管管线有没有在出帧), 所以停住的值会被一直沿用。默认配置用
   `torque` + `vibration`, 不碰它; 若你手动改成 `viscosity`, `load_config` 会打一条 WARN
   提醒。判定函数 `is_held_risk_source()` 有离线用例覆盖。
5. **`vibration` 是事件型信号**, 大部分时间是安静的 (实测一轮约 160 次调用, 非零样本只有个位
   数)。两个通道都接它的话, 直道上会几乎没有手感。
6. **`torque` 取的是幅值**, 左转与右转输出同样的力 (方向参数是齿轮/档位索引, 不是左右, 见
   `re/indrun_hook_mechanics.md` 第 4.1 / 5 节), 实测峰值只到 18/127。
7. **力反馈回调是边沿触发的**, 所以"方向盘稳住几秒没有新回调"属于正常现象, 不是掉线;
   卡死恢复后也不需要重启游戏, 回调会自己回来。

---

## 9. 协议要点 (郊狼 V3)

- 服务 `0000180C-…`, 写 `0000150A-…` (≤20 B), 通知 `0000150B-…`, 电量 `0000180A→00001500`
- `B0` 指令 20 字节, **每 100 ms 一帧**: seq(4bit) + A/B 强度解读方式(各 2bit) +
  A/B 强度(1 B each) + A 频率×4 + A 波形强度×4 + B 频率×4 + B 波形强度×4
- 每通道 4 组, **每组 25 ms**; 通道强度 0–200 (越界按 0); 波形频率 10–240; 波形强度 0–100
- **某通道 8 个值里只要有 1 个越界, 该通道 4 组全部丢弃**
- 只输出单通道时给另一通道塞一个非法值 (例如波形强度 101)
- 强度变化时序列号非 0, 且**要等 B1 回来**才能发下一次强度修改
- 官方仓库 [`dungeonlab-open/dglab-bluetooth-protocol`](https://github.com/dungeonlab-open/dglab-bluetooth-protocol)
  只有 **BLE** 规范 (`coyote/v3/README.md`)
- 另有 **SOCKET (WebSocket) 控制协议**, 与本 mod 走的 BLE 完全独立:
  - **V3** (旧): 规范在
    [`dungeonlab-open/dglab-websocket-simple`](https://github.com/dungeonlab-open/dglab-websocket-simple)
    的 `socket/` 目录, **该仓库已归档**; 一对一配对, JSON 四字段 `{type,clientId,targetId,message}`,
    波形按 `pulse-A:[...]` 批量队列下发。
  - **V4** (现行, 官方推荐): 中继服务器
    [`dungeonlab-open/dglab-websocket-server`](https://github.com/dungeonlab-open/dglab-websocket-server)
    的 `v4-server.ts` (默认 9998), SDK 是
    [`dungeonlab-open/dglab-kit`](https://github.com/dungeonlab-open/dglab-kit)。**一个控制端对多个 APP**;
    控制器先连上拿到 `clientId`, APP 用 `?tid=<clientId>` 连进来; 设备命令不在服务器解析, 而是
    `data` 里的 RPC (`device.op` / `device.op.clear` / `devices.get`), 由 SDK 生成; 意图是
    **定时任务** (`addIntensity` / `setTempIntensity(…,duration)` / `sendPulse(…,duration,frames)`,
    带 priority 与 immediate), 不是逐帧字节。
  - 两条路**都不是 BLE 的替代品**: 中继服务器没有蓝牙栈, 手机 APP 仍是 BLE 中枢, 也必须扫码配对。
    PC 只是多一跳网络把命令交给 APP。本项目选 BLE 直连的理由见
    `docs/research-dglab-transport.md` 第 1.1 / 1.2 节; `[link] mode=websocket` 因此仍未实现

  项目后续可能会提供使用Socket连接方案的替代版本

---

## 10. 许可与署名

本项目自身以 **MIT** 许可发布, 全文见 [LICENSE](LICENSE)。

第三方依赖与资料:

| | 许可 |
|---|---|
| [MinHook](https://github.com/TsudaKageyu/minhook) | MIT |
| [RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) | MIT |
| `_ue4ss_ref/ue4ss_0196ef29*/` (UE4SS 头文件) | MIT (同 RE-UE4SS) |
| [dglab-bluetooth-protocol](https://github.com/dungeonlab-open/dglab-bluetooth-protocol) | 上游无 LICENSE 文件, 见下方说明 |

> ⚠ 郊狼协议文档对爱好者免费, 但**商用需事先授权**。
> 上游没有 LICENSE 文件, 所以 `re/reference/dglab-bluetooth-protocol/` 里那四份官方文档的
> 授权条件**不适用**本仓库根目录的 MIT LICENSE。它们的来源、提交号与 sha256 记在
> 该目录的 `PROVENANCE.md` 里; `docs/` 下的分析文字则是我们自己写的。
