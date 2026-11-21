# Hub 消息协议 v1（主题 / 信封 / 单位）

> 对应方案 §5.3、§5.4、§10.3。本文是 ESP32 固件、泰山派 Agent、Windows Agent 的**唯一约定来源**。
> 改动协议必须升 `schema_version`，不得在 v1 里改字段含义。

**版本历史**

| schema_version | 变更 | 兼容性 |
|---|---|---|
| 1 | 初版：cpu / memory / temperature / disk / services / fan | — |
| 2 | metrics 增加可选对象 `gpu`（利用率、温度、显存）；其余字段含义不变 | 订阅方收 1 和 2 都合法；发布方发 2 后，只认 1 的老订阅方会丢帧 |
| 3 | metrics 增加 `cpu.physical_cores`、`processes[]`/`process_count`、`network`、`disks[]`（`disk` 单对象保留为主分区） | 订阅方收 1–3 都合法 |
| 4 | metrics 增加 `containers[]`/`container_count`（docker 运行中的容器） | 订阅方收 1–4 都合法；Linux 端发布 4，Windows 端仍发 3 |
| 5 | metrics 增加 `checks[]`（5 项任务状态；泰山派=闲鱼助手/网络代理/ESP32通信/Tailscale/摄像头，云服务器=1Panel/OpenResty/Docker/Tailscale/New API） | 订阅方收 1–5 都合法；各节点按自己的 `HUB_CHECKS_PROFILE` 发布 5 条 |
| 6 | metrics 的 `network` 增加 `interfaces[]` / `active_id` / `active_type`（分网卡监控 + 当前上网卡），旧 `rx/tx_bytes_per_sec` 语义改为“当前上网卡速率” | 订阅方收 1–6 都合法；Windows 发布 6，泰山派仍发 5 |

---

## 1. 主题命名

```text
hub/v1/devices/{device_id}/status      设备在线状态（retained）
hub/v1/devices/{device_id}/metrics     设备指标（不 retained）
hub/v1/events                          系统事件（上线/离线/告警）
hub/v1/commands/results/{request_id}   控制结果回执（S3 起使用）
```

| device_id | 设备 |
|---|---|
| `taishan-3m` | 泰山派 3M（RK3576 / Ubuntu），本地 Hub 宿主 |
| `windows-main` | Windows 11（S4） |
| `cloud-main` | 云服务器（Tailscale 已接入，只读监控） |

`device_id` 只允许 `[a-z0-9-]`，长度 ≤ 32。

### 1.1 retained 规则（安全红线）

| 主题 | retained | 原因 |
|---|---|---|
| `devices/*/status` | **是** | 新订阅者能立刻知道设备上次的状态 |
| `devices/*/metrics` | **否** | 指标高频且必须「新鲜」，不提供陈旧缓存 |
| `commands/results/*` | **否** | 命令结果不得在重连后自动补投 |

**retained 的 status 只是「最后一次广播」，不代表设备现在在线。**
订阅方必须用 `timestamp_utc + valid_for_seconds` 判断是否过期（见 §3）。

---

## 2. 消息信封

所有消息都是 UTF-8 JSON 对象，顶层固定这些字段：

```json
{
  "schema_version": 3,
  "device_id": "taishan-3m",
  "timestamp_utc": "2026-11-19T09:30:00Z",
  "message_id": "taishan-3m-1732008600-17",
  "valid_for_seconds": 8,
  "payload": { }
}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `schema_version` | int | 当前最高为 `6`（订阅方须接受 `1`–`6`） |
| `device_id` | string | 发布设备自身的 ID |
| `timestamp_utc` | string | ISO-8601 UTC，秒级精度，以设备本地时钟为准 |
| `message_id` | string | 设备内单调递增，用于去重与排查 |
| `valid_for_seconds` | int | 超过 `timestamp_utc + valid_for_seconds` 即视为**过期数据**，UI 不得显示为当前值 |
| `payload` | object | 见 §4 |

---

## 3. 在线判定（订阅方必须实现）

优先级从高到低：

1. **Hub 不可达**（MQTT 连不上）→ 所有设备显示「未知」，不得沿用上次绿色状态。
2. 收到 LWT 的 `state: "offline"` → 离线。
3. 收到 `state: "online"` 且 `now <= timestamp_utc + valid_for_seconds` → 在线。
4. 收到 `state: "online"` 但已超时 → **过期/未知**，不是在线。
5. 设备重启后一直没收到消息 → 「未知」，不是离线也不是在线。

「在线」与「数据新鲜」是两件事：设备可以在线但指标过期。

---

## 4. payload 定义

### 4.1 status

```json
{
  "state": "online",
  "agent_version": "0.1.0",
  "hostname": "squirrel-lckfb",
  "ips": ["192.168.0.156"],
  "uptime_seconds": 86123,
  "clock_synced": true
}
```

发布节奏：连接成功立即发布一次，之后每 5 s 一次，`valid_for_seconds = 15`。
LWT（Broker 代发）固定为：

```json
{ "state": "offline", "agent_version": "0.1.0", "hostname": "squirrel-lckfb", "ips": [], "uptime_seconds": 0, "clock_synced": false }
```

`clock_synced=false` 时订阅方应提示「该设备时间未同步，过期判断不可信」。

### 4.2 metrics

```json
{
  "cpu": {
    "usage_percent": 12.4,
    "load1": 0.42, "load5": 0.35, "load15": 0.28,
    "cores": 8,
    "physical_cores": 4
  },
  "memory": {
    "total_bytes": 4089008128,
    "used_bytes": 1834567680,
    "used_percent": 44.9,
    "swap_used_percent": 0.0
  },
  "temperature": {
    "soc_celsius": 46.5,
    "max_celsius": 48.1,
    "zones": { "soc-thermal": 46.5, "gpu-thermal": 48.1 }
  },
  "disk": {
    "path": "/",
    "total_bytes": 63000000000,
    "used_bytes": 12000000000,
    "used_percent": 19.0
  },
  "gpu": {
    "name": "NVIDIA GeForce RTX 4060 Laptop GPU",
    "usage_percent": 8.0,
    "temp_celsius": 43.0,
    "memory_used_bytes": 1288490188,
    "memory_total_bytes": 8585740288
  },
  "processes": [
    { "name": "chrome", "cpu_percent": 12.4, "memory_bytes": 1932735283 },
    { "name": "Code",   "cpu_percent": 3.1,  "memory_bytes": 650117120 }
  ],
  "process_count": 214,
  "network": { "rx_bytes_per_sec": 8500, "tx_bytes_per_sec": 12300 },
  "disks": [
    { "path": "C:\\", "used_percent": 45.3, "used_bytes": 243349258240, "total_bytes": 536873005056 },
    { "path": "D:\\", "used_percent": 41.1, "used_bytes": 386054561792, "total_bytes": 938920112128 }
  ],
  "uptime_seconds": 86123,
  "services": { "mosquitto": "active" },
  "fan": { "pwm": 128 }
}
```

单位约定：百分比 `0–100` 的浮点；温度摄氏浮点；容量字节整数；负载为内核原始值。

字段缺失（例如没有温度传感器、没有 swap）用 `null` 或不出现，**不要用 0 冒充**。

#### gpu（schema v2 新增，可选）

| 字段 | 类型 | 说明 |
|---|---|---|
| `name` | string | 显卡型号原文，例如 `NVIDIA GeForce RTX 4060 Laptop GPU`；取不到可为 `null` |
| `usage_percent` | number | GPU 利用率，`0–100` |
| `temp_celsius` | number | GPU 温度，摄氏 |
| `memory_used_bytes` / `memory_total_bytes` | int | 显存占用/总量，字节；两者须同时可用，否则 UI 无法算比例 |

约定：

- 没有独立显卡、或平台读不到 GPU 时，整个 `gpu` 对象可以**不出现**；UI 会分项显示「未上报」。
- `gpu` 的每个子字段都可以单独为 `null`（例如只给利用率、不给温度），UI 必须逐项判断，不能因为一个子字段缺失就把整块 GPU 说成没有。
- 目前没有 `fans` 字段：笔记本风扇转速由 EC 私有通道控制，`nvidia-smi` 恒报 `N/A`，`Win32_Fan` 返回空。宁可不显示，也不上报假转速。
- 采集来源：Windows 侧 `nvidia-smi --query-gpu=name,utilization.gpu,temperature.gpu,memory.used,memory.total --format=csv,noheader,nounits`，刷新周期 5 s（起进程约 60–120 ms，比读 sysfs 贵）。

#### v3 新增字段（均可选，缺失即不显示）

| 字段 | 类型 | 说明 |
|---|---|---|
| `cpu.physical_cores` | int | 物理核数；`cpu.cores` 是逻辑核（Windows 上是线程数、Linux 上是 `/proc/cpuinfo` 的处理器数） |
| `processes` | array | 进程列表，最多 3 条，**按内存占用降序**（同内存再按 CPU 降序）；每项 `name` / `cpu_percent` / `memory_bytes` |
| `process_count` | int | 进程总数（包含读不到 CPU 的受保护进程，别用数组长度冒充） |
| `network` | object | `rx_bytes_per_sec` / `tx_bytes_per_sec`，字节每秒，由累计收发字节差分得到 |
| `disks` | array | 固定磁盘分区列表（最多 4 个）；`disk` 仍然表示主分区，两者并存 |

约定：

- 进程 CPU% 按逻辑核数归一，口径与 Windows 资源管理器一致，上限 100。
- 网速需要两次采样，首帧给 `null`（显示 "--"），不拿 0 冒充「真的没流量」。
- Linux（泰山派）暂不上报 `processes` / `network` / `disks`，屏幕上对应卡片会回落到「温区温度 / 平均负载」，这是订阅方的能力回退，不是数据错误。

#### v4 新增字段（均可选，缺失即不显示）

| 字段 | 类型 | 说明 |
|---|---|---|
| `containers` | array | docker 中**运行中**的容器，最多 6 条；每项 `name`（容器全名）/ `service`（compose 服务名，非 compose 起的为空串）/ `state`（`running`） |
| `container_count` | int | 运行中的容器总数；读不到 docker 时为 `null`（表示「读不到」而不是「没有容器」） |

约定：

- 采集用 `docker ps --format`（约 30–60 ms），刷新周期 15 s；不调 `docker stats`（那要等一个采样周期，2 s 的发布节奏扛不住）。
- 当前用户不在 docker 组、daemon 未启动、未装 docker，都只让 `container_count = null`，不报错、不影响其它字段。
- 屏幕上 `containers` 只展示前 4 条；没有 GPU 的设备用它填右半列（有 GPU 的设备仍显示 GPU）。

#### v5 新增字段（可选，缺失即视为未上报）

| 字段 | 类型 | 说明 |
|---|---|---|
| `checks` | array | 固定 5 条任务状态，顺序按设备 profile：泰山派为 闲鱼助手 / 网络代理 / ESP32通信 / Tailscale / 摄像头（`xianyu`/`proxy`/`esp32`/`tailscale`/`camera`），云服务器为 1Panel / OpenResty / Docker / Tailscale / New API（`1panel`/`openresty`/`docker`/`tailscale`/`newapi`）；每项含 `id`、`label`（中文名）、`state`（`ok`/`warn`/`down`）、`detail`（短说明） |

```json
"checks": [
  { "id": "xianyu",     "label": "闲鱼助手",   "state": "ok",   "detail": "在线" },
  { "id": "proxy",      "label": "网络代理",   "state": "ok",   "detail": "在线" },
  { "id": "esp32",      "label": "ESP32通信",  "state": "ok",   "detail": "在线" },
  { "id": "tailscale",  "label": "Tailscale",  "state": "ok",   "detail": "在线" },
  { "id": "camera",     "label": "摄像头",     "state": "ok",   "detail": "在线" }
]
```

约定：

- `ok` = 正常；`warn` = 进程在但没就绪（如容器 unhealthy、代理未监听、Tailscale 未连上）；`down` = 未运行/未检测到。订阅方不得把 `warn` 显示成正常。
- 判定来源：闲鱼容器 `docker ps` 的 healthy 状态；代理 = `mihomo` 进程 + `127.0.0.1:7890` 监听；ESP32通信 = `1883` 上存在来自 `192.168.0.100` 的 ESTABLISHED 连接；Tailscale = `tailscaled` active + `BackendState=Running`；摄像头 = `/dev/video*` 存在 + `rkaiq_3A` active。
- 采集周期 5 s（docker 快照复用 15 s 缓存）；单项失败只影响自己，整帧照发。
- `checks` 缺失或不是数组时，订阅方按「未上报」处理（屏幕显示“等待数据”），不得显示成 0 项或全绿。
- 没有 GPU 的设备用 `checks` 填监控页右半列（原“运行容器”位置）；有 GPU 的设备仍显示 GPU，容器/任务状态不上屏。

#### v6 新增字段（可选，缺失即回落到旧的单网卡样式）

| 字段 | 类型 | 说明 |
|---|---|---|
| `network.interfaces` | array | 物理网卡，最多 2 条（有线、无线各 1 张）；每项 `id`（网卡别名，如 `以太网 5` / `WLAN`）、`type`（`wired`/`wifi`）、`status`（`up`/`down`）、`link_mbps`（协商速率，可空）、`rx_bytes_per_sec` / `tx_bytes_per_sec`（可空） |
| `network.active_id` | string/null | 当前默认出口的网卡别名，对应 `interfaces[].id`；判定来源是 `Find-NetRoute` 到外网地址的 `ifIndex` |
| `network.active_type` | string/null | `wired`/`wifi`/`other`/`null`；`active_id` 不在 `interfaces` 里（例如出口是 VPN 虚拟网卡）时为 `other` |

```json
"network": {
  "rx_bytes_per_sec": 6792,
  "tx_bytes_per_sec": 14528,
  "interfaces": [
    { "id": "以太网 5", "name": "以太网 5", "type": "wired", "status": "up",
      "link_mbps": 1000, "rx_bytes_per_sec": 6792, "tx_bytes_per_sec": 14528 },
    { "id": "WLAN", "name": "WLAN", "type": "wifi", "status": "down",
      "link_mbps": 0, "rx_bytes_per_sec": 0, "tx_bytes_per_sec": 0 }
  ],
  "active_id": "以太网 5",
  "active_type": "wired"
}
```

约定：

- 只报物理网卡：`PhysicalMediaType` 为 `802.3` 记 `wired`，`Native 802.11` 记 `wifi`；VMware / Hyper-V / vEthernet / Tailscale / Loopback / Bluetooth / TAP 一律排除。
- 断开的那张卡也保留（`status=down`、`link_mbps=0`），屏幕显示“未接入”，不隐藏行。
- 速率是每张卡各自的累计计数器差分；首次采样、计数器回绕给 `null`，不出现负值；采样间隔 30 s（一次 PowerShell 约 2–4 s，不能跟着 2 s 的发布节奏）。
- `rx_bytes_per_sec` / `tx_bytes_per_sec` 保留是为了兼容只认旧字段的订阅方（v1–v5），其值等于 `active_id` 那张卡的速率；找不到出口时给 `null`，不拿全机合计冒充。
- `interfaces` 缺失或为空数组时，订阅方回落到旧 ip + ↓/↑ 单行样式（泰山派 5 帧属于这一种）。



> 泰山派 3M 实测：`/sys/class/thermal/thermal_zone0..5` 提供 6 个温区
> （soc / bigcore / little-core / ddr / npu / gpu），`end0` 为千兆网口。

发布节奏：每 2 s 一条（温度 5 s、磁盘 30 s、服务 5 s 内部刷新，合并到同一帧），
`valid_for_seconds = 8`。

---

## 5. 凭据与 ACL（每设备独立）

| 用户名 | 用途 | 权限 |
|---|---|---|
| `taishan-agent` | 泰山派 Agent 发布者 | 只能写 `hub/v1/devices/taishan-3m/{status,metrics}` |
| `windows-agent` | Windows 主机 Agent 发布者 | 只能写 `hub/v1/devices/windows-main/{status,metrics}` |
| `esp32-terminal` | ESP32 终端订阅者 | 读 `hub/v1/devices/#`、`hub/v1/events`；写 `hub/v1/commands/results/#` |
| `ops` | 人工运维调试 | 读写全部 + 读 `$SYS/#` |

- 匿名连接必须被拒绝（`allow_anonymous false`）。
- 凭据只存在于 `/etc/taishan-hub/credentials.env`（root 600），不进仓库、不进日志。
- 1883 端口只允许局域网访问，**不得**在路由器做端口映射。
