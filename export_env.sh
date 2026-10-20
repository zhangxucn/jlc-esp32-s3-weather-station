#!/bin/zsh
# 在终端先设置 IDF_PATH，脚本再加载 ESP-IDF 官方环境。
if [[ -z "${IDF_PATH:-}" || ! -f "$IDF_PATH/export.sh" ]]; then
    print -u2 "请先将 IDF_PATH 设置为 ESP-IDF v6.1 安装目录。"
    return 1
fi
source "$IDF_PATH/export.sh"
