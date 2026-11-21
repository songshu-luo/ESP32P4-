#!/usr/bin/env bash
# 泰山派本地 Hub —— Mosquitto 安装脚本（幂等，可反复执行）
#
# 用法（在工程根目录的上级执行，或先 rsync 整个 services/ 上来）：
#   sudo bash services/taishan-hub/install-broker.sh
#
# 做什么：
#   1. 建 /etc/taishan-hub（root:mosquitto 750）
#   2. 生成五个账号的随机密码，写入 credentials.env（root 600）
#   3. 写 mosquitto.passwd / mosquitto.acl
#   4. 安装 conf.d/taishan-hub.conf
#   5. 重启并验证：匿名被拒、taishan-agent 可发、esp32-terminal 只读
#
# 不做：端口映射、公网暴露、TLS（LAN 内 1883，方案 §10.3 允许）。

set -euo pipefail

HUB_DIR=/etc/taishan-hub
CRED=$HUB_DIR/credentials.env
CONF_SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

msg() { printf '\n\033[1;36m== %s ==\033[0m\n' "$*"; }
die() { printf '\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "请用 sudo 运行"
command -v mosquitto_passwd >/dev/null || die "缺少 mosquitto_passwd，请先 apt install mosquitto"
id mosquitto >/dev/null 2>&1 || die "缺少 mosquitto 用户"

msg "1/6 准备目录"
# 目录 755：mosquitto 与 lckfb 两个不同属主都要能穿进来读各自的文件。
# 真正的保护在文件级 —— credentials.env 640 root:lckfb，
# mosquitto.passwd / acl 属主 mosquitto 且 600/640。
install -d -m 755 -o root -g root "$HUB_DIR"

msg "2/6 生成凭据（已存在则保留，缺的补上）"
gen() { openssl rand -base64 24 | tr -d '/+=' | cut -c1-20; }
if [ ! -f "$CRED" ]; then
  cat > "$CRED" <<EOF
# 泰山派 Hub 设备凭据 —— 仅本机 root 可读，禁止提交到仓库
MOSQUITTO_HOST=192.168.0.156
MOSQUITTO_PORT=1883
TAISHAN_AGENT_USER=taishan-agent
TAISHAN_AGENT_PASS=$(gen)
ESP32_USER=esp32-terminal
ESP32_PASS=$(gen)
WINDOWS_AGENT_USER=windows-agent
WINDOWS_AGENT_PASS=$(gen)
CLOUD_AGENT_USER=cloud-agent
CLOUD_AGENT_PASS=$(gen)
OPS_USER=ops
OPS_PASS=$(gen)
EOF
  chmod 640 "$CRED"
  chgrp lckfb "$CRED"
  echo "已生成新凭据：$CRED"
else
  echo "沿用已有凭据：$CRED"
  chown root:lckfb "$CRED"; chmod 640 "$CRED"
fi

# 后加的设备凭据：S1 时代生成的 credentials.env 里没有 windows-agent 这一组。
# 缺哪个补哪个，已存在的一律不覆盖——覆盖会把正在用的密码换掉，
# 表现为「agent 突然连不上」，而密码本身并不需要变。
append_cred() {   # $1=变量名  $2=固定值（空串表示随机生成）
  local key=$1 fixed=$2 value
  grep -q "^${key}=" "$CRED" && return 0
  if [ -n "$fixed" ]; then value=$fixed; else value=$(gen); fi
  printf '%s=%s\n' "$key" "$value" >> "$CRED"
  echo "已补充 $key"
}
append_cred WINDOWS_AGENT_USER windows-agent
append_cred WINDOWS_AGENT_PASS ""
append_cred CLOUD_AGENT_USER cloud-agent
append_cred CLOUD_AGENT_PASS ""

# shellcheck disable=SC1090
. "$CRED"

msg "3/6 写 password_file"
# 两个组件的属主要求互相打架：
#   - mosquitto_passwd 要求被改的文件 owner/group 都是 root
#   - mosquitto 守护进程要求 owner/group 都是 mosquitto（2.0 起会拒绝加载）
# 做法：先以 root:root 在临时路径生成，再 install 成 mosquitto:mosquitto 换上。
PW_TMP=$HUB_DIR/.mosquitto.passwd.tmp
install -m 600 -o root -g root /dev/null "$PW_TMP"
mosquitto_passwd -c -b "$PW_TMP" "$TAISHAN_AGENT_USER" "$TAISHAN_AGENT_PASS"
mosquitto_passwd    -b "$PW_TMP" "$ESP32_USER"        "$ESP32_PASS"
mosquitto_passwd    -b "$PW_TMP" "$WINDOWS_AGENT_USER" "$WINDOWS_AGENT_PASS"
mosquitto_passwd    -b "$PW_TMP" "$CLOUD_AGENT_USER"   "$CLOUD_AGENT_PASS"
mosquitto_passwd    -b "$PW_TMP" "$OPS_USER"          "$OPS_PASS"
install -m 600 -o mosquitto -g mosquitto "$PW_TMP" "$HUB_DIR/mosquitto.passwd"
rm -f "$PW_TMP"

msg "4/6 写 ACL 与 listener 配置"
# ACL 的 owner/group 都必须是 mosquitto（2.0 起会校验，否则将来拒绝加载）。
install -m 640 -o mosquitto -g mosquitto "$CONF_SRC/mosquitto/acl"     "$HUB_DIR/mosquitto.acl"
install -m 644 -o root -g root      "$CONF_SRC/mosquitto/mosquitto.conf" /etc/mosquitto/conf.d/taishan-hub.conf

msg "5/6 重启 broker"
# mosquitto 2.0 没有 --test；配置语法错误会直接让服务起不来，所以靠 is-active 把关。
systemctl restart mosquitto
sleep 2
systemctl is-active --quiet mosquitto || {
  journalctl -u mosquitto -n 30 --no-pager || true
  tail -30 /var/log/mosquitto/mosquitto.log 2>/dev/null || true
  die "mosquitto 未能启动"
}
echo "mosquitto 运行中"

msg "6/6 冒烟验证"
HOST=127.0.0.1

# 冒烟验证用临时运维凭据直连 127.0.0.1，避免依赖任何已部署客户端。
# 注意：mosquitto_pub 在 CONNACK 被拒时退出码可能是 0，必须看输出文本。
_AGENT_U=$TAISHAN_AGENT_USER
_AGENT_P=$TAISHAN_AGENT_PASS
_SUB_U=$ESP32_USER
_SUB_P=$ESP32_PASS

# 6a 匿名必须被拒。
# 注意：mosquitto_pub 在 CONNACK 被拒时退出码可能是 0，所以必须看输出文本。
anon_out=$(timeout 5 mosquitto_pub -V mqttv5 -h "$HOST" -p 1883 -t hub/v1/devices/x/status -m '{}' 2>&1 || true)
if echo "$anon_out" | grep -qi 'not authorized'; then
  echo "[OK] 匿名连接被拒绝"
else
  echo "[FAIL] 匿名居然能连 —— 鉴权没生效！输出：$anon_out"
  exit 1
fi

# 6b agent 能写自己的主题（无输出 = 成功）
if out=$(timeout 5 mosquitto_pub -V mqttv5 -h "$HOST" -u "$TAISHAN_AGENT_USER" -P "$TAISHAN_AGENT_PASS" \
     -t hub/v1/devices/taishan-3m/metrics -m '{"probe":1}' 2>&1); [ -n "$out" ]; then
  echo "[FAIL] agent 发布报错：$out"
  exit 1
fi
echo "[OK] taishan-agent 可发布 metrics"

# 6c agent 不能写别人的主题。
# 必须用 MQTT5 才会拿到 "Not authorized" 回执；MQTT3.1.1 下 broker 是静默丢弃，
# 客户端看不出任何异常。协议版本没得选，验收脚本统一用 -V mqttv5。
out=$(timeout 5 mosquitto_pub -V mqttv5 -h "$HOST" -u "$TAISHAN_AGENT_USER" -P "$TAISHAN_AGENT_PASS" \
     -t hub/v1/devices/esp32-terminal/status -m '{"probe":1}' -q 1 2>&1 || true)
if echo "$out" | grep -qi 'not authorized'; then
  echo "[OK] taishan-agent 越权写被拒（ACL 生效）"
else
  echo "[FAIL] 越权写居然成功，ACL 没生效！输出：${out:-<空>}"
  exit 1
fi

# 6d 订阅端能收到 agent 的消息
( timeout 6 mosquitto_sub -V mqttv5 -h "$HOST" -u "$ESP32_USER" -P "$ESP32_PASS" \
    -t 'hub/v1/devices/#' -C 1 > /tmp/_mq_cap.txt 2>/dev/null ) &
SUB=$!
sleep 1
mosquitto_pub -V mqttv5 -h "$HOST" -u "$TAISHAN_AGENT_USER" -P "$TAISHAN_AGENT_PASS" \
  -t hub/v1/devices/taishan-3m/status -m '{"probe":2}'
wait $SUB || true
if grep -q 'probe' /tmp/_mq_cap.txt 2>/dev/null; then
  echo "[OK] esp32-terminal 能订阅到 agent 消息"
else
  echo "[FAIL] 订阅链路不通"
  exit 1
fi
rm -f /tmp/_mq_cap.txt

# 6e windows-agent 只能写自己的主题，不能写 taishan-3m 的。
if out=$(timeout 5 mosquitto_pub -V mqttv5 -h "$HOST" -u "$WINDOWS_AGENT_USER" -P "$WINDOWS_AGENT_PASS" \
     -t hub/v1/devices/windows-main/metrics -m '{"probe":3}' 2>&1); [ -n "$out" ]; then
  echo "[FAIL] windows-agent 发布报错：$out"
  exit 1
fi
echo "[OK] windows-agent 可发布自己的 metrics"

out=$(timeout 5 mosquitto_pub -V mqttv5 -h "$HOST" -u "$WINDOWS_AGENT_USER" -P "$WINDOWS_AGENT_PASS" \
     -t hub/v1/devices/taishan-3m/metrics -m '{"probe":4}' -q 1 2>&1 || true)
if echo "$out" | grep -qi 'not authorized'; then
  echo "[OK] windows-agent 越权写被拒（ACL 生效）"
else
  echo "[FAIL] windows-agent 越权写居然成功，ACL 没生效！输出：${out:-<空>}"
  exit 1
fi

# 6f cloud-agent 只能写 cloud-main 的主题。
if out=$(timeout 5 mosquitto_pub -V mqttv5 -h "$HOST" -u "$CLOUD_AGENT_USER" -P "$CLOUD_AGENT_PASS" \
     -t hub/v1/devices/cloud-main/metrics -m '{"probe":5}' 2>&1); [ -n "$out" ]; then
  echo "[FAIL] cloud-agent 发布报错：$out"
  exit 1
fi
echo "[OK] cloud-agent 可发布 cloud-main metrics"

out=$(timeout 5 mosquitto_pub -V mqttv5 -h "$HOST" -u "$CLOUD_AGENT_USER" -P "$CLOUD_AGENT_PASS" \
     -t hub/v1/devices/taishan-3m/metrics -m '{"probe":6}' -q 1 2>&1 || true)
if echo "$out" | grep -qi 'not authorized'; then
  echo "[OK] cloud-agent 越权写被拒（ACL 生效）"
else
  echo "[FAIL] cloud-agent 越权写居然成功，ACL 没生效！输出：${out:-<空>}"
  exit 1
fi

cat <<EOF

全部完成。凭据位置：$CRED
查看方式：  sudo cat $CRED

下一步：跑 install-agent.sh 部署指标 Agent。
EOF
