/*
 * 中文字体声明（S2-b-zh）
 *
 * 这三个字体文件由 tools/gen_cjk_font.sh 生成，只包含 main/ 下源码字符串里
 * 实际出现过的汉字，因此体积很小（合计约 200 KB 字形数据）。
 *
 * 重新生成的时机：**任何** .c/.h 里新增或改动了中文文案之后。
 * 忘记重新生成的表现是屏幕上出现空心方块（tofu）。
 */

#pragma once

#include "lvgl.h"

LV_FONT_DECLARE(lv_font_cjk_16);
LV_FONT_DECLARE(lv_font_cjk_20);
LV_FONT_DECLARE(lv_font_cjk_28);
