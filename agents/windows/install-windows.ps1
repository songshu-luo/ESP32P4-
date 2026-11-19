<#
.SYNOPSIS
  安装 Windows 指标 Agent：生成配置 + 注册开机启动任务。

.DESCRIPTION
  只做两件事：
    1. 把 WINDOWS_AGENT_PASS 写进 %ProgramData%\taishan-hub\windows-agent.env
       （文件权限收紧到只有当前用户 + Administrators 可读）
    2. 注册计划任务 TaishanHubWindowsAgent，登录时用 pythonw.exe 静默启动

  密码从命令行参数传入，因此**不会**被打印到屏幕；也不写进任何仓库文件。

.EXAMPLE
  .\install-windows.ps1 -MqttPassword 'xxxxxxxx'

.EXAMPLE
  # 密码已在环境变量里（不落 history）
  .\install-windows.ps1 -MqttPassword $env:WINDOWS_AGENT_PASS
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$MqttPassword,

    [string]$BrokerHost = '192.168.0.156',
    [int]   $BrokerPort = 1883,
    [string]$MqttUser   = 'windows-agent',
    [string]$TaskName   = 'TaishanHubWindowsAgent'
)

$ErrorActionPreference = 'Stop'

$Here    = Split-Path -Parent $MyInvocation.MyCommand.Path
$EnvDir  = Join-Path $env:ProgramData 'taishan-hub'
$EnvPath = Join-Path $EnvDir 'windows-agent.env'
$LogDir  = Join-Path $EnvDir 'logs'

# ---------------------------------------------------------------- 前置检查

$python = (Get-Command python.exe -ErrorAction SilentlyContinue).Source
if (-not $python) { throw '找不到 python.exe，请先安装 Python 3.11 并加入 PATH。' }

$pythonw = Join-Path (Split-Path -Parent $python) 'pythonw.exe'
if (-not (Test-Path $pythonw)) { $pythonw = $python }   # 退回控制台版，窗口会闪一下

& $python -c "import paho.mqtt.client as m" 2>$null
if ($LASTEXITCODE -ne 0) {
    throw "缺少 paho-mqtt。请先执行：`"$python`" -m pip install paho-mqtt"
}

$agentPy = Join-Path $Here 'windows_agent.py'
if (-not (Test-Path $agentPy)) { throw "找不到 $agentPy" }

# ---------------------------------------------------------------- 写配置

New-Item -ItemType Directory -Force -Path $EnvDir, $LogDir | Out-Null

if (Test-Path $EnvPath) {
    Copy-Item $EnvPath "$EnvPath.bak" -Force
    Write-Host "已备份旧配置到 $EnvPath.bak" -ForegroundColor Yellow
}

@"
# 由 install-windows.ps1 生成，请勿提交到仓库。
WINDOWS_DEVICE_ID=windows-main
MOSQUITTO_HOST=$BrokerHost
MOSQUITTO_PORT=$BrokerPort
WINDOWS_AGENT_USER=$MqttUser
WINDOWS_AGENT_PASS=$MqttPassword
WINDOWS_DISK_PATH=C:\
WINDOWS_SERVICES=W32Time
"@ | Set-Content -Path $EnvPath -Encoding UTF8

# 只允许当前用户与管理员读，避免同机其他账号拿到 MQTT 密码
icacls $EnvPath /inheritance:r /grant:r "$($env:USERNAME):(R,W)" "Administrators:(F)" | Out-Null
Write-Host "配置已写入 $EnvPath（权限已收紧）" -ForegroundColor Green

# ---------------------------------------------------------------- 注册任务

$action  = New-ScheduledTaskAction -Execute $pythonw -Argument "`"$agentPy`"" -WorkingDirectory $Here
$trigger = New-ScheduledTaskTrigger -AtLogOn -User "$env:USERDOMAIN\$env:USERNAME"
$settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -StartWhenAvailable `
    -RestartCount 999 `
    -RestartInterval (New-TimeSpan -Minutes 1) `
    -ExecutionTimeLimit ([TimeSpan]::Zero)     # 不限运行时长

Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger `
    -Settings $settings -Description 'Windows 指标发布到泰山派 Hub（MQTT）' -Force | Out-Null

Write-Host "计划任务 $TaskName 已注册（登录时启动）" -ForegroundColor Green
Write-Host ''
Write-Host '下一步：'
Write-Host "  schtasks /Run   /TN $TaskName        # 立即启动"
Write-Host "  Get-Content '$LogDir\windows-agent.log' -Tail 20 -Wait   # 看日志"
Write-Host "  schtasks /End   /TN $TaskName        # 停止（会发 offline）"