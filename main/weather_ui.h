#pragma once
// 天气页面渲染：简洁版（浅蓝背景 + 白色文字，静态无动画）
#include <stdbool.h>
#include "weather.h"

#ifdef __cplusplus
extern "C" {
#endif

// 页面编号
#define PAGE_TODAY      0
#define PAGE_3DAY       1
#define PAGE_5DAY       2
#define PAGE_7DAY       3
#define PAGE_SETTINGS   4
#define PAGE_MAX        5

// 渲染「今日天气」页（默认开机页）
void ui_render_today(const weather_info_t *now, const weather_forecast_t *fc,
                     const weather_hourly_forecast_t *hy, const weather_alerts_t *alerts,
                     const weather_air_quality_t *air_quality);

// 渲染「N 天预报」页（days = 3/5/7）
void ui_render_forecast(const weather_forecast_t *fc, int days);
bool ui_city_manager_open(void);
void ui_city_manager_show(void);
void ui_city_manager_cancel(void);
bool ui_city_manager_scroll(int direction);
void ui_render_city_manager(void);
bool ui_city_manager_delete_at(int x, int y);

// 渲染设置页；返回 true 表示有需要立即生效的变更（城市/刷新间隔）
bool ui_render_settings(void);

typedef enum {
    SETTINGS_TAP_NONE = 0,
    SETTINGS_TAP_CHANGED,
    SETTINGS_TAP_REFETCH,
    SETTINGS_TAP_WIFI_SAVE,
    SETTINGS_TAP_WIFI_EDITED,
    SETTINGS_TAP_WIFI_SCAN,
} settings_tap_result_t;
settings_tap_result_t ui_city_manager_tap(int x, int y);

// 设置页触摸处理；区分无效点击、普通变更和需要立即重拉天气的变更
settings_tap_result_t ui_settings_tap(int x, int y);
bool ui_settings_picker_open(void);
void ui_settings_picker_cancel(void);
bool ui_settings_picker_scroll(int direction);
settings_tap_result_t ui_settings_picker_tap(int x, int y);
bool ui_settings_brightness_drag(int x, int y);
bool ui_settings_brightness_drag_end(void);
bool ui_settings_wifi_open(void);
void ui_settings_wifi_cancel(void);
const char *ui_settings_wifi_ssid(void);
const char *ui_settings_wifi_password(void);
settings_tap_result_t ui_settings_wifi_tap(int x, int y);
bool ui_settings_wifi_scanning(void);
bool ui_settings_wifi_scroll(int direction);
void ui_settings_wifi_set_scan_results(const char *const ssids[], int count);

// 渲染开屏/加载提示页
void ui_render_splash(const char *msg);

#ifdef __cplusplus
}
#endif
