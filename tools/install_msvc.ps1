# ---------------------------------------------------------------------------
# DGLab_IDAC — 极简安装脚本: Visual Studio Build Tools 装到 E 盘
#
# 为什么需要这个脚本:
#   C: 只剩 18.6 GB, 而 VS Build Tools 默认会往 C: 写 3~8 GB
#   (installer 本体 + package cache + shared 目录 + Windows SDK)。
#   VS 安装器支持 --path 显式重定向 cache / shared / install 三个位置,
#   但 cache 和 shared **只能在第一次安装时设定**, 之后再改要重装,
#   所以必须一次性写对。
#
# 用法(必须用【管理员】PowerShell 运行):
#   pwsh -File tools\install_msvc.ps1
#   或者先看它要干什么:
#   pwsh -File tools\install_msvc.ps1 -DryRun
#
# 参考:
#   https://learn.microsoft.com/en-us/visualstudio/install/use-command-line-parameters-to-install-visual-studio
#   https://learn.microsoft.com/en-us/visualstudio/install/workload-component-id-vs-build-tools
# ---------------------------------------------------------------------------

[CmdletBinding()]
param(
    # 所有 VS 组件装到这里
    [string]$InstallPath = 'E:\VS\BuildTools',
    # 下载下来的安装包缓存(可被 repair/modify 复用; 3~5 GB)
    [string]$CachePath   = 'E:\VS\cache',
    # VS 各实例共享的组件目录
    [string]$SharedPath  = 'E:\VS\shared',
    # 引导程序临时存放处
    [string]$BootstrapperDir = 'E:\VS',
    # 只打印命令不执行
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

function Write-Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Write-Warn($msg) { Write-Host "!!  $msg" -ForegroundColor Yellow }

# ---------------------------------------------------------------------------
# 0. 前置检查
# ---------------------------------------------------------------------------
if (-not ([Security.Principal.WindowsPrincipal] `
          [Security.Principal.WindowsIdentity]::GetCurrent()
         ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "必须用管理员权限运行 —— VS 安装器要写 HKLM 和 Program Files。"
}

foreach ($p in @($InstallPath, $CachePath, $SharedPath, $BootstrapperDir)) {
    $drive = (Split-Path -Qualifier $p).TrimEnd(':')
    $d = Get-PSDrive -Name $drive -ErrorAction SilentlyContinue
    if (-not $d) { throw "盘符不存在: $p" }
    Write-Host ("    {0,-22} 所在盘 {1}: 剩余 {2:N1} GB" -f $p, $drive, ($d.Free / 1GB))
}

# C: 至少要留出 15 GB: 即便 cache/shared 都搬走了,
# Windows SDK、.NET targeting pack、MSBuild、VC redist 仍然写 C:,
# 而且安装过程会在 %TEMP% 里解包。
$cFree = (Get-PSDrive -Name C).Free / 1GB
if ($cFree -lt 12) {
    Write-Warn ("C: 只剩 {0:N1} GB, 建议先清理到 15 GB 以上再装。" -f $cFree)
} else {
    Write-Host ("    C: 剩余 {0:N1} GB (SDK 等必要组件会写这里)" -f $cFree)
}

# ---------------------------------------------------------------------------
# 1. 下载引导程序
# ---------------------------------------------------------------------------
$vsExe = Join-Path $BootstrapperDir 'vs_buildtools.exe'
if (Test-Path $vsExe) {
    Write-Step "已有引导程序, 跳过下载: $vsExe"
} else {
    Write-Step "下载 vs_buildtools.exe -> $vsExe"
    New-Item -ItemType Directory -Force -Path $BootstrapperDir | Out-Null
    $url = 'https://aka.ms/vs/17/release/vs_buildtools.exe'
    Write-Host "    $url"
    if (-not $DryRun) {
        Invoke-WebRequest -Uri $url -OutFile $vsExe -UseBasicParsing
        $sz = (Get-Item $vsExe).Length / 1MB
        Write-Host ("    完成, {0:N2} MB" -f $sz)
        # 引导程序是签名的, 顺手验一下
        $sig = Get-AuthenticodeSignature $vsExe
        Write-Host ("    签名: {0} ({1})" -f $sig.Status, $sig.SignerCertificate.Subject)
        if ($sig.Status -ne 'Valid') { throw "引导程序签名校验失败: $($sig.Status)" }
    }
}

# ---------------------------------------------------------------------------
# 2. 组织安装参数
# ---------------------------------------------------------------------------
# 组件选择原则: 只要"能编译出 x64 DLL"这一件事。
#   * 用 Microsoft.VisualStudio.Component.VC.Tools.x86.x64 而不是整个
#     Microsoft.VisualStudio.Workload.VCTools 工作负载 —— 后者会顺带拉
#     vcpkg / CMake / ASan / 测试工具, 多 1~2 GB。本工程用 D: 盘已有的
#     CMake(D:\Program Files\CMake) + Ninja, 不需要 VS 那套。
#   * Windows11SDK.26100 给 Windows 头文件和 lib。本机没有 Windows Kits,
#     必须装。
#   * VC.Redist.14.Latest 是 CRT 头文件/lib 的前置。
$components = @(
    'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'   # MSVC v143 编译器 + 库
    'Microsoft.VisualStudio.Component.VC.Redist.14.Latest' # vcruntime / CRT
    'Microsoft.VisualStudio.Component.Windows11SDK.26100'  # Windows SDK 头+lib
)

$installArgs = @(
    '--quiet'
    '--wait'
    '--norestart'
    '--nocache'          # 装完删包, 省盘; 想保留 repair 能力就去掉这行
    "--installPath", $InstallPath
    '--path', "cache=$CachePath"
    '--path', "shared=$SharedPath"
)
foreach ($c in $components) { $installArgs += @('--add', $c) }

Write-Step "安装参数:"
$installArgs | ForEach-Object { Write-Host "    $_" -NoNewline; Write-Host "" }
Write-Host ""
Write-Host "    注: 本脚本用 --nocache。若想保留离线包以便以后 repair/modify," -ForegroundColor DarkGray
Write-Host "        把 --nocache 去掉, 缓存会占 $CachePath 3~5 GB。" -ForegroundColor DarkGray

if ($DryRun) {
    Write-Step "DryRun: 未执行安装。实际命令:"
    Write-Host "`"$vsExe`" $($installArgs -join ' ')"
    return
}

# ---------------------------------------------------------------------------
# 3. 安装 (10~25 分钟, --wait 会阻塞到装完)
# ---------------------------------------------------------------------------
Write-Step "开始安装 (界面不会出现, 请耐心等待; 日志在 %TEMP%\dd_*.log)"
$sw = [Diagnostics.Stopwatch]::StartNew()
$proc = Start-Process -FilePath $vsExe -ArgumentList $installArgs -Wait -PassThru
$sw.Stop()
$code = $proc.ExitCode
Write-Step ("安装器退出码 {0}, 耗时 {1:N1} 分钟" -f $code, $sw.Elapsed.TotalMinutes)

# 官方退出码表:
#   0 成功 / 3010 成功但需重启 / 1641 成功且已发起重启
#   740 需要提权 / 1602 被取消 / 1618 已有别的安装在进行
#   5007 机器不满足要求 / 5005 命令行解析错误
switch ($code) {
    0     { }
    3010  { Write-Warn "安装成功, 但需要重启一次。" }
    1641  { Write-Warn "安装成功, 系统正在重启。" }
    740   { throw "需要管理员权限。" }
    1602  { throw "安装被取消。" }
    1618  { throw "另一个安装正在进行, 稍后重试。" }
    5005  { throw "命令行参数解析失败 —— 检查 --installPath / --path 的引号。" }
    5007  { throw "机器不满足 VS 2022 的最低要求。" }
    default { throw "安装失败, 退出码 $code。看 %TEMP%\dd_setup_*.log。" }
}

# ---------------------------------------------------------------------------
# 4. 验证: 找到 cl.exe 和 vcvars64.bat
# ---------------------------------------------------------------------------
Write-Step "验证安装"
$vcvars = Join-Path $InstallPath 'VC\Auxiliary\Build\vcvars64.bat'
$clExe  = Get-ChildItem (Join-Path $InstallPath 'VC\Tools\MSVC') -Recurse `
                        -Filter cl.exe -ErrorAction SilentlyContinue |
          Where-Object { $_.FullName -match 'Hostx64\\x64' } | Select-Object -First 1

if (Test-Path $vcvars) { Write-Host "    vcvars64.bat  OK  $vcvars" -ForegroundColor Green }
else { Write-Warn "没找到 $vcvars —— 安装可能没真正完成。" }

if ($clExe) {
    Write-Host "    cl.exe        OK  $($clExe.FullName)" -ForegroundColor Green
    Write-Host ("    版本: " + ((& $clExe.FullName 2>&1 | Select-Object -First 1)))
} else { Write-Warn "没找到 cl.exe。" }

"--- Windows SDK (固定在 C:, 这是官方行为) ---"
Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\Include' -ErrorAction SilentlyContinue |
    Select-Object -ExpandProperty Name | ForEach-Object { Write-Host "    $_" }

"--- 各盘剩余空间 ---"
Get-CimInstance Win32_LogicalDisk -Filter 'DriveType=3' | ForEach-Object {
    Write-Host ("    {0} 剩余 {1:N1} GB" -f $_.DeviceID, ($_.FreeSpace / 1GB))
}

Write-Host ""
Write-Step "完成。下一步:"
Write-Host "    构建 mod (含 vcvars 环境):"
Write-Host "      pwsh -File tools\build.ps1 -Configuration Release"
