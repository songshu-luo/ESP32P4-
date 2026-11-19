#!/usr/bin/env bash
# S2-a 构建：sdkconfig.defaults（可提交）+ sdkconfig.secrets（本地、含密码）叠加。
# 改了 defaults 必须删 sdkconfig 重建，否则旧值一直沿用（见交接文档 §4 坑 3）。
set -euo pipefail

PROJ="$HOME/esp32_workspace/Smart terminal/firmware/esp32-terminal"
cd "$PROJ"

if [ ! -f sdkconfig.secrets ]; then
    echo "缺少 sdkconfig.secrets（本地 MQTT 密码，不入库）。" >&2
    exit 1
fi

# 只在 defaults/secrets/分区表发生变化时重建 sdkconfig，避免每次都全量重编
STAMP=build/.sdkconfig.stamp
if [ ! -f sdkconfig ] || [ ! -f "$STAMP" ] \
   || [ sdkconfig.defaults -nt "$STAMP" ] || [ sdkconfig.secrets -nt "$STAMP" ] \
   || [ partitions.csv -nt "$STAMP" ]; then
    echo "[build] regenerating sdkconfig from defaults + secrets"
    rm -f sdkconfig
fi

# shellcheck disable=SC1091
source "$HOME/esp-idf/export.sh" > /dev/null

export SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.secrets"
idf.py build

mkdir -p build
touch "$STAMP"

# 立刻校验密码确实被编进去了（只看长度，不打印内容）
if grep -q '^CONFIG_TERMINAL_MQTT_PASSWORD=""$' sdkconfig; then
    echo "[build] WARNING: MQTT password is empty in sdkconfig" >&2
    exit 2
fi
echo "[build] MQTT password baked in: yes"
