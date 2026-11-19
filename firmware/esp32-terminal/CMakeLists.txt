# ESP32-P4 智能终端固件
#
# 基线来源：espressif/esp-bsp (master)，examples/display
# 板级支持包、LVGL 端口与 LCD/触摸驱动已 vendor 到 components/ 下，
# 保证离线可编译、版本可追溯。
cmake_minimum_required(VERSION 3.16)

set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp32_terminal)
