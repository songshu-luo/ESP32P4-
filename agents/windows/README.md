# Windows 指标 Agent（S4-a）

把本机（Windows 11，`device_id = windows-main`）的 CPU / 内存 / 磁盘 / 运行时长
发布到泰山派 Hub，ESP32 终端从 `hub/v1/devices/windows-main/*` 读取。

协议见 [`protocol/topics-v1.md`](../../protocol/topics-v1.md)，与泰山派 Agent 完全一致：

| 项目 | 值 |
| --- | --- |
| status 主题 | `hub/v1/devices/windows-main/status`，retained，5 s 一帧，`valid_for_seconds=15` |
| metrics 主题 | `hub/v1/devices/windows-main/metrics`，不 retained，2 s 一帧，`valid_for_seconds=8` |
| LWT | status 主题上 `state=offline`，retained |
| 正常退出 | 显式补发 `state=offline`（任务计划程序停止属于正常断开，LWT 不会触发） |

## 依赖

- Python 3.11（本机已验证 3.11.9）
- `paho-mqtt`（本机已验证 2.2.0）
- **不需要 psutil**：全部指标用 `ctypes` 调 Win32 API，见 `metrics.py` 顶部说明

## 文件

| 文件 | 作用 |
| --- | --- |
| `windows_agent.py` | 主循环、MQTT、LWT、信封构造 |
| `metrics.py` | ctypes 采集（`GetSystemTimes` / `GlobalMemoryStatusEx` / `GetDiskFreeSpaceExW`） |
| `config.py` | 读 `%ProgramData%\taishan-hub\windows-agent.env` |
| `windows-agent.env.example` | 配置模板（真实密码不在此文件） |
| `install-windows.ps1` | 生成配置、注册开机启动任务 |

## 手动跑通

```powershell
cd <仓库>\agents\windows
python windows_agent.py
```

期望日志：

```
启动：device=windows-main broker=192.168.0.156:1883 python=3.11.9
已连接 192.168.0.156:1883
```

验证收到数据（在泰山派上执行）：

```bash
mosquitto_sub -h 127.0.0.1 -u <ops 账号> -P <ops 密码> -t 'hub/v1/devices/windows-main/#' -v
```

## 字段约定（哪些是 null）

| 字段 | 本机取值 | 原因 |
| --- | --- | --- |
| `cpu.usage_percent` | 数值 | `GetSystemTimes` 两次采样差值；刚启动第一帧为 `null` |
| `cpu.load1/5/15` | **`null`** | Windows 没有 loadavg，不拿 CPU 使用率冒充 |
| `memory.swap_used_percent` | **`null`** | Windows 的 pagefile 混着物理内存，算出来是错的 |
| `temperature.*` | **`null`** | 普通权限读不到 CPU 传感器，不编数字 |
| `services` | 由 `WINDOWS_SERVICES` 决定 | 查不到的服务不出现在结果里 |

屏幕侧规则：字段为 `null` 显示 `--`，绝不显示 `0`。

## 时间同步的判断

`status.payload.clock_synced` 读注册表
`HKLM\SYSTEM\CurrentControlSet\Services\W32Time\Config\LastKnownGoodTime`
（REG_QWORD，FILETIME），超过 7 天没同步则给 `false`。

**不用 `w32tm /query /status`**：它的输出随系统语言变化，中文系统上是
本地化文本，解析必错（本项目踩过）。

## 开机自启（先手动跑通再做）

见 `install-windows.ps1`。安装后任务名 `TaishanHubWindowsAgent`：

```powershell
schtasks /Query /TN TaishanHubWindowsAgent /V /FO LIST   # 查看
schtasks /Run   /TN TaishanHubWindowsAgent               # 立即启动
schtasks /End   /TN TaishanHubWindowsAgent               # 停止（会发 offline）
```

日志：`%ProgramData%\taishan-hub\logs\windows-agent.log`（1 MB × 3 轮转）