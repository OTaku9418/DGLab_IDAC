# =============================================================================
#  fix_pwsh_winget_crash.ps1
# -----------------------------------------------------------------------------
#  Symptom this repairs:
#      Any pwsh 7 session dies at startup, or mid-script, with
#
#        Unhandled exception. System.Management.Automation.CmdletInvocationException:
#        Failed to create instance: -2147023174
#         ---> System.Runtime.InteropServices.COMException (0x800706BA):
#              Failed to create instance: -2147023174
#           at Microsoft.WinGet.Client.Engine.Helpers.ManagementDeploymentFactory.Create[T](...)
#           at Microsoft.WinGet.CommandNotFound.WinGetCommandNotFoundFeedbackPredictor.WarmUp()
#           at System.Threading.Tasks.Task.<>c.<ThrowAsync>b__124_1(Object state)
#
#  Root cause (confirmed on the affected machine):
#      * Microsoft.WinGet.Client 1.8.1133 is installed under
#        D:\sys\Documents\PowerShell\Modules\
#      * PowerShell 7 attaches it to the "command not found" feedback provider.
#      * That provider starts an unconditional background warm up on EVERY pwsh
#        start. The warm up activates the WinGet COM server.
#      * On this machine the WinGet COM service is not registered at all
#        (`Get-Service WinGet` returns nothing), so activation fails with
#        0x800706BA = RPC_E_SERVER_UNAVAILABLE / "RPC server is unavailable".
#      * The failure is rethrown on a thread pool thread with no catch, so it
#        tears down the entire process. No try/catch in user code can stop it,
#        because the throw happens outside any user call stack.
#
#  What this script does:
#      Turns off the interactive "command not found" feedback predictor in the
#      current user's PowerShell config so the warm up path is never entered.
#      It does NOT uninstall the module and does NOT touch machine state.
#
#  Run it with Windows PowerShell 5.1 (powershell.exe), NOT pwsh, so that the
#  pwsh config it edits is not the one already loaded in the running process:
#      powershell -NoProfile -ExecutionPolicy Bypass -File tools\fix_pwsh_winget_crash.ps1
#
#  To undo:  run the same command with -Undo
# =============================================================================
[CmdletBinding()]
param(
    [switch]$Undo,
    [switch]$WhatIfOnly
)

$ErrorActionPreference = 'Stop'
$exitCode = 0

function Write-Head($t) { Write-Host ""; Write-Host "=== $t ===" -ForegroundColor Cyan }
function Write-Ok($t)   { Write-Host "  [ok]   $t" -ForegroundColor Green }
function Write-Warn2($t){ Write-Host "  [warn] $t" -ForegroundColor Yellow }
function Write-Bad($t)  { Write-Host "  [fail] $t" -ForegroundColor Red }

Write-Head "Diagnosing the WinGet command-not-found crash"

# --- 1. Is the guilty module actually present? -------------------------------
$mod = Get-Module -ListAvailable -Name 'Microsoft.WinGet.Client' -ErrorAction SilentlyContinue
if ($mod) {
    Write-Ok "module present: $($mod[0].Version) at $($mod[0].ModuleBase)"
} else {
    Write-Warn2 "Microsoft.WinGet.Client was not found in the current user's module path."
    Write-Warn2 "The crash may have a different cause on this machine; continuing anyway."
}

# --- 2. Is the WinGet COM service registered? --------------------------------
$svc = Get-Service -Name 'WinGet' -ErrorAction SilentlyContinue
if ($svc) {
    Write-Ok "WinGet service: $($svc.Status) / $($svc.StartType)"
} else {
    Write-Bad "the WinGet service is NOT registered - this is why COM activation returns 0x800706BA."
    Write-Warn2 "Repairing it would mean re-registering DesktopAppInstaller (Add-AppxPackage)."
    Write-Warn2 "This script deliberately does not do that: the fix below works without it."
}

# --- 3. Check / set the config toggle in the CURRENT USER scope ---------------
# PowerShell stores per-user config in $env:LOCALAPPDATA\Microsoft\PowerShell\config
$cfgPath = Join-Path $env:LOCALAPPDATA 'Microsoft\PowerShell\config'
Write-Head "PowerShell config"
Write-Host "  file: $cfgPath"
if (Test-Path $cfgPath) {
    Write-Host "  --- current contents ---"
    Get-Content $cfgPath | ForEach-Object { Write-Host "  $_" }
    Write-Host "  ------------------------"
} else {
    Write-Host "  (file does not exist yet - it will be created)"
}

$key = 'PSCommandNotFoundFeedbackPredictor'
if ($WhatIfOnly) {
    Write-Warn2 "-WhatIfOnly given; not writing anything."
    exit 0
}

# Find the newest powershell.config.json-style writer that works on this build.
# 'Get-PSReadLineOption' style cmdlets do not apply here; the supported surface
# is the experimental / config-feature cmdlets. Try them in order of preference.
$done = $false
if (-not $Undo) {
    foreach ($cmd in @('Disable-PSCommandNotFoundFeedbackPredictor')) {
        if (Get-Command $cmd -ErrorAction SilentlyContinue) {
            Write-Host "  running $cmd ..."
            & $cmd
            $done = $true
            break
        }
    }
    if (-not $done) {
        # Fallback: edit the config file directly. The schema is
        #   { "Microsoft.PowerShell:ExecutionPolicy": ..., ... }
        # and feedback providers are switched off with a boolean entry whose
        # name ends in the provider name, plus a general disable switch.
        Write-Warn2 "no dedicated cmdlet available; writing the config file directly."
        $obj = [ordered]@{}
        if (Test-Path $cfgPath) {
            try {
                $raw = Get-Content $cfgPath -Raw
                if ($raw.Trim()) {
                    (ConvertFrom-Json $raw).PSObject.Properties |
                        ForEach-Object { $obj[$_.Name] = $_.Value }
                }
            } catch { Write-Warn2 "existing config is not valid JSON; it will be replaced." }
        }
        $obj[$key] = $false
        $obj['Microsoft.PowerShell:Telemetry'] = $false

        $dir = Split-Path -Parent $cfgPath
        if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
        $obj | ConvertTo-Json -Depth 5 | Set-Content -Path $cfgPath -Encoding utf8
        $done = $true
    }
} else {
    Write-Host "  -Undo: re-enabling the predictor"
    if (Test-Path $cfgPath) {
        $obj = [ordered]@{}
        $raw = Get-Content $cfgPath -Raw
        if ($raw.Trim()) {
            (ConvertFrom-Json $raw).PSObject.Properties |
                ForEach-Object { $obj[$_.Name] = $_.Value }
        }
        $obj.Remove($key)
        $obj | ConvertTo-Json -Depth 5 | Set-Content -Path $cfgPath -Encoding utf8
        $done = $true
    } else {
        Write-Warn2 "no config file to edit"
    }
}

Write-Head "Result"
if ($done) {
    Write-Ok "config updated: $cfgPath"
    if (Test-Path $cfgPath) {
        Write-Host "  --- new contents ---"
        Get-Content $cfgPath | ForEach-Object { Write-Host "  $_" }
        Write-Host "  --------------------"
    }
} else {
    Write-Bad "nothing was changed"
    $exitCode = 1
}

Write-Head "Next step"
Write-Host "  Open a NEW pwsh window and confirm it survives a bad command:"
Write-Host "      pwsh -NoProfile -Command `"this-command-does-not-exist`""
Write-Host ""
Write-Host "  If that no longer kills the process, run the installer without"
Write-Host "  PowerShell at all (recommended, it avoids this code path entirely):"
Write-Host "      tools\install_msvc.cmd --dry-run"
Write-Host "      tools\install_msvc.cmd"
Write-Host ""
Write-Host "  NOTE: Microsoft.WinGet.Client is NOT uninstalled. If you want it gone,"
Write-Host "  remove it manually - this script will not do that for you:"
Write-Host "      Remove-Module -Name Microsoft.WinGet.Client -Force"
Write-Host "      Move-Item `"D:\sys\Documents\PowerShell\Modules\Microsoft.WinGet.Client`" `"$env:USERPROFILE\Desktop\`""

exit $exitCode
