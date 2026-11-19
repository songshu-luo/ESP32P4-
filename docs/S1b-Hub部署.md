# S1-b 泰山派本地 Hub 部署记录

> 日期：2026-11-19
> 阶段：S1-b（Mosquitto Broker + 泰山派指标 Agent）
> 状态：**实机通过，Windows 与 WSL 均可从局域网订阅到真实指标**

---

## 1. 实机环境（本次实测）

| 项目 | 值 |
|---|---|
| 主机名 | `squirrel-lckfb` |
| 型号 | 泰山派 3M，RK3576 |
| 系统 | Ubuntu 24.04.4 LTS（kernel `6.1.99`，aarch64）|
| CPU | 8 核，4 GB RAM，64 GB eMMC（`/dev/root`，已用 27%）|
| 局域网 IP | **`192.168.0.156`**（`end0`，千兆，DHCP）|
| Tailscale IP | `100.67.22.115`（供远程运维）|
| 登录 | `lckfb`，**sudo 免密**（已确认 `sudo -n true` 成功）|
| 已开机时长 | 4 天（16:37 实测）|
| NTP | chrony 在跑，`timedatectl` 显示 `NTPSynchronized=yes` |
| 温度传感器 | `/sys/class/thermal/thermal_zone0..5`：soc / bigcore / little-core / ddr / npu / gpu |
| 风扇 | `hwmon` 里的 `pwmfan`，`pwm1` 0–255 |

> 板上还跑着无关的 Docker 容器（`ydisks-xianyu-helper`，占 59188 端口）与 `mihomo` 代理
> （127.0.0.1:7890 / 9090）。本项目**不碰**它们；Mosquitto 只新开 1883。

---

## 2. 部署结果

| 组件 | 路径 / 单元 | 状态 |
|---|---|---|
| Mosquitto 2.0.18 | `mosquitto.service` | enabled + active |
| 本 Hub 配置 | `/etc/mosquitto/conf.d/taishan-hub.conf` | 已安装 |
| 凭据 | `/etc/taishan-hub/credentials.env`（root:lckfb 640）| 已生成 |
| 口令文件 | `/etc/taishan-hub/mosquitto.passwd`（mosquitto:mosquitto 600）| 3 个账号 |
| ACL | `/etc/taishan-hub/mosquitto.acl`（mosquitto:mosquitto 640）| 已生效 |
| 指标 Agent | `/opt/taishan-hub/agent/{agent,config,metrics}.py` | root 644 |
| Agent 配置 | `/etc/taishan-hub/agent.env`（root:lckfb 640）| 已写入 |
| Agent 单元 | `taishan-hub-agent.service` | enabled + active，`Restart=always` |

源码在仓库 `services/taishan-hub/`，用两个幂等脚本部署：

```bash
# 从 WSL（需已装公钥到板上）
cd ~/esp32_workspace/"Smart terminal"
rsync -a services/taishan-hub/ lckfb@192.168.0.156:/home/lckfb/taishan-hub/
ssh lckfb@192.168.0.156 'sudo -n bash /home/lckfb/taishan-hub/install-broker.sh'
ssh lckfb@192.168.0.156 'sudo -n bash /home/lckfb/taishan-hub/install-agent.sh'
```

两个脚本都**可反复执行**：凭据已存在就不覆盖。

---

## 3. 账号与 ACL（实测通过）

| 用户名 | 权限 | 验证方式 |
|---|---|---|
| `taishan-agent` | 只能写 `hub/v1/devices/taishan-3m/{status,metrics}` | 写自己主题成功；写 `esp32-terminal/status` 返回 `Not authorized` 且消息未投递 |
| `esp32-terminal` | 读 `hub/v1/devices/#`、`hub/v1/events`；写 `hub/v1/commands/results/#` 和自己的 status/metrics；**不可读 `$SYS`** | 订阅全部设备成功；写别人 metrics 被拒 |
| `ops` | 读写全部 + 读 `$SYS/#` | `mosquitto_sub -t '$SYS/broker/version'` 成功 |
| 匿名 | 拒绝 | `Connection error: Not authorized` |

**重要发现（写固件时要用）：** Mosquitto 在 MQTT 3.1.1 下对越权发布是**静默丢弃**，
客户端完全看不出异常；只有 **MQTT 5** 才会回 `Not authorized`。ESP32 用 ESP-IDF 的
`esp-mqtt` 时若要排查 ACL，应选 MQTT 5 协议版本，否则"发成功但没人收到"会很难查。

> 凭据查看：`sudo cat /etc/taishan-hub/credentials.env`
> 后续 ESP32 固件需要其中的 `ESP32_USER` / `ESP32_PASS`，只写进 NVS，不进仓库。

### 踩过的权限坑

`mosquitto_passwd` 要求文件 **owner/group 都是 root**，而 `mosquitto` 守护进程要求
**owner/group 都是 mosquitto**，两者互相打架，2.0.18 之后会直接拒绝加载。
解法：先在 `root:root` 的临时文件里生成，再 `install -o mosquitto -g mosquitto` 换上。
`/etc/taishan-hub` 目录本身必须 755，否则两个不同属主总有一个读不到。

---

## 4. 消息实测数据

`protocol/topics-v1.md` 规定的节奏**已被实测验证**：

```
[metrics] 20 s 内收到 10 条，间隔 = [2.00, 2.08, 2.02, 2.03, 2.05, 2.02, 2.06, 2.02, 2.03]
[status]  20 s 内收到  5 条，间隔 = [4.00, 5.11, 5.06, 5.11]
[status]  state=online  clock_synced=True  ips=['192.168.0.156']  expired=False
[metrics] cpu 2.3% | mem 27.5% | soc 41.6 °C | disk 27.2% | mosquitto=active
```

真实 metrics 样例：

```json
{
  "cpu": {"usage_percent": 3.5, "load1": 0.37, "load5": 0.27, "load15": 0.19, "cores": 8},
  "memory": {"total_bytes": 4086906880, "used_bytes": 1160060928,
             "used_percent": 28.4, "swap_used_percent": null},
  "temperature": {"soc_celsius": 42.5, "max_celsius": 42.5,
                  "zones": {"soc-thermal": 42.5, "bigcore-thermal": 41.6,
                            "little-core-thermal": 41.6, "ddr-thermal": 41.6,
                            "npu-thermal": 41.6, "gpu-thermal": 41.6}},
  "disk": {"path": "/", "total_bytes": 59501207552,
           "used_bytes": 16182054912, "used_percent": 27.2},
  "services": {"mosquitto": "active"},
  "fan": {"pwm": 100},
  "uptime_seconds": 368576
}
```

`swap_used_percent` 为 `null` 是因为本机没启用 swap —— 按协议要求**不拿 0 冒充**。

---

## 5. 在线/离线行为（实测）

| 场景 | 结果 |
|---|---|
| 正常在线 | `status` retained，每 5 s 刷新，`valid_for_seconds=15` |
| `systemctl stop` | Agent 主动发一条 `state=offline`，retained 立刻更新（2 s 内） |
| `kill -9` | Broker 通过 LWT 代发 offline，**2 s 内**可见（keepalive=30） |
| `systemctl start` | 4 s 内回到 `online` |
| 断网/崩溃后重启 | `Restart=always` 自动拉起 |

> LWT 只在**非正常断开**时触发。systemd 的 stop/restart 属于正常断开，
> Broker 不会补发 offline —— 所以 Agent 在收到 SIGTERM 时会自己发一条 offline。
> 这个细节不做的话，retained 状态会一直停在 `online`，只有等有效期过了才失效。

---

## 6. 验收命令（随时可复跑）

```bash
# 在 WSL 或 Windows（已装 paho-mqtt + 有 WSL 的 SSH 密钥）
cd ~/esp32_workspace/"Smart terminal"
python services/taishan-hub/verify/verify_from_wsl.py
```

期望输出结尾：`结果： PASS`。

手工单条订阅（在板上）：

```bash
sudo -n bash -c '. /etc/taishan-hub/credentials.env
mosquitto_sub -V mqttv5 -h 127.0.0.1 -u "$ESP32_USER" -P "$ESP32_PASS" \
  -t "hub/v1/devices/taishan-3m/metrics" -C 1'
```

---

## 7. 运维速查

| 需求 | 命令 |
|---|---|
| Agent 日志 | `journalctl -u taishan-hub-agent -f` |
| Agent 重启 | `sudo systemctl restart taishan-hub-agent` |
| Broker 日志 | `sudo tail -f /var/log/mosquitto/mosquitto.log` |
| 改监控服务清单 | 编辑 `/etc/taishan-hub/agent.env` 的 `HUB_SERVICES`，重启 Agent |
| 改发布节奏 | `agent.env` 目前只暴露设备/磁盘/服务；节奏常量在 `agent/config.py`（S2 前会挪到 env） |
| broker 自身统计 | 用 `ops` 账号订 `$SYS/broker/#` |

---

## 8. 已知未做（留给 S2/S3）

- **TLS / 8883**：目前是 LAN 内明文 1883。方案 §10.3 允许 LAN 明文，
  但上公网前必须换 TLS（证书 + 客户端校验）。
- **静态 IP / DHCP 保留**：泰山派目前是 DHCP 的 `192.168.0.156`，路由器重启后有变的风险。
  ESP32 固件写死 IP 前，建议先在路由器做地址保留。
- **Agent 发布节奏可调**：现在写在 `config.py` 里，`agent.env` 只覆盖设备/磁盘/服务。
- **`$SYS` 指标入库**：暂时只在调试时人工看。
- **无 healthcheck 端点**：systemd 靠进程存活判断；`Restart=always` 已覆盖崩溃场景。
