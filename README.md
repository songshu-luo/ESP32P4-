# ESP32-P4 智能终端

ESP32-P4 + 7 寸屏的**独立**设备状态终端：不依赖电脑开机，通过局域网 MQTT 从泰山派本地 Hub
汇总多台设备的状态，一屏看多台，可按键切换查看明细。终端只做**查看**，不下发控制命令。

- 固件版本：`0.5.5-s2e`（app 约 1.53 MB，app 分区 4 MB，余 63%）
- 已接入设备：`taishan-3m`（泰山派 3M · RK3576 / Ubuntu 24.04）、`windows-main`（Windows 11）
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
│   ├── 项目交接.md            ← 现状、结构、坑、下一步
│   ├── 部署与运维.md          ← Hub / Agent 部署、烧录、诊断命令
│   └── 设计方案-V3.1.md       ← 精简版方案（只读监控口径）
└── .gitignore                 # 排除 build/、managed_components/、sdkconfig.secrets、tools/.cache
```

## 环境

| 项目 | 值 |
|---|---|
| 编译 | WSL2 Ubuntu 24.04，ESP-IDF **v5.4.4**（`~/esp-idf`） |
| 工程 | `~/esp32_workspace/Smart terminal/firmware/esp32-terminal` |
| 烧录 | Windows 侧，串口 **COM5**，esptool |
| 板卡 | ESP32-P4-Function-EV-Board V1.5.2，16 MB Flash，屏幕 1024×600（EK79007 + GT911） |

## 三个常用命令

```bash
# 1) 编译（改过中文文案要先重生成字体）
cd ~/esp32_workspace/"Smart terminal"/firmware/esp32-terminal
bash tools/gen_cjk_font.sh     # 仅当 main/ 里新增/修改了中文字符串
bash build-s2a.sh              # 会自动叠加 sdkconfig.secrets 并校验密码
```

```bash
# 2) 把产物复制到 Windows（避免 esptool 读 \\wsl.localhost 失败）
B=build; D=/mnt/c/Users/haha4/p4-s0
cp $B/bootloader/bootloader.bin $B/partition_table/partition-table.bin $B/esp32_terminal.bin $D/
```

```powershell
# 3) 烧录（单行，别用续行符）
python -m esptool --chip esp32p4 -p COM5 -b 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB 0x2000 C:\Users\haha4\p4-s0\bootloader.bin 0x9000 C:\Users\haha4\p4-s0\partition-table.bin 0x20000 C:\Users\haha4\p4-s0\esp32_terminal.bin
```

## 两条硬约定

1. **中文必须用 `main/fonts/lv_font_cjk_16/20/28`**，不能用 Montserrat；改中文文案后必须重跑
   `tools/gen_cjk_font.sh`，否则新字显示成方块。
2. **`sdkconfig.secrets`（含真实 MQTT 密码）绝不入库**；`build/`、`managed_components/`、
   `tools/.cache/` 也都在 `.gitignore` 里。

更细的现状与坑见 `docs/项目交接.md`；部署、烧录、排障命令见 `docs/部署与运维.md`。
