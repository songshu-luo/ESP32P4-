#!/usr/bin/env bash
#
# 编译 ESP32 固件并发布到泰山派 OTA 目录。
#
# 用法：
#   ./tools/ota_publish.sh                         # 用当前 version.txt 编译并发布
#   ./tools/ota_publish.sh 1.0.1-ota-20261121      # 更新 version.txt 后编译并发布
#   ./tools/ota_publish.sh "" --no-build           # 不编译，只发布已有 build/esp32_terminal.bin
#
# 服务端目录：/home/lckfb/esp32-ota
# ESP32 拉取：http://192.168.0.156:8080/version.json
#
# 发布顺序：先上传带版本号的 bin，最后用 mv 原子替换 version.json，
# 避免 ESP32 在传输中途读到指向不存在/不完整文件的 manifest。

set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OTA_HOST="${OTA_HOST:-192.168.0.156}"
OTA_USER="${OTA_USER:-lckfb}"
OTA_DIR="${OTA_DIR:-/home/lckfb/esp32-ota}"

VERSION_ARG="${1:-}"
NO_BUILD="${2:-}"

cd "$APP_DIR"

if [[ -n "$VERSION_ARG" ]]; then
    if [[ ! "$VERSION_ARG" =~ ^[A-Za-z0-9._-]+$ ]]; then
        echo "版本号只能包含字母、数字、点、下划线、连字符: $VERSION_ARG" >&2
        exit 1
    fi
    if [[ -f version.txt ]]; then
        cp version.txt "version.txt.bak-$(date +%Y%m%d-%H%M%S)"
    fi
    printf '%s\n' "$VERSION_ARG" > version.txt
fi

VERSION="$(tr -d '[:space:]' < version.txt)"
if [[ -z "$VERSION" ]]; then
    echo "version.txt 为空" >&2
    exit 1
fi

if [[ "$NO_BUILD" != "--no-build" ]]; then
    if [[ -z "${IDF_PATH:-}" ]]; then
        # shellcheck disable=SC1091
        source "$HOME/esp-idf/export.sh" >/dev/null
    fi
    idf.py build
fi

BIN="$APP_DIR/build/esp32_terminal.bin"
if [[ ! -f "$BIN" ]]; then
    echo "找不到 $BIN，先编译" >&2
    exit 1
fi

SHA256="$(sha256sum "$BIN" | awk '{print $1}')"
SIZE="$(stat -c%s "$BIN")"

STAGE="$APP_DIR/build/ota-publish"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp "$BIN" "$STAGE/esp32_terminal-${VERSION}.bin"

cat > "$STAGE/version.json" <<JSON
{
  "version": "$VERSION",
  "url": "http://${OTA_HOST}:8080/esp32_terminal-${VERSION}.bin",
  "sha256": "$SHA256",
  "size": $SIZE
}
JSON

ssh -o ConnectTimeout=8 "$OTA_USER@$OTA_HOST" "mkdir -p '$OTA_DIR'"
scp "$STAGE/esp32_terminal-${VERSION}.bin" "$OTA_USER@$OTA_HOST:$OTA_DIR/"
scp "$STAGE/version.json" "$OTA_USER@$OTA_HOST:$OTA_DIR/version.json.tmp"
ssh "$OTA_USER@$OTA_HOST" "mv '$OTA_DIR/version.json.tmp' '$OTA_DIR/version.json'"

echo "发布完成"
echo "  version : $VERSION"
echo "  size    : $SIZE"
echo "  sha256  : $SHA256"
echo "  url     : http://${OTA_HOST}:8080/esp32_terminal-${VERSION}.bin"
echo "  manifest: http://${OTA_HOST}:8080/version.json"
