# ESP32 网线 OTA 使用说明

> 更新日期：2026-11-21
> 当前版本：`1.0.1-ota-20261121`
> 适用固件：ESP32-P4-Function-EV-Board V1.5.2，ESP-IDF v5.4.4

## 1. 架构

```text
WSL Ubuntu-24.04
  └── idf.py build
        └── tools/ota_publish.sh
              ├── 计算 SHA256 / size
              ├── 上传 esp32_terminal-<version>.bin
              └── 原子替换 version.json
                    ↓ scp
泰山派 192.168.0.156
  └── /home/lckfb/esp32-ota
        └── python3 -m http.server 8080（systemd: esp32-ota.service）
              ↓ HTTP
ESP32 192.168.0.100
  ├── 网页 http://192.168.0.100/
  └── 拉取 http://192.168.0.156:8080/version.json
```

Windows 只做 USB 烧录与串口查看；日常 OTA 不增加 Windows 常驻服务。WSL 编译，泰山派提供只读固件源。

## 2. 分区表

```csv
nvs,        data, nvs,     0xa000,   0x6000,
phy_init,   data, phy,     0x10000,  0x1000,
otadata,    data, ota,     0x11000,  0x2000,
ota_0,      app,  ota_0,   0x20000,  4M,
ota_1,      app,  ota_1,   0x420000, 4M,
```

- `nvs` 与 `phy_init` 偏移保持不变，已有 MQTT 配置不丢。
- 当前固件约 1.65 MB，单槽 4 MB，剩余约 61%。
- 首次从单 `factory` 迁移必须 USB 同时烧 `bootloader`、`partition-table`、`ota_data_initial`、`app`；只烧 app 会因为分区表不匹配而无法启动。

## 3. 首次迁移烧录（已完成一次）

```powershell
$OTA_STAGE = "C:\Users\haha4\esp32-flash-tmp\ota-20261121"

python -m esptool --chip esp32p4 -p COM5 -b 460800 `
  --before default-reset --after hard-reset write-flash `
  --flash-mode dio --flash-size 16MB --flash-freq 80m `
  0x2000  "$OTA_STAGE\bootloader.bin" `
  0x9000  "$OTA_STAGE\partition-table.bin" `
  0x11000 "$OTA_STAGE\ota_data_initial.bin" `
  0x20000 "$OTA_STAGE\esp32_terminal.bin"
```

可回滚的旧文件：

- `C:\Users\haha4\esp32-flash-tmp\bootloader-pre-ota-20261121.bin`
- `C:\Users\haha4\esp32-flash-tmp\partition-table-pre-ota-20261121.bin`
- `C:\Users\haha4\esp32-flash-tmp\esp32_terminal-pre-ota-20261121.bin`

## 4. 日常发布

在 WSL 项目目录：

```bash
cd "/home/songshu/esp32_workspace/Smart terminal/firmware/esp32-terminal"

# 用当前 version.txt 编译并发布
./tools/ota_publish.sh

# 改版本号、编译并发布（会先备份 version.txt）
./tools/ota_publish.sh 1.0.2-ota-20261121

# 不编译，只发布已有 build/esp32_terminal.bin（build 被清理后要先编译）
./tools/ota_publish.sh "" --no-build
```

脚本行为：

1. 把版本号写入 `version.txt`（如传了参数）；
2. `idf.py build`；
3. 计算 `sha256` 与 `size`；
4. 先 `scp` 带版本号的 bin；
5. 最后 `scp version.json.tmp`，远端 `mv` 成 `version.json`，保证 manifest 不会指向半截文件。

## 5. ESP32 网页

```text
http://192.168.0.100/
```

接口：

| 方法 | 路径 | 作用 |
|---|---|---|
| GET | `/` | OTA 控制页 |
| GET | `/api/status` | 当前版本、可用版本、状态、进度、错误 |
| POST | `/api/check` | 异步检查 `version.json` |
| POST | `/api/update` | 异步下载并升级 |

页面无鉴权，仅依赖局域网隔离；不要把 80 端口映射到公网。

## 6. 升级与回滚

升级流程：

1. 读取 `version.json`，比较 `version` 与当前 app 版本；
2. 流式下载到另一个 OTA 槽，同时计算 SHA256；
3. 校验 size、SHA256、`esp_ota_end`；
4. `esp_ota_set_boot_partition` 切槽并重启；
5. 新固件首启为 `PENDING_VERIFY`；
6. MQTT 收到首帧后调用 `esp_ota_mark_app_valid_cancel_rollback()` 标记 valid；
7. 120 秒内没有 MQTT 首帧，则 `esp_ota_mark_app_invalid_rollback_and_reboot()` 回滚。

已验证：

- `1.0.0-ota-20261121` → `1.0.1-ota-20261121` OTA 成功；
- 重启后运行在 `ota_1`（`0x420000`）；
- 页面状态变为 `idle`，提示“新固件已通过 MQTT 首帧确认”；
- 三台设备 MQTT 首帧正常。

未实测：

- 负向回滚（故意发布无法连 MQTT 的固件，等 120 秒验证回滚）尚未执行；
- 断网/断电中途升级的恢复场景尚未压测。

## 7. 泰山派服务

```bash
ssh lckfb@192.168.0.156
systemctl status esp32-ota.service
ss -ltnp | grep :8080
ls -lh /home/lckfb/esp32-ota
```

服务单元：`services/taishan-hub/esp32-ota.service`
监听：`0.0.0.0:8080`
目录：`/home/lckfb/esp32-ota`
