#ifndef __WEATHER_H__
#define __WEATHER_H__

#include <stdbool.h>
#include "esp_http_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WEATHER_GAODE,  // 高德天气
    WEATHER_XINZHI, // 心知天气
    WEATHER_HEFENG  // 和风天气
} weather_type_t;

// 位置信息字段
typedef struct {
    char *ip_address;      // IP地址
    char *province;        // 省份
    char *city;            // 城市
    char *district;        // 区县
    char *isp;             // 运营商
} location_info_t;

// 天气信息结构体
typedef struct {
    char* weather;          // 天气现象
    float temperature;      // 温度
    float feels_like;       // 体感温度
    float humidity;         // 湿度
    char* wind_dir;         // 风向
    char* wind_scale;       // 风力等级
    float wind_speed;       // 风速(km/h)
    float precip;           // 降水量(mm)
    float pressure;         // 气压(hPa)
    float visibility;       // 能见度(km)
    float cloud;            // 云量(%)
    float dew_point;       // 露点温度
    char* update_time;     // 更新时间
    location_info_t* location_info; // 位置信息
} weather_info_t;

typedef struct {
    char *api_key;
    char *api_host;     // 使用和风天气时需要
    char *city;         // 可选，如果为NULL则自动获取位置
    char *location_id;  // 可选，指定和风天气 Location ID
    char *province;     // 可选，指定地区名称
    char *district;     // 可选，指定区县名称
    weather_type_t type;
} weather_config_t;

#define WEATHER_FORECAST_MAX_DAYS 7
#define WEATHER_HOURLY_MAX_HOURS 24

// 单日预报
typedef struct {
    char *fx_date;          // 预报日期 2026-10-19
    char *text_day;         // 白天天气现象
    char *text_night;       // 夜间天气现象
    char *wind_dir_day;     // 白天风向
    char *wind_scale_day;   // 白天风力
    char *sunrise;           // 日出时间 HH:MM
    char *sunset;            // 日落时间 HH:MM
    float temp_max;         // 最高温度
    float temp_min;         // 最低温度
    float humidity;         // 相对湿度(%)
    float precip;           // 降水量(mm)
    float uv_index;         // 紫外线指数
} weather_daily_t;

// 多日预报
typedef struct {
    char *update_time;                       // 更新时间
    int day_count;                           // 实际天数
    weather_daily_t daily[WEATHER_FORECAST_MAX_DAYS];
    location_info_t *location_info;          // 位置信息
} weather_forecast_t;

// 单小时预报
typedef struct {
    char *fx_time;          // 预报时间 2026-10-20T09:00+08:00
    char *text;             // 天气现象
    float temp;             // 温度
    float pop;              // 降水概率(%)
} weather_hourly_t;

// 逐小时预报
typedef struct {
    char *update_time;
    int hour_count;
    weather_hourly_t hourly[WEATHER_HOURLY_MAX_HOURS];
    location_info_t *location_info;
} weather_hourly_forecast_t;

#define WEATHER_ALERT_MAX 4
typedef struct {
    char event[48];
    char headline[192];
    char severity[16];
    char color[16];
} weather_alert_item_t;

typedef struct {
    bool available;       // false when lookup/request failed or service is unavailable
    int count;
    weather_alert_item_t items[WEATHER_ALERT_MAX];
} weather_alerts_t;

typedef struct {
    bool available;
    char aqi[12];
    char category[24];
} weather_air_quality_t;

/**
 * @brief 获取天气信息
 * 
 * @param config    配置信息
 * @return weather_info_t*
 */
weather_info_t* weather_get(weather_config_t *config);

/**
 * @brief 获取城市信息
 * 
 * @param ip     IP地址如果为NULL则自动获取
 * @return location_info_t* 
 */
location_info_t* get_city_by_ip(const char *ip);

/**
 * @brief 打印天气信息
 * 
 * @param info  天气信息 weather_info_t*
 */
void weather_print_info(const weather_info_t *info);

/**
 * @brief 释放位置信息内存
 * 
 * @param location_info  位置信息 location_info_t*
 */
void location_info_free(location_info_t *location_info);

/**
 * @brief 释放天气信息内存
 *
 * @param info  天气信息 weather_info_t*
 */
void weather_info_free(weather_info_t *info);

/**
 * @brief 获取多日天气预报（和风天气 /v7/weather/7d）
 *
 * @param config    配置信息（type 需为 WEATHER_HEFENG）
 * @param days      需要的天数（1-7，超过7按7处理）
 * @return weather_forecast_t* 失败返回NULL，用完需调用 weather_forecast_free 释放
 */
weather_forecast_t* weather_get_forecast(weather_config_t *config, int days);

/**
 * @brief 打印多日天气预报
 *
 * @param forecast  预报信息 weather_forecast_t*
 */
void weather_print_forecast(const weather_forecast_t *forecast);

/**
 * @brief 释放多日预报内存
 *
 * @param forecast  预报信息 weather_forecast_t*
 */
void weather_forecast_free(weather_forecast_t *forecast);

/**
 * @brief 获取逐小时预报（和风天气 /v7/weather/24h）
 *
 * @param config    配置信息（type 需为 WEATHER_HEFENG）
 * @param hours     需要的小时数（1-24，超过按 24 处理）
 * @return weather_hourly_forecast_t* 失败返回 NULL，用完需调用 weather_hourly_free 释放
 */
weather_hourly_forecast_t* weather_get_hourly(weather_config_t *config, int hours);

// Fetch current official severe-weather alerts for the configured QWeather location.
weather_alerts_t* weather_get_alerts(weather_config_t *config);
void weather_alerts_free(weather_alerts_t *alerts);
weather_air_quality_t* weather_get_air_quality(weather_config_t *config);
void weather_air_quality_free(weather_air_quality_t *air_quality);

/**
 * @brief 打印逐小时预报
 *
 * @param hourly  小时预报信息
 */
void weather_print_hourly(const weather_hourly_forecast_t *hourly);

/**
 * @brief 释放逐小时预报内存
 *
 * @param hourly  小时预报信息
 */
void weather_hourly_free(weather_hourly_forecast_t *hourly);

#ifdef __cplusplus
}
#endif

#endif  // __WEATHER_H__
