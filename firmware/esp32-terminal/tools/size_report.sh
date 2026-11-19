#!/usr/bin/env bash
# 体积盘点（S2-d 前置）：只读 build/ 产物，不重新编译。
#   bash tools/size_report.sh
set -uo pipefail

PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
B="$PROJ/build"
SIZE=riscv32-esp-elf-size

echo "== app 二进制 =="
ls -l "$B"/esp32_terminal.bin | awk '{print $5" bytes  "$9}'

echo
echo "== 中文字体对象（main/fonts）=="
for f in 16 20 28; do
    o="$B/esp-idf/main/CMakeFiles/__idf_main.dir/fonts/lv_font_cjk_${f}.c.obj"
    if [ -f "$o" ]; then
        printf "cjk_%-3s " "$f"
        $SIZE "$o" | sed -n '2p'
    fi
done

echo
echo "== 应用代码对象（main/src）=="
for o in "$B"/esp-idf/main/CMakeFiles/__idf_main.dir/*.c.obj; do
    [ -f "$o" ] || continue
    printf "%-24s " "$(basename "$o" .c.obj)"
    $SIZE "$o" | sed -n '2p'
done