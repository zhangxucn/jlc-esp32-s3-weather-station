#pragma once
// 设置项（NVS 持久化）：城市 / 亮度 / 刷新间隔 / 温度单位 / 息屏时间
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 预设城市列表（设置页循环选择；"" = 自动定位）
#define CITY_COUNT 24
extern const char * const CITIES[CITY_COUNT];
#define SETTINGS_REGION_NAME_LEN 64
#define SETTINGS_LOCATION_ID_LEN 10
#define SETTINGS_WIFI_SSID_LEN 33
#define SETTINGS_WIFI_PASSWORD_LEN 65
#define SETTINGS_MAX_CITIES 10

typedef struct {
    int city_idx;
    char location_id[SETTINGS_LOCATION_ID_LEN];
    char province[SETTINGS_REGION_NAME_LEN];
    char city[SETTINGS_REGION_NAME_LEN];
    char district[SETTINGS_REGION_NAME_LEN];
} saved_city_t;

typedef struct {
    int  city_idx;        // 旧版设置迁移兼容；0 = 自动定位
    int  brightness;      // 背光 0-100
    int  refresh_min;     // 天气刷新间隔（分钟）
    bool unit_f;          // false=°C true=°F
    int  sleep_sec;       // 息屏秒数，0 = 从不
    char location_id[SETTINGS_LOCATION_ID_LEN];
    char province[SETTINGS_REGION_NAME_LEN];
    char city[SETTINGS_REGION_NAME_LEN];
    char district[SETTINGS_REGION_NAME_LEN];
    char wifi_ssid[SETTINGS_WIFI_SSID_LEN];
    char wifi_password[SETTINGS_WIFI_PASSWORD_LEN];
    int city_count;
    int active_city;
    saved_city_t cities[SETTINGS_MAX_CITIES];
} app_settings_t;

extern app_settings_t g_settings;

// 当前城市名（设置里选的城市；空串=自动定位）
const char *settings_city(void);
const char *settings_location_id(void);
const char *settings_province(void);
const char *settings_district(void);
void settings_set_region(const char *province, const char *city,
                         const char *district, const char *location_id);
int settings_city_count(void);
int settings_active_city(void);
const saved_city_t *settings_city_at(int index);
bool settings_activate_city(int index);
bool settings_cycle_city(int direction);
bool settings_add_region(const char *province, const char *city,
                         const char *district, const char *location_id);
bool settings_remove_city(int index);
void settings_use_auto_location(void);
const char *settings_wifi_ssid(void);
const char *settings_wifi_password(void);
void settings_set_wifi(const char *ssid, const char *password);

// 加载/保存（NVS）
void settings_load(void);
void settings_save(void);

// 亮度选项 / 刷新选项 / 息屏选项（设置页循环用）
extern const int BRIGHTNESS_LEVELS[];
extern const int BRIGHTNESS_COUNT;
extern const int REFRESH_LEVELS[];
extern const int REFRESH_COUNT;
extern const int SLEEP_LEVELS[];   // 末项为 0=从不
extern const int SLEEP_COUNT;

#ifdef __cplusplus
}
#endif
