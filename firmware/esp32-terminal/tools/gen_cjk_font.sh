#!/usr/bin/env bash
# 生成中文子集字体（S2-b-zh）
#
# 为什么需要这一步：LVGL 自带的 Montserrat 只覆盖拉丁字母，中文字形必须自己生成。
# 全套 Source Han Sans SC 有 16 MB，塞不进 flash，所以只收源码里真正用到的字。
#
# 用法（任意目录）：
#   bash tools/gen_cjk_font.sh
#
# 什么时候要重跑：**任何** main/ 下的 .c/.h 新增或改动了中文文案之后。
# 忘了重跑的表现：屏幕上出现空心方块（tofu）。
#
# 依赖：Node.js。lv_font_conv 默认装在 tools/.cache/ 下（不入库），
# 首次运行时自动安装；若本机不能联网，按提示手动装一次即可。

set -euo pipefail

PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MAIN="$PROJ/main"
FONTS="$MAIN/fonts"
OTF="$PROJ/managed_components/lvgl__lvgl/scripts/generators/built_in_font/SourceHanSansSC-Normal.otf"
CACHE="$PROJ/tools/.cache"
LVFC="$CACHE/node_modules/lv_font_conv/lv_font_conv.js"

[ -f "$OTF" ] || { echo "找不到源字体: $OTF" >&2; exit 1; }

# ---- 找 node（WSL 原生优先，否则调用 Windows 侧的 node.exe）----
# 不能直接执行 npx.cmd：.cmd 是批处理，bash 会因 CRLF 报
# "unexpected EOF while looking for matching `''"。所以一律用 node 直接跑 JS 入口。
if command -v node >/dev/null 2>&1; then
    NODE=(node)
elif [ -x "/mnt/c/Program Files/nodejs/node.exe" ]; then
    NODE=("/mnt/c/Program Files/nodejs/node.exe")
else
    echo "找不到 Node.js：请安装，或确认 C:\\Program Files\\nodejs 存在" >&2
    exit 1
fi

# ---- 首次运行：把 lv_font_conv 装进 tools/.cache（不入库）----
if [ ! -f "$LVFC" ]; then
    echo "[font] 首次运行，安装 lv_font_conv@1.5.3 到 tools/.cache ……"
    mkdir -p "$CACHE"
    # npm 的入口脚本；Windows 版和 WSL 版位置不同，两个都试
    NPM_CLI=""
    if [ -f "/mnt/c/Program Files/nodejs/node_modules/npm/bin/npm-cli.js" ]; then
        NPM_CLI="C:\\Program Files\\nodejs\\node_modules\\npm\\bin\\npm-cli.js"
    else
        NPM_CLI="$(command -v npm >/dev/null 2>&1 && node -e 'console.log(require.resolve("npm/bin/npm-cli.js"))' 2>/dev/null || true)"
    fi
    [ -n "$NPM_CLI" ] || { echo "找不到 npm-cli.js，请手动安装 lv_font_conv" >&2; exit 1; }

    "${NODE[@]}" "$NPM_CLI" install --no-save --no-audit --no-fund \
        --prefix "$(wslpath -w "$CACHE" 2>/dev/null || echo "$CACHE")" \
        lv_font_conv@1.5.3
    [ -f "$LVFC" ] || { echo "安装失败：$LVFC 不存在" >&2; exit 1; }
fi

# ---- 1. 从源码字符串字面量里收集要用的字 ----
RANGES=$(python3 - "$MAIN" <<'PY'
import glob, sys, os

def strip_comments(src):
    """去掉注释和字符常量，只保留字符串字面量里的内容。"""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif c == "/" and i + 1 < n and src[i + 1] == "/":
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif c == '"':
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == '"':
                    break
                j += 1
            out.append(src[i:j + 1])
            i = j + 1
        elif c == "'":
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == "'":
                    break
                j += 1
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)

main = sys.argv[1]
chars = set()
for p in sorted(glob.glob(os.path.join(main, "*.c"))) + sorted(glob.glob(os.path.join(main, "*.h"))):
    for ch in strip_comments(open(p, encoding="utf-8").read()):
        o = ord(ch)
        # CJK 统一汉字 + 中日韩标点 + 全角标点
        if (0x4E00 <= o <= 0x9FFF) or (0x3000 <= o <= 0x303F) or (0xFF00 <= o <= 0xFFEF):
            chars.add(o)

if not chars:
    sys.exit("源码里没找到任何汉字")

cps = sorted(chars)
ranges, start, prev = [], cps[0], cps[0]
for cp in cps[1:]:
    if cp == prev + 1:
        prev = cp
        continue
    ranges.append((start, prev))
    start = prev = cp
ranges.append((start, prev))

# ASCII 一起收进去，这样中文标签里的英文/数字（GB、C、%）也能显示
parts = ["0x20-0x7f"]
parts += [("%#x" % a) if a == b else ("%#x-%#x" % (a, b)) for a, b in ranges]
sys.stderr.write("收集到 %d 个汉字，%d 个字符区间\n" % (len(cps), len(ranges)))
print(",".join(parts))
PY
)

# ---- 2. 逐个尺寸生成 ----
for SIZE in 16 20 28; do
    echo "[font] generating lv_font_cjk_$SIZE.c"
    "${NODE[@]}" "$(wslpath -w "$LVFC" 2>/dev/null || echo "$LVFC")" \
        --size "$SIZE" --bpp 4 --format lvgl \
        --lv-font-name "lv_font_cjk_$SIZE" \
        --font "$(wslpath -w "$OTF" 2>/dev/null || echo "$OTF")" -r "$RANGES" \
        --lv-include lvgl.h \
        --no-compress --force-fast-kern-format \
        -o "$FONTS/lv_font_cjk_$SIZE.c"
done

ls -l "$FONTS"/lv_font_cjk_*.c
echo "[font] 完成。记得重新编译并烧录。"
