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
#
# 路径约定：WSL 原生 node 用 Linux 路径，只有 Windows 侧 node.exe 才做
# wslpath 转换；两条路都支持，不要混用（否则会出现 \\wsl.localhost\... 路径）。

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
#
# 两条路必须严格区分：
#   NATIVE_NODE=1：WSL 里的 Linux node，全部传 Linux 路径（/home/...）；
#   NATIVE_NODE=0：Windows 的 node.exe，传进去的路径必须先 wslpath -w。
NATIVE_NODE=0
if command -v node >/dev/null 2>&1; then
    NODE=("$(command -v node)")
    # WSL 互操作也可能让 command -v node 命中 /mnt/c/.../node.exe，
    # 那种情况仍是 Windows 程序，必须走 wslpath 分支。
    case "${NODE[0]}" in
        /mnt/*|*.exe|*.EXE) NATIVE_NODE=0 ;;
        *)                  NATIVE_NODE=1 ;;
    esac
elif [ -x "/mnt/c/Program Files/nodejs/node.exe" ]; then
    NODE=("/mnt/c/Program Files/nodejs/node.exe")
    NATIVE_NODE=0
else
    echo "找不到 Node.js：请安装，或确认 C:\\Program Files\\nodejs 存在" >&2
    exit 1
fi

# 按 node 类型转换路径：原生 node 原样返回，Windows node 转成盘符路径。
node_path() {
    if [ "${NATIVE_NODE}" -eq 1 ]; then
        printf '%s' "$1"
    else
        wslpath -w "$1" 2>/dev/null || printf '%s' "$1"
    fi
}

# ---- 首次运行：把 lv_font_conv 装进 tools/.cache（不入库）----
if [ ! -f "$LVFC" ]; then
    echo "[font] 首次运行，安装 lv_font_conv@1.5.3 到 tools/.cache ……"
    mkdir -p "$CACHE"

    if [ "${NATIVE_NODE}" -eq 1 ]; then
        # 原生 node：npm 也必须是 WSL 里的，安装目标保持 Linux 路径。
        # --cache 指到 tools/.cache 下：~/.npm 可能残留 root 拥有的旧缓存
        # （上一轮用 sudo/容器跑过 npm 就会这样），不要再依赖它。
        command -v npm >/dev/null 2>&1 || {
            echo "找到 node 但没有 npm，请安装 npm 或手动装 lv_font_conv" >&2
            exit 1
        }
        ( cd "$CACHE" && npm install --cache "$CACHE/.npm" \
            --no-save --no-audit --no-fund lv_font_conv@1.5.3 )
    else
        # Windows node：npm-cli.js 和 --prefix 都要转成 Windows 路径。
        NPM_CLI=""
        if [ -f "/mnt/c/Program Files/nodejs/node_modules/npm/bin/npm-cli.js" ]; then
            NPM_CLI="/mnt/c/Program Files/nodejs/node_modules/npm/bin/npm-cli.js"
        else
            NPM_CLI="$("${NODE[@]}" -e 'console.log(require.resolve("npm/bin/npm-cli.js"))' 2>/dev/null || true)"
        fi
        [ -n "$NPM_CLI" ] || { echo "找不到 npm-cli.js，请手动安装 lv_font_conv" >&2; exit 1; }

        "${NODE[@]}" "$(node_path "$NPM_CLI")" install --no-save --no-audit --no-fund \
            --prefix "$(node_path "$CACHE")" \
            lv_font_conv@1.5.3
    fi

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
    "${NODE[@]}" "$(node_path "$LVFC")" \
        --size "$SIZE" --bpp 4 --format lvgl \
        --lv-font-name "lv_font_cjk_$SIZE" \
        --font "$(node_path "$OTF")" -r "$RANGES" \
        --lv-include lvgl.h \
        --no-compress --force-fast-kern-format \
        -o "$(node_path "$FONTS/lv_font_cjk_$SIZE.c")"
done

ls -l "$FONTS"/lv_font_cjk_*.c
echo "[font] 完成。记得重新编译并烧录。"
