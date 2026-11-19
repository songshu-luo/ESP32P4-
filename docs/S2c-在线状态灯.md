# S2-c 在线状态灯（泰山派在线 / 离线 / 未知）

状态：**已实机验收通过**（2026-11-19）

## 1. 这一步做了什么

屏幕在指标卡上方左侧多了一颗**圆点灯 + 文字**，显示泰山派 3M 的在线状态：

```
● 在线   泰山派 3M   squirrel-lckfb                 更新于 0.4 秒前   第 37 帧
```

同时 MQTT 卡片的第二行从「1234 条」改成**按主题拆开**的计数：

```
MQTT 服务器
已连接
指标 1234   状态 412
squirrel-lckfb.local  192.168.0.156
```

## 2. 三态是怎么判定的（协议 §3 的实现）

优先级从高到低，**先判定上层条件**：

| 序 | 条件 | 显示 | 颜色 |
|---|---|---|---|
| 1 | MQTT 没连上（不在 `MQTT_LINK_ONLINE`） | 未知 | 灰 |
| 2 | 收到 `status` 且 `state=online`、未超时 | 在线 | 绿 |
| 3 | 收到 `status` 且 `state=offline`（含 LWT） | 离线 | 红 |
| 4 | 曾经 online 但超过 `valid_for_seconds`（15 s） | 过期 | 黄 |
| 5 | 从没收到过 | 未知 | 灰 |

**第 1 条是重点**。broker 挂掉后，`status` 是 retained 主题，最后一条 `online` 还留在
broker 上；如果只看消息内容，屏幕会一直绿着。所以必须先看 MQTT 链路本身，
链路断了就一律显示「未知」，不沿用缓存——这是协议里唯一一条关于「不要相信缓存」
的硬性要求。

第 4 条单独区分「过期」：说明 Hub 链路通、但设备不再发心跳了，比「未知」信息量大。

## 3. 代码结构

新增两个文件（与 `device_metrics.c` 同一套模式）：

- `main/device_status.h` / `.c`：解析 `hub/v1/devices/taishan-3m/status`，
  对外只给一份互斥保护的只读快照 `device_status_get()`。
  主题过滤、`schema_version` 校验、`payload.state` 必填校验都在里面；
  `state` 字符串不认识时**不猜**，显示未知。
- `main/app_ui.c` 新增 `status_poll_cb()`：500 ms 轮询一次 mqtt 链路状态 +
  device_status 快照，按上表合成三态。

`mqtt_link` 的改动：

- `mqtt_link_set_data_cb()` → `mqtt_link_add_data_cb()`，最多 4 个回调，
  各模块自己按主题过滤，`mqtt_link` 不做分发策略。
- 状态快照新增 `metrics_count` / `status_count`，用**主题后缀**计数
  （`/metrics`、`/status`）。只看总数的话，metrics 停了但 status 还在来，
  屏幕上是看不出区别的。

新鲜度判定和 metrics 一样：ESP32 没有 RTC，用**到达时刻 + `valid_for_seconds`**，
不去和 `timestamp_utc` 比。

## 4. 验收清单

正常情况：

1. 开机约 2–5 s 后灯变**绿**、文字「在线」。
2. MQTT 卡片显示「指标 N   状态 M」，**两个数都在涨**（指标 2 s 一条，状态 5 s 一条）。
3. 右上角帧数继续增长。

故意测「离线」（在泰山派上执行）：

```bash
sudo systemctl stop taishan-hub-agent
```

- LWT 应在 ~2 s 内被 broker 代发，灯变**红**、文字「离线」。
- 注意：指标卡此时不会立刻变灰（status 的 LWT 只影响在线灯，metrics 要等 8 s 有效期）。
  两件事分开是刻意的——设备离线 ≠ 指标过期，虽然通常会先后发生。

恢复：

```bash
sudo systemctl start taishan-hub-agent
```

- 状态灯几秒内回绿。

故意测「未知」（拔掉板子的网线）：

- 灯在 MQTT 断开后变**灰**、「未知」，**不是红色**。
  因为从 ESP32 视角这是「Hub 不可达」，不是「设备明确说自己在离线」。
- 再插回网线，几秒后自动恢复绿。

## 5. 已知未做

- **状态灯没有单独的独立区域**：它和指标区共用一行。S2-d 做页面骨架时，
  会挪到顶部常驻状态栏。
- **`clock_synced=false` 的提示没上屏**：协议 §4.1 要求订阅方提示
  「该设备时间未同步，过期判断不可信」。已解析并存进快照
  （`has_clock_flag` / `clock_synced`），UI 暂未使用——等 S2-d 状态栏一起做。
- **多设备**：目前只订阅 `taishan-3m` 一个设备的状态。S3 引入第二台设备时，
  `device_status` 需要改成按 device_id 索引的表。
- **`uptime_seconds` 已解析未上屏**：留给监控页。

## 5.1 收尾时删掉的文字（0.5.1-s2c）

为了腾出 flash，也为了屏幕更安静，这一版**删掉了**：

| 位置 | 删掉的内容 |
|---|---|
| 标题下 | `芯片 rev v1.3   核心 2   屏幕 1024x600` |
| 网络卡 | `100M 全双工`、网关地址（在线时只显示「在线」+ IP）|
| 页脚 | `LVGL 9   数据来源 hub/v1/devices/taishan-3m/metrics` |

需要这些信息时看串口日志（`log_board_info()` 仍会打印芯片 rev / Flash / PSRAM / LCD）。

## 6. 固件体积提醒

加入中文字体（S2-b-zh）后 app 分区余量一度只剩 **6%**（89 KB）。
随后删掉了芯片型号/rev/核心数/屏幕分辨率行、页脚（LVGL 版本 + 数据来源），
以及网速/双工/网关那行，并重生成字体（汉字 100 个 / 字形 195 个），
现状 **0x15e430 已用，0x18bd0（7%，101 KB）剩余**。

S2-d 页面骨架、S3 控制页之前仍需做一次体积盘点，可选手段：
字体降到 bpp 2、只生成 16/20 两个字号、裁掉 BSP 连带的无用组件
（`esp_video`、`esp_codec_dev`），或改用自定义分区表放大 app
（flash 有 16 MB，目前只用了 1.5 MB）。
