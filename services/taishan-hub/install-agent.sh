#!/usr/bin/env bash
# 泰山派本地 Hub —— 指标 Agent 安装脚本（幂等）
#
# 前置：install-broker.sh 已成功执行（需要 /etc/taishan-hub/credentials.env）
#
# 用法（在板子上，工程目录 services/taishan-hub/ 下）：
#   sudo bash install-agent.sh
#
# 做什么：
#   1. 装 python3-paho-mqtt（只此一个运行时依赖）
#   2. 把 agent/ 装到 /opt/taishan-hub/agent
#   3. 写 /etc/taishan-hub/agent.env
#   4. 装 systemd 单元并 enable --now
#   5. 等 10 s，验证 status/metrics 都有真实数据

set -euo pipefail

HUB_DIR=/etc/taishan-hub
INSTALL_DIR=/opt/taishan-hub/agent
UNIT_NAME=taishan-hub-agent.service
CONF_SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

msg() { printf '\n\033[1;36m== %s ==\033[0m\n' "$*"; }
die() { printf '\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "请用 sudo 运行"
[ -f "$HUB_DIR/credentials.env" ] || die "缺少 $HUB_DIR/credentials.env，请先跑 install-broker.sh"

msg "1/5 安装运行时依赖"
if python3 -c 'import paho.mqtt.client' 2>/dev/null; then
  echo "paho-mqtt 已存在"
else
  DEBIAN_FRONTEND=noninteractive apt-get install -y -q python3-paho-mqtt 2>&1 | tail -3
fi
python3 -c 'import paho.mqtt; print("paho-mqtt", paho.mqtt.__version__)'

msg "2/5 安装 agent 代码到 $INSTALL_DIR"
install -d -m 755 -o root -g root "$INSTALL_DIR"
for f in "$CONF_SRC"/agent/*.py; do
  install -m 644 -o root -g root "$f" "$INSTALL_DIR/$(basename "$f")"
done
ls -l "$INSTALL_DIR"

msg "3/5 写 agent.env（已存在则保留）"
if [ ! -f "$HUB_DIR/agent.env" ]; then
  install -m 640 -o root -g lckfb "$CONF_SRC/agent/agent.env" "$HUB_DIR/agent.env"
  echo "已写入默认配置"
else
  echo "沿用已有 $HUB_DIR/agent.env"
fi

msg "4/5 安装并启动 systemd 单元"
install -m 644 -o root -g root "$CONF_SRC/$UNIT_NAME" "/etc/systemd/system/$UNIT_NAME"
systemctl daemon-reload
systemctl enable --quiet "$UNIT_NAME"
systemctl restart "$UNIT_NAME"
sleep 3
systemctl is-active --quiet "$UNIT_NAME" || {
  journalctl -u "$UNIT_NAME" -n 40 --no-pager || true
  die "agent 未能启动"
}
echo "agent 运行中"

msg "5/5 等 10 s 后验证消息"
set -a; . "$HUB_DIR/credentials.env"; set +a
H=127.0.0.1

echo "--- status（retained，连上就能拿到） ---"
status=$(timeout 6 mosquitto_sub -V mqttv5 -h "$H" -u "$ESP32_USER" -P "$ESP32_PASS" \
          -t hub/v1/devices/taishan-3m/status -C 1 2>&1) || true
echo "$status"
echo "$status" | grep -q '"state": "online"' || die "status 不是 online，检查 journalctl -u $UNIT_NAME"

echo "--- metrics（实时，等一条最新） ---"
metrics=$(timeout 10 mosquitto_sub -V mqttv5 -h "$H" -u "$ESP32_USER" -P "$ESP32_PASS" \
           -t hub/v1/devices/taishan-3m/metrics -C 1 2>&1) || true
echo "$metrics"

python3 - "$metrics" <<'PY'
import json, sys
raw = sys.argv[1]
try:
    doc = json.loads(raw)
except json.JSONDecodeError:
    print("[FAIL] metrics 不是合法 JSON")
    sys.exit(1)

p = doc.get("payload", {})
checks = {
    "cpu.usage_percent": p.get("cpu", {}).get("usage_percent"),
    "memory.used_percent": p.get("memory", {}).get("used_percent"),
    "temperature.soc_celsius": p.get("temperature", {}).get("soc_celsius"),
    "disk.used_percent": p.get("disk", {}).get("used_percent"),
}
failed = False
for key, value in checks.items():
    if value is None:
        print(f"[FAIL] {key} 为空")
        failed = True
    else:
        print(f"[OK]   {key} = {value}")
svc = p.get("services", {}).get("mosquitto")
print(f"[{'OK' if svc == 'active' else 'FAIL'}]   services.mosquitto = {svc}")
if svc != "active":
    failed = True
sys.exit(1 if failed else 0)
PY

cat <<EOF

Agent 部署完成。
  状态：  systemctl status $UNIT_NAME
  日志：  journalctl -u $UNIT_NAME -f
  配置：  $HUB_DIR/agent.env（改完 systemctl restart $UNIT_NAME）
EOF
