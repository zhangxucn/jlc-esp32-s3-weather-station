#pragma once

// 私人配置由未纳入版本控制的 app_secrets.h 提供。
// 首次编译请复制 app_secrets.example.h 为 app_secrets.h 并填写自己的配置。
#if defined(__has_include)
#  if __has_include("app_secrets.h")
#    include "app_secrets.h"
#  endif
#endif

// ================= WiFi 配置 =================
// 主网络连接失败后自动尝试备选网络
#ifndef APP_WIFI_SSID
#define APP_WIFI_SSID       ""
#endif
#ifndef APP_WIFI_PASSWORD
#define APP_WIFI_PASSWORD   ""
#endif
#ifndef APP_WIFI_SSID2
#define APP_WIFI_SSID2      ""
#endif
#ifndef APP_WIFI_PASSWORD2
#define APP_WIFI_PASSWORD2  ""
#endif

// ================= 天气配置 =================
// 和风天气（QWeather）；凭据保存在 app_secrets.h，不随公开仓库发布。
#ifndef APP_QWEATHER_KEY
#define APP_QWEATHER_KEY    ""
#endif
#ifndef APP_QWEATHER_HOST
#define APP_QWEATHER_HOST   ""
#endif

// 城市：留空 "" 则按出口 IP 自动定位；也可填 "北京"、"常州" 等
#define APP_CITY            ""

// 自动刷新间隔（分钟），免费版每日 1000 次配额，10 分钟 ≈ 144 次/日
#define APP_UPDATE_INTERVAL_MIN  10

// ================= 预报配置 =================
// 天气预报显示天数：3 / 5 / 7（改后重新编译烧录生效）
#define APP_FORECAST_DAYS       7
