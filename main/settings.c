// 设置项实现（NVS 持久化）
#include <string.h>
#include <stdio.h>
#include <nvs_flash.h>
#include <nvs.h>
#include "esp_log.h"
#include "settings.h"

static const char *TAG = "SET";

static void city_from_active(void)
{
    if (g_settings.city_count < 1) return;
    if (g_settings.active_city < 0 || g_settings.active_city >= g_settings.city_count)
        g_settings.active_city = 0;
    saved_city_t *c = &g_settings.cities[g_settings.active_city];
    g_settings.city_idx = c->city_idx;
    strlcpy(g_settings.location_id, c->location_id, sizeof(g_settings.location_id));
    strlcpy(g_settings.province, c->province, sizeof(g_settings.province));
    strlcpy(g_settings.city, c->city, sizeof(g_settings.city));
    strlcpy(g_settings.district, c->district, sizeof(g_settings.district));
}

static void active_to_city(void)
{
    if (g_settings.city_count < 1) {
        g_settings.city_count = 1;
        g_settings.active_city = 0;
    }
    if (g_settings.active_city < 0 || g_settings.active_city >= g_settings.city_count)
        g_settings.active_city = 0;
    saved_city_t *c = &g_settings.cities[g_settings.active_city];
    c->city_idx = g_settings.city_idx;
    strlcpy(c->location_id, g_settings.location_id, sizeof(c->location_id));
    strlcpy(c->province, g_settings.province, sizeof(c->province));
    strlcpy(c->city, g_settings.city, sizeof(c->city));
    strlcpy(c->district, g_settings.district, sizeof(c->district));
}

// 预设城市（索引 0 = 自动定位）
const char * const CITIES[CITY_COUNT] = {
    "自动定位", "北京", "上海", "广州", "深圳", "济南", "青岛", "杭州",
    "南京", "苏州", "成都", "重庆", "武汉", "西安", "天津", "郑州",
    "长沙", "福州", "厦门", "合肥", "沈阳", "大连", "昆明", "三亚",
};

const int BRIGHTNESS_LEVELS[] = { 100, 60, 30, 10 };
const int BRIGHTNESS_COUNT = 4;
const int REFRESH_LEVELS[] = { 10, 30, 60 };
const int REFRESH_COUNT = 3;
const int SLEEP_LEVELS[] = { 30, 60, 0 };   // 0 = 从不息屏
const int SLEEP_COUNT = 3;

app_settings_t g_settings = {
    .city_idx = 0,
    .brightness = 100,
    .refresh_min = 10,
    .unit_f = false,
    .sleep_sec = 30,
};

const char *settings_city(void)
{
    if (g_settings.location_id[0]) return g_settings.city;
    if (g_settings.city_idx <= 0 || g_settings.city_idx >= CITY_COUNT) return "";
    return CITIES[g_settings.city_idx];
}

const char *settings_location_id(void)
{
    return g_settings.location_id[0] ? g_settings.location_id : NULL;
}

const char *settings_province(void) { return g_settings.province[0] ? g_settings.province : NULL; }
const char *settings_district(void) { return g_settings.district[0] ? g_settings.district : NULL; }
const char *settings_wifi_ssid(void) { return g_settings.wifi_ssid[0] ? g_settings.wifi_ssid : NULL; }
const char *settings_wifi_password(void) { return g_settings.wifi_password; }

void settings_set_wifi(const char *ssid, const char *password)
{
    strlcpy(g_settings.wifi_ssid, ssid ? ssid : "", sizeof(g_settings.wifi_ssid));
    strlcpy(g_settings.wifi_password, password ? password : "", sizeof(g_settings.wifi_password));
}

void settings_set_region(const char *province, const char *city,
                         const char *district, const char *location_id)
{
    strlcpy(g_settings.province, province ? province : "", sizeof(g_settings.province));
    strlcpy(g_settings.city, city ? city : "", sizeof(g_settings.city));
    strlcpy(g_settings.district, district ? district : "", sizeof(g_settings.district));
    strlcpy(g_settings.location_id, location_id ? location_id : "", sizeof(g_settings.location_id));
    g_settings.city_idx = 0;
    active_to_city();
}

void settings_use_auto_location(void)
{
    settings_set_region("", "", "", "");
}

int settings_city_count(void) { return g_settings.city_count; }
int settings_active_city(void) { return g_settings.active_city; }
const saved_city_t *settings_city_at(int index)
{
    return index >= 0 && index < g_settings.city_count ? &g_settings.cities[index] : NULL;
}
bool settings_activate_city(int index)
{
    if (index < 0 || index >= g_settings.city_count) return false;
    bool changed = g_settings.active_city != index;
    g_settings.active_city = index;
    city_from_active();
    return changed;
}
bool settings_cycle_city(int direction)
{
    if (g_settings.city_count < 2 || !direction) return false;
    int next = (g_settings.active_city + (direction > 0 ? 1 : g_settings.city_count - 1)) % g_settings.city_count;
    return settings_activate_city(next);
}
bool settings_add_region(const char *province, const char *city,
                         const char *district, const char *location_id)
{
    const char *id = location_id ? location_id : "";
    for (int i = 0; i < g_settings.city_count; ++i) {
        saved_city_t *c = &g_settings.cities[i];
        bool same = id[0] ? strcmp(c->location_id, id) == 0 :
                    (!c->location_id[0] && strcmp(c->city, city ? city : "") == 0);
        if (same) return settings_activate_city(i);
    }
    if (g_settings.city_count >= SETTINGS_MAX_CITIES) return false;
    int i = g_settings.city_count++;
    saved_city_t *c = &g_settings.cities[i];
    memset(c, 0, sizeof(*c));
    strlcpy(c->location_id, id, sizeof(c->location_id));
    strlcpy(c->province, province ? province : "", sizeof(c->province));
    strlcpy(c->city, city ? city : "", sizeof(c->city));
    strlcpy(c->district, district ? district : "", sizeof(c->district));
    c->city_idx = 0;
    g_settings.active_city = i;
    city_from_active();
    return true;
}
bool settings_remove_city(int index)
{
    if (index < 0 || index >= g_settings.city_count) return false;
    for (int i = index; i + 1 < g_settings.city_count; ++i)
        g_settings.cities[i] = g_settings.cities[i + 1];
    --g_settings.city_count;
    memset(&g_settings.cities[g_settings.city_count], 0, sizeof(saved_city_t));
    if (g_settings.city_count == 0) {
        g_settings.city_count = 1;
        g_settings.active_city = 0;
        memset(&g_settings.cities[0], 0, sizeof(saved_city_t));
    } else if (g_settings.active_city > index) --g_settings.active_city;
    else if (g_settings.active_city == index && g_settings.active_city >= g_settings.city_count)
        g_settings.active_city = g_settings.city_count - 1;
    city_from_active();
    return true;
}

static void load_string(nvs_handle_t nh, const char *key, char *out, size_t size)
{
    size_t len = size;
    if (nvs_get_str(nh, key, out, &len) != ESP_OK) out[0] = '\0';
}

static bool option_contains(const int *values, int count, int value)
{
    for (int i = 0; i < count; ++i) if (values[i] == value) return true;
    return false;
}

void settings_load(void)
{
    nvs_handle_t nh;
    if (nvs_open("settings", NVS_READONLY, &nh) != ESP_OK) {
        g_settings.city_count = 1;
        g_settings.active_city = 0;
        active_to_city();
        ESP_LOGW(TAG, "no saved settings, use defaults");
        return;
    }
    int32_t v;
    if (nvs_get_i32(nh, "city_idx", &v) == ESP_OK && v >= 0 && v < CITY_COUNT) g_settings.city_idx = v;
    if (nvs_get_i32(nh, "brightness", &v) == ESP_OK && v >= 1 && v <= 100) g_settings.brightness = v;
    if (nvs_get_i32(nh, "refresh_min", &v) == ESP_OK && option_contains(REFRESH_LEVELS, REFRESH_COUNT, v)) g_settings.refresh_min = v;
    if (nvs_get_i32(nh, "unit_f", &v) == ESP_OK) g_settings.unit_f = (v != 0);
    if (nvs_get_i32(nh, "sleep_sec", &v) == ESP_OK && option_contains(SLEEP_LEVELS, SLEEP_COUNT, v)) g_settings.sleep_sec = v;
    load_string(nh, "loc_id", g_settings.location_id, sizeof(g_settings.location_id));
    load_string(nh, "province", g_settings.province, sizeof(g_settings.province));
    load_string(nh, "city", g_settings.city, sizeof(g_settings.city));
    load_string(nh, "district", g_settings.district, sizeof(g_settings.district));
    load_string(nh, "wifi_ssid", g_settings.wifi_ssid, sizeof(g_settings.wifi_ssid));
    load_string(nh, "wifi_pass", g_settings.wifi_password, sizeof(g_settings.wifi_password));
    int32_t count = 0, active = 0;
    if (nvs_get_i32(nh, "city_count", &count) == ESP_OK && count > 0 && count <= SETTINGS_MAX_CITIES) {
        g_settings.city_count = count;
        if (nvs_get_i32(nh, "active_city", &active) == ESP_OK && active >= 0 && active < count)
            g_settings.active_city = active;
        for (int i = 0; i < g_settings.city_count; ++i) {
            char key[24]; int32_t idx = 0;
            snprintf(key, sizeof(key), "c%d_idx", i);
            if (nvs_get_i32(nh, key, &idx) == ESP_OK && idx >= 0 && idx < CITY_COUNT) g_settings.cities[i].city_idx = idx;
            snprintf(key, sizeof(key), "c%d_id", i); load_string(nh, key, g_settings.cities[i].location_id, sizeof(g_settings.cities[i].location_id));
            snprintf(key, sizeof(key), "c%d_prov", i); load_string(nh, key, g_settings.cities[i].province, sizeof(g_settings.cities[i].province));
            snprintf(key, sizeof(key), "c%d_city", i); load_string(nh, key, g_settings.cities[i].city, sizeof(g_settings.cities[i].city));
            snprintf(key, sizeof(key), "c%d_dist", i); load_string(nh, key, g_settings.cities[i].district, sizeof(g_settings.cities[i].district));
        }
        city_from_active();
    } else {
        g_settings.city_count = 1; g_settings.active_city = 0; active_to_city();
    }
    nvs_close(nh);
    ESP_LOGI(TAG, "loaded: city=%d bright=%d refresh=%d unit=%s sleep=%d",
             g_settings.city_idx, g_settings.brightness, g_settings.refresh_min,
             g_settings.unit_f ? "F" : "C", g_settings.sleep_sec);
}

void settings_save(void)
{
    nvs_handle_t nh;
    if (nvs_open("settings", NVS_READWRITE, &nh) != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed");
        return;
    }
    nvs_set_i32(nh, "city_idx", g_settings.city_idx);
    nvs_set_i32(nh, "brightness", g_settings.brightness);
    nvs_set_i32(nh, "refresh_min", g_settings.refresh_min);
    nvs_set_i32(nh, "unit_f", g_settings.unit_f ? 1 : 0);
    nvs_set_i32(nh, "sleep_sec", g_settings.sleep_sec);
    nvs_set_str(nh, "loc_id", g_settings.location_id);
    nvs_set_str(nh, "province", g_settings.province);
    nvs_set_str(nh, "city", g_settings.city);
    nvs_set_str(nh, "district", g_settings.district);
    nvs_set_str(nh, "wifi_ssid", g_settings.wifi_ssid);
    nvs_set_str(nh, "wifi_pass", g_settings.wifi_password);
    nvs_set_i32(nh, "city_count", g_settings.city_count);
    nvs_set_i32(nh, "active_city", g_settings.active_city);
    for (int i = 0; i < g_settings.city_count; ++i) {
        char key[24];
        snprintf(key, sizeof(key), "c%d_idx", i); nvs_set_i32(nh, key, g_settings.cities[i].city_idx);
        snprintf(key, sizeof(key), "c%d_id", i); nvs_set_str(nh, key, g_settings.cities[i].location_id);
        snprintf(key, sizeof(key), "c%d_prov", i); nvs_set_str(nh, key, g_settings.cities[i].province);
        snprintf(key, sizeof(key), "c%d_city", i); nvs_set_str(nh, key, g_settings.cities[i].city);
        snprintf(key, sizeof(key), "c%d_dist", i); nvs_set_str(nh, key, g_settings.cities[i].district);
    }
    nvs_commit(nh);
    nvs_close(nh);
    ESP_LOGI(TAG, "saved");
}
