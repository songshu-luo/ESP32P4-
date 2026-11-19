# S2-a ESP32 接入 MQTT —— 连接状态可见

> 日期：2026-11-19
> 阶段：S2-a（ESP32 ← MQTT ← 泰山派 Hub）
> 状态：**已实机验收**（2026-11-19）：屏幕 MQTT 卡显示正常，连接成功，
> 计数每 2 s +1（串口日志未截取，但行为已符合预期）。
> 本阶段**不解析业务载荷**，只验证「能不能连上并订阅成功」。

---

## 1. 本阶段做了什么

| 改动 | 文件 |
|---|---|
| 新增 MQTT 链路模块（后台任务 + 状态机 + 互斥快照）| `main/mqtt_link.h`、`main/mqtt_link.c` |
| 底部卡片拆成左右两块：NETWORK / MQTT BROKER | `main/app_ui.c` |
| 启动时并行拉起以太网和 MQTT（都不阻塞 UI）| `main/main.c` |
| Kconfig 暴露 broker 地址/端口/账号 | `main/Kconfig.projbuild` |
| 打开 MQTT 5 并固化地址 | `sdkconfig.defaults` |
| 可重复执行的构建脚本（叠加本地密码文件）| `build-s2a.sh` |

版本号：`0.2.0-s1` → **`0.3.0-s2a`**，顶栏右侧同步显示 `S2-a mqtt link`。

---

## 2. 屏幕上应该看到什么

底部两张卡片，各 468×140：

```
┌─ NETWORK ──────────────────┐   ┌─ MQTT BROKER ──────────────┐
│ ONLINE                     │   │ SUBSCRIBED                 │
│ 192.168.0.100              │   │ 37 msg                     │
│ 100M full duplex  gw …     │   │ squirrel-lckfb.local  192… │
└────────────────────────────┘   └────────────────────────────┘
```

MQTT 卡三行的含义：

| 行 | 内容 |
|---|---|
| 第 1 行 | 状态词：`IDLE` / `NO BROKER` / `RESOLVING` / `CONNECTING` / `SUBSCRIBING` / `SUBSCRIBED` / `DISCONNECTED` / `AUTH FAIL` / `MQTT ERROR` |
| 第 2 行 | 已收到的消息条数（只计数，不解析）|
| 第 3 行 | 正常时显示「主机名 + 解析出的 IP」；异常时显示具体原因 |

状态词与颜色的对应：

| 状态 | 颜色 | 意味着 |
|---|---|---|
| `SUBSCRIBED` | 绿 | 连上并订阅成功，链路可用 |
| `CONNECTING` / `SUBSCRIBING` / `RESOLVING` | 黄 | 正在推进 |
| `DISCONNECTED` | 黄 | 连上过又断了，正在退避重连 |
| `AUTH FAIL` | 红 | Broker 明确拒绝账号密码 |
| `NO BROKER` | 红 | 三个候选地址都解析不到 |
| `MQTT ERROR` | 红 | TCP 不通 / 订阅失败等，看串口日志 |

> 第 2 行数字应该**不断变大**（每 2 秒一条 metrics、每 5 秒一条 status）。
> 数字不动但状态是绿 = 连上了但没收到消息，属于异常。

---

## 3. 连接策略（为什么这样写）

### 3.1 地址解析：三级回退

泰山派没做 DHCP 地址保留，所以**每次重连都重新解析**，按顺序试：

1. `squirrel-lckfb.local` —— mDNS（用户选定）
2. `squirrel-lckfb` —— 路由器 DHCP 注册的裸主机名
3. `192.168.0.156` —— 写死 IP 兜底

**实测结论**：IDF 5.4.4 的 lwIP 已经带一次性 mDNS 查询
（`CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES=y`），对 `.local` 结尾的名字会直接向
`224.0.0.251:5353` 发组播查询。用 Python 复现同样的报文（临时源端口、无 QU 位）
从本 LAN 发出去，泰山派的 avahi **3 秒内以单播回了正确的 A 记录**：

```
query:  squirrel-lckfb.local  (src 192.168.0.122:49305)
reply:  192.168.0.156:5353  flags=0x8400  AN=1  A=192.168.0.156
```

所以**不需要引入 `espressif/mdns` 组件**，`getaddrinfo()` 直接用。

> 注意：Windows 上 `nslookup squirrel-lckfb.local` 会失败（Windows 的 DNS 客户端
> 不查 mDNS），但 `ping squirrel-lckfb.local` 能通——别被 nslookup 误导。

### 3.2 关掉 esp-mqtt 的自动重连

`disable_auto_reconnect = true`，重连由 `mqtt_link.c` 自己做。理由：
esp-mqtt 自己重连时会一直用**同一个已解析的 IP**，DHCP 换地址后永远连不上，
而且看不出原因。自管重连才能在每次连接前重新解析。

退避：3 s 起，翻倍，封顶 30 s；正常掉线后回到 3 s。

### 3.3 用 MQTT 5

Mosquitto 在 3.1.1 下对越权发布**静默丢弃**，只有 5.0 会回 `Not authorized`。
虽然 S2-a 只订阅，但同样的道理适用于鉴权失败——3.1.1 的连接拒绝信息也更粗。

---

## 4. 密码怎么进固件（重要）

密码**不进仓库**。做法：

- `sdkconfig.defaults`（入库）里 `CONFIG_TERMINAL_MQTT_PASSWORD=""`
- `sdkconfig.secrets`（**不入库**，`chmod 600`）里放真实密码
- 构建时叠加：`SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.secrets"`

`build-s2a.sh` 封装了这个流程，并且在编译结束后检查 `sdkconfig` 里的密码
是不是空的（空就当构建失败）。

**踩过的坑**：kconfig 的 `string` 项值**必须带双引号**，写成 `KEY=value` 会被当成
非法输入而静默回退成默认值——第一次构建就是密码悄悄变成空串，bin 照常生成、
照常烧录，只是连不上。`build-s2a.sh` 末端的检查就是为了堵这个坑。

生成 `sdkconfig.secrets`（一次即可）：

```bash
cd ~/esp32_workspace/"Smart terminal"/firmware/esp32-terminal
ssh lckfb@192.168.0.156 'sudo -n grep ^ESP32_PASS= /etc/taishan-hub/credentials.env' \
  | sed 's/^ESP32_PASS=/CONFIG_TERMINAL_MQTT_PASSWORD=/' \
  | sed -E 's/=(.*)$/="\1"/' > sdkconfig.secrets
chmod 600 sdkconfig.secrets
```

---

## 5. 编译

```bash
cd ~/esp32_workspace/"Smart terminal"/firmware/esp32-terminal
bash build-s2a.sh
```

期望最后两行：

```
esp32_terminal.bin binary size 0x14bef0 bytes. ...
[build] MQTT password baked in: yes
```

脚本只在 `sdkconfig.defaults` / `sdkconfig.secrets` 更新时才重建 `sdkconfig`，
平时开机编译不会触发全量重编。

---

## 6. 验收步骤

烧录后看屏幕 + 串口日志，逐条对：

| # | 期望 | 说明 |
|---|---|---|
| 1 | 开机后几秒内 MQTT 卡变 `SUBSCRIBED` 绿色 | 正常情况 3–8 s |
| 2 | `n msg` 数字持续增长 | 每 2 s 一条 metrics，20 s 应涨 10 左右 |
| 3 | 第 3 行显示 `squirrel-lckfb.local  192.168.0.156` | 说明走的是 mDNS 名 |
| 4 | 串口出现 `resolved squirrel-lckfb.local -> 192.168.0.156` | |
| 5 | 串口出现 `connected, protocol v5` 和 `subscribed to hub/v1/devices/#` | |
| 6 | 拔网线 → NETWORK 变 `LINK DOWN`，MQTT 变 `DISCONNECTED` | |
| 7 | 插回网线 → 十几秒内两者都恢复绿色 | 验证重连和重新解析 |

**故意制造失败，确认诊断文字正确**（可选，但建议做一次）：

| 做法 | 期望 |
|---|---|
| 泰山派上 `sudo systemctl stop mosquitto` | MQTT 卡变 `MQTT ERROR`（`tcp connect failed`），每 3–30 s 重试 |
| 上面再 `sudo systemctl start mosquitto` | 一个退避周期内恢复 `SUBSCRIBED` |
| 临时把 `sdkconfig.secrets` 密码改成错的再烧 | `AUTH FAIL`（红色） |
| 泰山派关机 | MQTT 卡 `MQTT ERROR`，NETWORK 卡仍 `ONLINE` |

---

## 7. 已知未做（S2-b 起）

- **不解析任何载荷**：`msg_count` 只计数，不读 JSON。
- **不发 `esp32-terminal/status`**：按协议 ESP32 也该上报自己的在线状态，留给后续。
- **无 LWT**：本阶段没有会话来清理的 retained 状态。
- **TLS**：仍是 LAN 内明文 1883，上公网前必须换 8883。
- **`sdkconfig.secrets` 没有 .gitignore 保护**（项目还不是 git 仓库）——
  建仓库时必须先把这条加进去。
