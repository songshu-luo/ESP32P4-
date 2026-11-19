# Hub 消息协议 v1（主题 / 信封 / 单位）

> 对应方案 §5.3、§5.4、§10.3。本文是 ESP32 固件、泰山派 Agent、Windows Agent 的**唯一约定来源**。
> 改动协议必须升 `schema_version`，不得在 v1 里改字段含义。

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
| `cloud-main` | 云服务器（S5） |

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
  "schema_version": 1,
  "device_id": "taishan-3m",
  "timestamp_utc": "2026-11-19T09:30:00Z",
  "message_id": "taishan-3m-1732008600-17",
  "valid_for_seconds": 8,
  "payload": { }
}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `schema_version` | int | 当前为 `1` |
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
    "cores": 8
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
  "uptime_seconds": 86123,
  "services": { "mosquitto": "active" },
  "fan": { "pwm": 128 }
}
```

单位约定：百分比 `0–100` 的浮点；温度摄氏浮点；容量字节整数；负载为内核原始值。

字段缺失（例如没有温度传感器、没有 swap）用 `null` 或不出现，**不要用 0 冒充**。

> 泰山派 3M 实测：`/sys/class/thermal/thermal_zone0..5` 提供 6 个温区
> （soc / bigcore / little-core / ddr / npu / gpu），`end0` 为千兆网口。

发布节奏：每 2 s 一条（温度 5 s、磁盘 30 s、服务 5 s 内部刷新，合并到同一帧），
`valid_for_seconds = 8`。

---

## 5. 凭据与 ACL（每设备独立）

| 用户名 | 用途 | 权限 |
|---|---|---|
| `taishan-agent` | 泰山派 Agent 发布者 | 只能写 `hub/v1/devices/taishan-3m/{status,metrics}` |
| `esp32-terminal` | ESP32 终端订阅者 | 读 `hub/v1/devices/#`、`hub/v1/events`；写 `hub/v1/commands/results/#` |
| `ops` | 人工运维调试 | 读写全部 + 读 `$SYS/#` |

- 匿名连接必须被拒绝（`allow_anonymous false`）。
- 凭据只存在于 `/etc/taishan-hub/credentials.env`（root 600），不进仓库、不进日志。
- 1883 端口只允许局域网访问，**不得**在路由器做端口映射。
