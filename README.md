# ESP32-P4 智能终端

ESP32-P4 + 7 寸屏的**独立**设备状态终端：不依赖电脑开机，通过局域网 MQTT 从泰山派本地 Hub
汇总多台设备的状态，一屏看多台，可按键切换查看明细。终端只做**查看**，不下发控制命令。

- 固件版本：`1.0.1-ota-20261121`（app 1,645,056 B；双 OTA 槽各 4 MB，余约 61%）
- OTA：网页 `http://192.168.0.100/`，从泰山派 `http://192.168.0.156:8080/version.json` 拉取
- 已接入设备：`taishan-3m`（泰山派 3M · RK3576 / Ubuntu 24.04）、`windows-main`（Windows 11）、`cloud-main`（云服务器 · Tailscale `100.66.69.21`）
- Hub：泰山派 `192.168.0.156:1883`（Mosquitto 2.0.18）
- 终端：ESP32-P4 rev v1.3，IP `192.168.0.100`，MAC `30:ed:a0:e2:23:6f`

## 目录

```text
Smart terminal/
├── README.md                  ← 本文件
├── firmware/esp32-terminal/   ← 固件工程（ESP-IDF，main/ 是全部业务代码）
├── agents/windows/            ← Windows 指标 Agent（Python）
├── services/taishan-hub/      ← 板端 Hub + 指标 Agent 的安装脚本与源码
├── protocol/topics-v1.md      ← MQTT 协议（权威，写固件前必读）
├── docs/
│   ├── 设计方案-V3.1.md       ← 定位、拓扑、协议、架构决策
│   ├── 部署与运维.md          ← Hub / Agent 部署、烧录、诊断、已知坑
│   └── OTA网线升级.md         ← 双槽 OTA、发布、回滚
└── .gitignore                 # 排除 build/、managed_components/、secrets、缓存、*.bak-*、__pycache__
```

## 环境

| 项目 | 值 |
|---|---|
| 编译 | WSL2 Ubuntu 24.04，ESP-IDF **v5.4.4**（`~/esp-idf`） |
| 工程 | `~/esp32_workspace/Smart terminal/firmware/esp32-terminal` |
| 烧录 | Windows 侧，串口 **COM5**，esptool |
| 板卡 | ESP32-P4-Function-EV-Board V1.5.2，16 MB Flash，屏幕 1024×600（EK79007 + GT911） |

## 常用命令

```bash
# 1) 编译（改过中文文案要先重生成字体）
cd ~/esp32_workspace/"Smart terminal"/firmware/esp32-terminal
bash tools/gen_cjk_font.sh     # 仅当 main/ 里新增/修改了中文字符串
bash build-s2a.sh              # 会自动叠加 sdkconfig.secrets 并校验密码
```

```bash
# 2) 把产物复制到 Windows 暂存目录（避免 esptool 读 \\wsl.localhost 失败）
B=build; D=/mnt/c/Users/haha4/esp32-usb-stage
mkdir -p "$D"
cp $B/bootloader/bootloader.bin $B/partition_table/partition-table.bin $B/ota_data_initial.bin $B/esp32_terminal.bin "$D"/
```

```powershell
# 3) 首次迁移/救砖（四件套，单行，别用续行符）
python -m esptool --chip esp32p4 -p COM5 -b 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB 0x2000 C:\Users\haha4\esp32-usb-stage\bootloader.bin 0x9000 C:\Users\haha4\esp32-usb-stage\partition-table.bin 0x11000 C:\Users\haha4\esp32-usb-stage\ota_data_initial.bin 0x20000 C:\Users\haha4\esp32-usb-stage\esp32_terminal.bin
```

日常升级优先用第 4 条 OTA；USB 四件套只在首次迁移、救砖或改分区表时用。

```bash
# 4) OTA 发布：WSL 编译 → 泰山派 HTTP 8080 → ESP32 网页拉取
cd ~/esp32_workspace/"Smart terminal"/firmware/esp32-terminal
./tools/ota_publish.sh                    # 用当前 version.txt
./tools/ota_publish.sh 1.0.2-ota-20261121 # 改版本号并发布
```

日常 OTA 不需要插 USB；只有改分区表/bootloader 时才需要重新 USB 烧录。

## 两条硬约定

1. **中文必须用 `main/fonts/lv_font_cjk_16/20/28`**，不能用 Montserrat；改中文文案后必须重跑
   `tools/gen_cjk_font.sh`，否则新字显示成方块。
2. **`sdkconfig.secrets`（含真实 MQTT 密码）绝不入库**；`build/`、`managed_components/`、
   `tools/.cache/` 也都在 `.gitignore` 里。

部署、烧录、排障见 `docs/部署与运维.md`；OTA 见 `docs/OTA网线升级.md`。
