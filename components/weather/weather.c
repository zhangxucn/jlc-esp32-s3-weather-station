#include "weather.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "lwip/dns.h"
#include "esp_crt_bundle.h"
#include <ctype.h>
#include <stdlib.h>
#include "zlib.h"
static const char *TAG = "WEATHER";

// 定义安全释放宏
#define SAFE_FREE(ptr) do { if (ptr) { free(ptr); ptr = NULL; } } while(0)

// 内部使用的缓冲区
static char weather_buffer[8192];
static size_t weather_len = 0;

// 用于存储 Content-Encoding 头部值
static char content_encoding_value[32] = {0};

// URL编码函数
static void url_encode(char *dest, const char *src, size_t max_len) {
    size_t i = 0, j = 0;
    while (src[i] != '\0' && j < max_len - 1) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            dest[j++] = src[i++];
        } else {
            if (j + 3 >= max_len) break;  // 额外的安全检查
            snprintf(&dest[j], max_len - j, "%%%02X", c);
            j += 3;
            i++;
        }
    }
    dest[j] = '\0';
}

// HTTP事件处理器
static esp_err_t weather_http_handler(esp_http_client_event_t *evt) {
    switch(evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            // ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            // 存储Content-Encoding头
            if (strcasecmp(evt->header_key, "Content-Encoding") == 0) {
                strlcpy(content_encoding_value, evt->header_value, sizeof(content_encoding_value));
            }
            break;

        case HTTP_EVENT_ON_DATA:
            if (weather_len + evt->data_len >= sizeof(weather_buffer)) {
                ESP_LOGE(TAG, "Buffer overflow");
                return ESP_FAIL;
            }
            memcpy(weather_buffer + weather_len, evt->data, evt->data_len);
            weather_len += evt->data_len;
            break;
        default:
            break;
    }
    return ESP_OK;
}

// 解压 gzip 数据（支持动态扩容，7天预报解压后约4KB）
static char* decompress_gzip_data(const char *compressed_data, int compressed_len) {
    z_stream strm;
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = compressed_len;
    strm.next_in = (Bytef *)compressed_data;

    // 初始按压缩后4倍分配，至少8KB
    size_t out_capacity = compressed_len * 4;
    if (out_capacity < 8192) out_capacity = 8192;

    char *uncompressed_data = malloc(out_capacity);
    if (uncompressed_data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for uncompressed data");
        return NULL;
    }

    strm.avail_out = out_capacity;
    strm.next_out = (Bytef *)uncompressed_data;

    int ret = inflateInit2(&strm, 16 + MAX_WBITS);
    if (ret != Z_OK) {
        ESP_LOGE(TAG, "inflateInit2 failed: %d", ret);
        free(uncompressed_data);
        return NULL;
    }

    while (1) {
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) {
            break;
        }
        if (ret == Z_OK && strm.avail_out == 0) {
            // 输出缓冲不足，扩容一倍继续
            size_t used = strm.total_out;
            out_capacity *= 2;
            char *tmp = realloc(uncompressed_data, out_capacity);
            if (tmp == NULL) {
                ESP_LOGE(TAG, "realloc failed during decompress");
                inflateEnd(&strm);
                free(uncompressed_data);
                return NULL;
            }
            uncompressed_data = tmp;
            strm.next_out = (Bytef *)(uncompressed_data + used);
            strm.avail_out = out_capacity - used;
            continue;
        }
        ESP_LOGE(TAG, "inflate failed: %d", ret);
        inflateEnd(&strm);
        free(uncompressed_data);
        return NULL;
    }

    inflateEnd(&strm);

    // 确保解压后的数据以 null 结尾
    if (strm.total_out >= out_capacity) {
        char *tmp = realloc(uncompressed_data, out_capacity + 1);
        if (tmp == NULL) {
            free(uncompressed_data);
            return NULL;
        }
        uncompressed_data = tmp;
    }
    uncompressed_data[strm.total_out] = '\0';

    return uncompressed_data;
}

// 执行HTTP请求
static char* weather_http_request_internal(const char *url, const char *api_key_header) {
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = weather_http_handler,
        .timeout_ms = 60000,
        .buffer_size = 3072,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return NULL;
    if (api_key_header && api_key_header[0] &&
        esp_http_client_set_header(client, "X-QW-Api-Key", api_key_header) != ESP_OK) {
        esp_http_client_cleanup(client);
        return NULL;
    }

    weather_len = 0;
    memset(weather_buffer, 0, sizeof(weather_buffer));
    memset(content_encoding_value, 0, sizeof(content_encoding_value));

    esp_err_t err = esp_http_client_perform(client);    // 执行HTTP请求
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return NULL;
    }

    if (weather_len >= sizeof(weather_buffer)) {
        weather_len = sizeof(weather_buffer) - 1;
    }
    weather_buffer[weather_len] = '\0';

    // 检查是否是 gzip 压缩数据
    if (content_encoding_value[0] && strcmp(content_encoding_value, "gzip") == 0) {
        ESP_LOGI(TAG, "Data is gzip compressed, decompressing...");
        char *uncompressed_data = decompress_gzip_data(weather_buffer, weather_len);
        if (uncompressed_data) {
            esp_http_client_cleanup(client);
            return uncompressed_data;
        } else {
            esp_http_client_cleanup(client);
            return NULL;
        }
    }

    esp_http_client_cleanup(client);
    return strdup(weather_buffer);
}

static char* weather_http_request(const char *url)
{
    return weather_http_request_internal(url, NULL);
}

static char* weather_http_request_with_api_key(const char *url, const char *api_key)
{
    return weather_http_request_internal(url, api_key);
}

location_info_t* get_city_by_ip(const char *ip) {
    char url[128];
    // 使用ip-api.com的API
    snprintf(url, sizeof(url), "http://ip-api.com/json/%s?lang=zh-CN", ip ? ip : "");

    char *response = weather_http_request(url);
    if (!response) return NULL;

    cJSON *root = cJSON_Parse(response);
    if (!root) {
        free(response);
        return NULL;
    }

    location_info_t *loc_info = calloc(1, sizeof(location_info_t));
    if (!loc_info) {
        cJSON_Delete(root);
        free(response);
        return NULL;
    }

    // 解析IP信息
    cJSON *status = cJSON_GetObjectItem(root, "status");
    if (status && cJSON_IsString(status) && strcmp(status->valuestring, "success") == 0) {
        // 解析省份和城市信息
        cJSON *regionName = cJSON_GetObjectItem(root, "regionName");
        cJSON *city = cJSON_GetObjectItem(root, "city");
        cJSON *isp = cJSON_GetObjectItem(root, "isp");
        cJSON *query = cJSON_GetObjectItem(root, "query");

        if (regionName && cJSON_IsString(regionName))
            loc_info->province = strdup(regionName->valuestring);
        if (city && cJSON_IsString(city))
            loc_info->city = strdup(city->valuestring);
        if (isp && cJSON_IsString(isp))
            loc_info->isp = strdup(isp->valuestring);
        if (query && cJSON_IsString(query))
            loc_info->ip_address = strdup(query->valuestring);
    }

    cJSON_Delete(root);
    free(response);
    return loc_info;
}

// 解析高德天气
static weather_info_t* parse_gaode(const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return NULL;

    weather_info_t *info = calloc(1, sizeof(weather_info_t));
    if (!info) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON *lives = cJSON_GetObjectItem(root, "lives");
    if (lives && cJSON_IsArray(lives)) {
        cJSON *item = cJSON_GetArrayItem(lives, 0);
        if (item) {
            info->weather = strdup(cJSON_GetObjectItem(item, "weather")->valuestring);
            info->temperature = atof(cJSON_GetObjectItem(item, "temperature_float")->valuestring);
            info->humidity = atof(cJSON_GetObjectItem(item, "humidity_float")->valuestring);
            info->wind_dir = strdup(cJSON_GetObjectItem(item, "winddirection")->valuestring);
            info->wind_scale = strdup(cJSON_GetObjectItem(item, "windpower")->valuestring);
            info->update_time = strdup(cJSON_GetObjectItem(item, "reporttime")->valuestring);
        }
    }

    cJSON_Delete(root);
    return info;
}

// 解析心知天气
static weather_info_t* parse_xinzhi(const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return NULL;

    weather_info_t *info = calloc(1, sizeof(weather_info_t));
    if (!info) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON *results = cJSON_GetObjectItem(root, "results");
    if (results && cJSON_IsArray(results)) {
        cJSON *item = cJSON_GetArrayItem(results, 0);
        if (item) {
            cJSON *now = cJSON_GetObjectItem(item, "now");
            if (now) {
                // 提取天气信息
                info->weather = strdup(cJSON_GetObjectItem(now, "text")->valuestring);
                info->temperature = atof(cJSON_GetObjectItem(now, "temperature")->valuestring);

                // 提取最后更新时间
                cJSON *last_update = cJSON_GetObjectItem(item, "last_update");
                if (last_update) {
                    info->update_time = strdup(last_update->valuestring);
                }
            }
        }
    }

    cJSON_Delete(root);
    return info;
}

// 获取和风天气的城市ID
static char* get_hefeng_location_id(const char *city_name, const char *api_host, const char *api_key) {
    char url[256];
    char encoded_city[64] = {0};
    url_encode(encoded_city, city_name, sizeof(encoded_city));

    snprintf(url, sizeof(url), "https://%s/geo/v2/city/lookup?key=%s&location=%s&number=1",
            api_host, api_key, encoded_city);

    char *response = weather_http_request(url);
    if (!response) return NULL;

    cJSON *root = cJSON_Parse(response);
    if (!root) {
        free(response);
        return NULL;
    }
    char *location_id = NULL;
    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (code && cJSON_IsString(code) && strcmp(code->valuestring, "200") == 0) {
        cJSON *location = cJSON_GetObjectItem(root, "location");
        if (location && cJSON_IsArray(location)) {
            cJSON *first_item = cJSON_GetArrayItem(location, 0);
            if (first_item) {
                cJSON *id = cJSON_GetObjectItem(first_item, "id");
                if (id && cJSON_IsString(id)) {
                    location_id = strdup(id->valuestring);
                }
            }
        }
    }
    cJSON_Delete(root);
    free(response);
    return location_id;
}

static location_info_t *location_from_config(const weather_config_t *config)
{
    if (config && ((config->location_id && config->location_id[0]) ||
                   (config->city && config->city[0]))) {
        location_info_t *loc = calloc(1, sizeof(*loc));
        if (!loc) return NULL;
        if (config->province) loc->province = strdup(config->province);
        if (config->city) loc->city = strdup(config->city);
        if (config->district) loc->district = strdup(config->district);
        return loc;
    }
    return get_city_by_ip(NULL);
}

static char *resolve_hefeng_location_id(const weather_config_t *config,
                                         const location_info_t *loc)
{
    if (config && config->location_id && config->location_id[0])
        return strdup(config->location_id);
    if (!loc || !loc->city) return NULL;
    return get_hefeng_location_id(loc->city, config->api_host, config->api_key);
}

static bool get_hefeng_coordinates(const weather_config_t *config, char *lat, size_t lat_size,
                                  char *lon, size_t lon_size)
{
    location_info_t *loc = location_from_config(config);
    if (!loc) return false;
    char *location_id = resolve_hefeng_location_id(config, loc);
    location_info_free(loc);
    if (!location_id) return false;

    char url[320];
    snprintf(url, sizeof(url), "https://%s/geo/v2/city/lookup?key=%s&location=%s&number=1&lang=zh",
             config->api_host, config->api_key, location_id);
    free(location_id);

    char *response = weather_http_request(url);
    if (!response) return false;
    cJSON *root = cJSON_Parse(response);
    free(response);
    if (!root) return false;

    bool ok = false;
    cJSON *code = cJSON_GetObjectItem(root, "code");
    cJSON *locations = cJSON_GetObjectItem(root, "location");
    cJSON *first = cJSON_IsArray(locations) ? cJSON_GetArrayItem(locations, 0) : NULL;
    cJSON *lat_item = first ? cJSON_GetObjectItem(first, "lat") : NULL;
    cJSON *lon_item = first ? cJSON_GetObjectItem(first, "lon") : NULL;
    if (code && cJSON_IsString(code) && strcmp(code->valuestring, "200") == 0 &&
        lat_item && cJSON_IsString(lat_item) && lon_item && cJSON_IsString(lon_item)) {
        strlcpy(lat, lat_item->valuestring, lat_size);
        strlcpy(lon, lon_item->valuestring, lon_size);
        ok = lat[0] && lon[0];
    }
    cJSON_Delete(root);
    return ok;
}

static int alert_severity_rank(const char *severity)
{
    if (!severity) return 0;
    if (strcmp(severity, "extreme") == 0) return 4;
    if (strcmp(severity, "severe") == 0) return 3;
    if (strcmp(severity, "moderate") == 0) return 2;
    if (strcmp(severity, "minor") == 0) return 1;
    return 0;
}

weather_air_quality_t* weather_get_air_quality(weather_config_t *config)
{
    weather_air_quality_t *result = calloc(1, sizeof(*result));
    if (!result) return NULL;
    if (!config || config->type != WEATHER_HEFENG || !config->api_host || !config->api_key)
        return result;

    char lat[20] = {0}, lon[20] = {0};
    if (!get_hefeng_coordinates(config, lat, sizeof(lat), lon, sizeof(lon))) {
        ESP_LOGW(TAG, "Air quality location lookup failed");
        return result;
    }

    char url[320];
    snprintf(url, sizeof(url), "https://%s/airquality/v1/current/%s/%s?lang=zh",
             config->api_host, lat, lon);
    char *response = weather_http_request_with_api_key(url, config->api_key);
    if (!response) return result;

    cJSON *root = cJSON_Parse(response);
    free(response);
    if (!root) return result;

    cJSON *indexes = cJSON_GetObjectItem(root, "indexes");
    cJSON *selected = NULL;
    cJSON *fallback = NULL;
    if (cJSON_IsArray(indexes)) {
        int count = cJSON_GetArraySize(indexes);
        for (int i = 0; i < count; ++i) {
            cJSON *item = cJSON_GetArrayItem(indexes, i);
            cJSON *code = item ? cJSON_GetObjectItem(item, "code") : NULL;
            if (!code || !cJSON_IsString(code)) continue;
            if (strcmp(code->valuestring, "cn-mee") == 0) {
                selected = item;
                break;
            }
            if (!fallback && strcmp(code->valuestring, "qaqi") != 0) fallback = item;
        }
    }
    if (!selected) selected = fallback;
    if (selected) {
        cJSON *aqi = cJSON_GetObjectItem(selected, "aqiDisplay");
        cJSON *category = cJSON_GetObjectItem(selected, "category");
        if (aqi && cJSON_IsString(aqi) && aqi->valuestring[0])
            strlcpy(result->aqi, aqi->valuestring, sizeof(result->aqi));
        if (category && cJSON_IsString(category) && category->valuestring[0])
            strlcpy(result->category, category->valuestring, sizeof(result->category));
        result->available = result->aqi[0] || result->category[0];
    }
    cJSON_Delete(root);
    if (result->available)
        ESP_LOGI(TAG, "Air quality AQI: %s %s", result->aqi, result->category);
    else
        ESP_LOGW(TAG, "Air quality data unavailable");
    return result;
}

void weather_air_quality_free(weather_air_quality_t *air_quality)
{
    free(air_quality);
}

weather_alerts_t* weather_get_alerts(weather_config_t *config)
{
    weather_alerts_t *result = calloc(1, sizeof(*result));
    if (!result) return NULL;
    if (!config || config->type != WEATHER_HEFENG || !config->api_host || !config->api_key) return result;

    char lat[20] = {0}, lon[20] = {0};
    if (!get_hefeng_coordinates(config, lat, sizeof(lat), lon, sizeof(lon))) {
        ESP_LOGW(TAG, "Weather alert location lookup failed");
        return result;
    }

    char url[320];
    snprintf(url, sizeof(url), "https://%s/weatheralert/v1/current/%s/%s?key=%s&lang=zh",
             config->api_host, lat, lon, config->api_key);
    char *response = weather_http_request(url);
    if (!response) return result;
    cJSON *root = cJSON_Parse(response);
    free(response);
    if (!root) return result;

    cJSON *metadata = cJSON_GetObjectItem(root, "metadata");
    cJSON *code = cJSON_GetObjectItem(root, "code");
    bool response_ok = metadata != NULL || (code && cJSON_IsString(code) && strcmp(code->valuestring, "200") == 0);
    cJSON *alerts = cJSON_GetObjectItem(root, "alerts");
    if (response_ok) {
        result->available = true;
        if (cJSON_IsArray(alerts)) {
            int n = cJSON_GetArraySize(alerts);
            for (int i = 0; i < n && result->count < WEATHER_ALERT_MAX; ++i) {
                cJSON *item = cJSON_GetArrayItem(alerts, i);
                cJSON *message_type = item ? cJSON_GetObjectItem(item, "messageType") : NULL;
                cJSON *message_code = message_type ? cJSON_GetObjectItem(message_type, "code") : NULL;
                if (message_code && cJSON_IsString(message_code) &&
                    strcmp(message_code->valuestring, "cancel") == 0) continue;

                weather_alert_item_t *out = &result->items[result->count++];
                cJSON *event_type = cJSON_GetObjectItem(item, "eventType");
                cJSON *value = event_type ? cJSON_GetObjectItem(event_type, "name") : NULL;
                if (value && cJSON_IsString(value)) strlcpy(out->event, value->valuestring, sizeof(out->event));
                value = cJSON_GetObjectItem(item, "headline");
                if (value && cJSON_IsString(value)) strlcpy(out->headline, value->valuestring, sizeof(out->headline));
                value = cJSON_GetObjectItem(item, "severity");
                if (value && cJSON_IsString(value)) strlcpy(out->severity, value->valuestring, sizeof(out->severity));
                cJSON *color = cJSON_GetObjectItem(item, "color");
                value = color ? cJSON_GetObjectItem(color, "code") : NULL;
                if (value && cJSON_IsString(value)) strlcpy(out->color, value->valuestring, sizeof(out->color));
            }
        }

        // Put the most serious current alert first for the compact home-screen banner.
        for (int i = 0; i < result->count; ++i) {
            for (int j = i + 1; j < result->count; ++j) {
                if (alert_severity_rank(result->items[j].severity) > alert_severity_rank(result->items[i].severity)) {
                    weather_alert_item_t swap = result->items[i];
                    result->items[i] = result->items[j];
                    result->items[j] = swap;
                }
            }
        }
    }
    cJSON_Delete(root);
    if (!result->available) ESP_LOGW(TAG, "Weather alert API returned an error response");
    else ESP_LOGI(TAG, "Weather alerts: %d active", result->count);
    return result;
}

void weather_alerts_free(weather_alerts_t *alerts)
{
    free(alerts);
}

// 解析和风天气
static weather_info_t* parse_hefeng(const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return NULL;

    weather_info_t *info = calloc(1, sizeof(weather_info_t));
    if (!info) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON *now = cJSON_GetObjectItem(root, "now");
    if (now) {
        // 基本天气信息
        info->weather = strdup(cJSON_GetObjectItem(now, "text")->valuestring);
        info->temperature = atof(cJSON_GetObjectItem(now, "temp")->valuestring);
        info->feels_like = atof(cJSON_GetObjectItem(now, "feelsLike")->valuestring);
        info->humidity = atof(cJSON_GetObjectItem(now, "humidity")->valuestring);

        // 风信息
        info->wind_dir = strdup(cJSON_GetObjectItem(now, "windDir")->valuestring);
        info->wind_scale = strdup(cJSON_GetObjectItem(now, "windScale")->valuestring);
        info->wind_speed = atof(cJSON_GetObjectItem(now, "windSpeed")->valuestring);

        // 其他气象数据
        info->precip = atof(cJSON_GetObjectItem(now, "precip")->valuestring);
        info->pressure = atof(cJSON_GetObjectItem(now, "pressure")->valuestring);
        info->visibility = atof(cJSON_GetObjectItem(now, "vis")->valuestring);
        info->cloud = atof(cJSON_GetObjectItem(now, "cloud")->valuestring);
        info->dew_point = atof(cJSON_GetObjectItem(now, "dew")->valuestring);

        // 更新时间
        info->update_time = strdup(cJSON_GetObjectItem(root, "updateTime")->valuestring);
    }

    cJSON_Delete(root);
    return info;
}

weather_info_t* weather_get(weather_config_t *config) {
    if (!config) return NULL;
    location_info_t *location_info = location_from_config(config);
    if (!location_info) { ESP_LOGE(TAG, "Failed to resolve location"); return NULL; }

    char url[256];
    weather_info_t *weather_info = NULL;
    char encoded_city[64] = {0};
    char *response = NULL;
    switch(config->type) {
        case WEATHER_GAODE:
            if(!config->api_key){
                ESP_LOGE(TAG, "GAODE API key is not configured");
                return NULL;
            }
            url_encode(encoded_city, location_info->city, sizeof(encoded_city));
            snprintf(url, sizeof(url), "https://restapi.amap.com/v3/weather/weatherInfo?city=%s&key=%s",
                    encoded_city, config->api_key);
            response = weather_http_request(url);
            if (response) {
                weather_info = parse_gaode(response);
                free(response); // 解析完成后释放响应内存
            }
            break;


        case WEATHER_XINZHI:
            if(!config->api_key){
                ESP_LOGE(TAG, "XINZHI API key is not configured");
                return NULL;
            }
            url_encode(encoded_city, location_info->city, sizeof(encoded_city));
            snprintf(url, sizeof(url), "https://api.seniverse.com/v3/weather/now.json?key=%s&location=%s&language=zh-Hans&unit=c",
                    config->api_key, encoded_city);
            response = weather_http_request(url);
            if (response) {
                weather_info = parse_xinzhi(response);
                free(response);
            }
            break;


        case WEATHER_HEFENG:
            if (!config->api_host || !config->api_key) {
                ESP_LOGE(TAG, "HEFENG API host or key is not configured");
                return NULL;
            }
            char *location_id = resolve_hefeng_location_id(config, location_info);
            if (!location_id) {
                ESP_LOGE(TAG, "Failed to get location ID for city: %s", location_info->city);
                location_info_free(location_info);
                return NULL;
            }

            snprintf(url, sizeof(url), "https://%s/v7/weather/now?location=%s&key=%s",
                    config->api_host, location_id, config->api_key);
            response = weather_http_request(url);
            if (response) {
                weather_info = parse_hefeng(response);
                free(response);
            }

            free(location_id);
            break;

        default:
            break;
    }
    if(weather_info!=NULL) {
        weather_info->location_info = location_info; // 保存位置信息
    }
    return weather_info;
}

void weather_print_info(const weather_info_t *info) {
    if (!info) return;

    // 天气符号映射
    const char* weather_icon = "☁️";
    if (info->weather) {
        if (strstr(info->weather, "晴")) weather_icon = "☀️";
        else if (strstr(info->weather, "雨")) weather_icon = "🌧️";
        else if (strstr(info->weather, "雪")) weather_icon = "❄️";
        else if (strstr(info->weather, "雷")) weather_icon = "⛈️";
        else if (strstr(info->weather, "雾")) weather_icon = "🌫️";
        else if (strstr(info->weather, "风")) weather_icon = "🌬️";
    }

    printf("\n  🌈 WEATHER REPORT %s\n", weather_icon);
    printf("%s\n"," _ _ _ _____ _____ _____ _____ _____ _____ ");
    printf("%s\n","| | | |   __|  _  |_   _|  |  |   __| __  |");
    printf("%s\n","| | | |   __|     | | | |     |   __|    -|");
    printf("%s\n","|_____|_____|__|__| |_| |__|__|_____|__|__|\n");

    // 位置信息
    if (info->location_info) {
        char city[64] = {0};
        if (info->location_info->province && info->location_info->city) {
            snprintf(city, sizeof(city), "%s·%s",
                    info->location_info->province,
                    info->location_info->city);
        } else if (info->location_info->province) {
            snprintf(city, sizeof(city), "%s",
                    info->location_info->province);
        } else if (info->location_info->city) {
            snprintf(city, sizeof(city), "%s",
                    info->location_info->city);
        }
        printf("📍 位置: %s\n", city);
    }

    // 预格式化带单位的数据
    char temp_str[16] = "", feels_str[16] = "", humidity_str[16] = "";
    char precip_str[16] = "", pressure_str[16] = "", vis_str[16] = "";
    char cloud_str[16] = "", dew_str[16] = "", wspeed_str[16] = "";

    if (info->temperature != 0.0f) snprintf(temp_str, sizeof(temp_str), "%.1f℃", info->temperature);
    if (info->feels_like != 0.0f) snprintf(feels_str, sizeof(feels_str), "%.1f℃", info->feels_like);
    if (info->humidity != 0.0f) snprintf(humidity_str, sizeof(humidity_str), "%.1f%%", info->humidity);
    if (info->wind_speed != 0.0f) snprintf(wspeed_str, sizeof(wspeed_str), "%.1fkm/h", info->wind_speed);
    if (info->precip != 0.0f) snprintf(precip_str, sizeof(precip_str), "%.1fmm", info->precip);
    if (info->pressure != 0.0f) snprintf(pressure_str, sizeof(pressure_str), "%.1fhPa", info->pressure);
    if (info->visibility != 0.0f) snprintf(vis_str, sizeof(vis_str), "%.1fkm", info->visibility);
    if (info->cloud != 0.0f) snprintf(cloud_str, sizeof(cloud_str), "%.1f%%", info->cloud);
    if (info->dew_point != 0.0f) snprintf(dew_str, sizeof(dew_str), "%.1f℃", info->dew_point);

    printf("┌───────────────────────────────────────┐\n");
    bool is_empty =
        (!info->weather || strlen(info->weather) == 0) &&
        info->temperature == 0.0f &&
        info->feels_like == 0.0f &&
        info->humidity == 0.0f &&
        (!info->wind_dir || strlen(info->wind_dir) == 0) &&
        (!info->wind_scale || strlen(info->wind_scale) == 0) &&
        info->wind_speed == 0.0f &&
        info->precip == 0.0f &&
        info->pressure == 0.0f &&
        info->visibility == 0.0f &&
        info->cloud == 0.0f &&
        info->dew_point == 0.0f;

    if (is_empty)
    {
        printf("│ ⚠️ 未获取到有效的天气数据             │\n");
        goto end;
    }

    if (info->weather) printf("│ %s  天气: %-29s │\n", weather_icon, info->weather);
    if (info->temperature != 0.0f) printf("│ 🌡️  温度: %-30s │\n", temp_str);
    if (info->feels_like != 0.0f) printf("│ 🤒 体感: %-30s │\n", feels_str);
    if (info->humidity != 0.0f) printf("│ 💧 湿度: %-28s │\n", humidity_str);
    if (info->wind_dir) printf("│ 🍃 风向: %-31s │\n", info->wind_dir);
    if (info->wind_scale) printf("│ 💨 风力: %-28s │\n", info->wind_scale);
    if (info->wind_speed != 0.0f) printf("│ 🌬️  风速: %-28s │\n", wspeed_str);
    if (info->precip != 0.0f) printf("│ 🌧️  降水: %-28s │\n", precip_str);
    if (info->pressure != 0.0f) printf("│ ⏲️  气压: %-28s │\n", pressure_str);
    if (info->visibility != 0.0f) printf("│ 👁️  能见度: %-26s │\n", vis_str);
    if (info->cloud != 0.0f) printf("│ ☁️  云量: %-28s │\n", cloud_str);
    if (info->dew_point != 0.0f) printf("│ 💦 露点: %-30s │\n", dew_str);

end:
        printf("└───────────────────────────────────────┘\n");

    if (info->update_time) printf("\n🕒 更新时间: %s\n", info->update_time);
    printf("\n");
}

void location_info_free(location_info_t *location_info) {
    if (!location_info) return;

    SAFE_FREE(location_info->ip_address);
    SAFE_FREE(location_info->province);
    SAFE_FREE(location_info->city);
    SAFE_FREE(location_info->district);
    SAFE_FREE(location_info->isp);
    SAFE_FREE(location_info);
}

void weather_info_free(weather_info_t *info) {
    if (!info) return;

    // 释放天气信息
    SAFE_FREE(info->weather);
    SAFE_FREE(info->wind_dir);
    SAFE_FREE(info->wind_scale);
    SAFE_FREE(info->update_time);

    // 释放位置信息
    if (info->location_info) {
        location_info_free(info->location_info);
    }
    SAFE_FREE(info);
}

// ==================== 多日天气预报 ====================

// 取字符串字段（返回NULL表示缺失）
static const char* json_str(cJSON *obj, const char *key) {
    cJSON *v = cJSON_GetObjectItem(obj, key);
    return (v && cJSON_IsString(v) && v->valuestring) ? v->valuestring : NULL;
}

// 取数值字段（和风API数字字段是字符串类型）
static float json_num(cJSON *obj, const char *key) {
    cJSON *v = cJSON_GetObjectItem(obj, key);
    if (!v) return 0.0f;
    if (cJSON_IsString(v) && v->valuestring) return atof(v->valuestring);
    if (cJSON_IsNumber(v)) return (float)v->valuedouble;
    return 0.0f;
}

// 由日期字符串 (2026-10-19) 计算星期（蔡勒公式，不依赖系统时间）
static const char* forecast_weekday(const char *fx_date) {
    int y = 0, m = 0, d = 0;
    if (!fx_date || sscanf(fx_date, "%d-%d-%d", &y, &m, &d) != 3) return "";
    if (m < 3) { m += 12; y -= 1; }
    int k = y % 100;
    int j = y / 100;
    int h = (d + 13 * (m + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7; // 0=周六
    static const char *names[] = {"周六", "周日", "周一", "周二", "周三", "周四", "周五"};
    return names[h % 7];
}

// 解析和风 /v7/weather/7d 响应
static weather_forecast_t* parse_hefeng_forecast(const char *json, int days) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return NULL;

    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (!code || !cJSON_IsString(code) || strcmp(code->valuestring, "200") != 0) {
        ESP_LOGE(TAG, "Forecast API error, code=%s",
                 (code && cJSON_IsString(code)) ? code->valuestring : "?");
        cJSON_Delete(root);
        return NULL;
    }

    weather_forecast_t *forecast = calloc(1, sizeof(weather_forecast_t));
    if (!forecast) {
        cJSON_Delete(root);
        return NULL;
    }

    const char *update = json_str(root, "updateTime");
    if (update) forecast->update_time = strdup(update);

    cJSON *daily = cJSON_GetObjectItem(root, "daily");
    if (daily && cJSON_IsArray(daily)) {
        int n = cJSON_GetArraySize(daily);
        if (n > days) n = days;
        if (n > WEATHER_FORECAST_MAX_DAYS) n = WEATHER_FORECAST_MAX_DAYS;

        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(daily, i);
            if (!item) continue;
            weather_daily_t *d = &forecast->daily[i];

            const char *s;
            if ((s = json_str(item, "fxDate")))     d->fx_date = strdup(s);
            if ((s = json_str(item, "textDay")))     d->text_day = strdup(s);
            if ((s = json_str(item, "textNight")))   d->text_night = strdup(s);
            if ((s = json_str(item, "windDirDay")))  d->wind_dir_day = strdup(s);
            if ((s = json_str(item, "windScaleDay")))d->wind_scale_day = strdup(s);
            if ((s = json_str(item, "sunrise")))     d->sunrise = strdup(s);
            if ((s = json_str(item, "sunset")))      d->sunset = strdup(s);
            if ((s = json_str(item, "sunrise")))     d->sunrise = strdup(s);
            if ((s = json_str(item, "sunset")))      d->sunset = strdup(s);

            d->temp_max  = json_num(item, "tempMax");
            d->temp_min  = json_num(item, "tempMin");
            d->humidity  = json_num(item, "humidity");
            d->precip    = json_num(item, "precip");
            d->uv_index  = json_num(item, "uvIndex");
        }
        forecast->day_count = n;
    }

    cJSON_Delete(root);
    return forecast;
}

weather_forecast_t* weather_get_forecast(weather_config_t *config, int days) {
    if (!config || config->type != WEATHER_HEFENG) {
        ESP_LOGE(TAG, "Forecast currently supports HEFENG only");
        return NULL;
    }
    if (!config->api_host || !config->api_key) {
        ESP_LOGE(TAG, "HEFENG API host or key is not configured");
        return NULL;
    }
    if (days < 1) days = 1;
    if (days > WEATHER_FORECAST_MAX_DAYS) days = WEATHER_FORECAST_MAX_DAYS;

    location_info_t *location_info = location_from_config(config);
    if (!location_info) return NULL;

    char *location_id = resolve_hefeng_location_id(config, location_info);
    if (!location_id) {
        ESP_LOGE(TAG, "Failed to get location ID for city: %s", location_info->city);
        location_info_free(location_info);
        return NULL;
    }

    char url[256];
    snprintf(url, sizeof(url), "https://%s/v7/weather/7d?location=%s&key=%s",
            config->api_host, location_id, config->api_key);
    free(location_id);

    char *response = weather_http_request(url);
    if (!response) {
        location_info_free(location_info);
        return NULL;
    }

    weather_forecast_t *forecast = parse_hefeng_forecast(response, days);
    free(response);

    if (!forecast) {
        location_info_free(location_info);
        return NULL;
    }
    forecast->location_info = location_info;
    return forecast;
}

// 天气现象 -> 图标
static const char* weather_icon_of(const char *text) {
    if (!text) return "☁️";
    if (strstr(text, "晴")) return "☀️";
    if (strstr(text, "雨")) return "🌧️";
    if (strstr(text, "雪")) return "❄️";
    if (strstr(text, "雷")) return "⛈️";
    if (strstr(text, "雾")) return "🌫️";
    if (strstr(text, "风")) return "🌬️";
    return "☁️";
}

void weather_print_forecast(const weather_forecast_t *forecast) {
    if (!forecast || forecast->day_count <= 0) return;

    if (forecast->location_info) {
        char city[64] = {0};
        if (forecast->location_info->province && forecast->location_info->city) {
            snprintf(city, sizeof(city), "%s·%s",
                    forecast->location_info->province, forecast->location_info->city);
        } else if (forecast->location_info->city) {
            snprintf(city, sizeof(city), "%s", forecast->location_info->city);
        }
        printf("\n  📅 未来 %d 天预报（%s）\n", forecast->day_count, city);
    } else {
        printf("\n  📅 未来 %d 天预报\n", forecast->day_count);
    }

    printf("  ─────────────────────────────────────────────\n");

    for (int i = 0; i < forecast->day_count; i++) {
        const weather_daily_t *d = &forecast->daily[i];

        // 日期只显示 月-日
        char date_short[6] = {0};
        if (d->fx_date && strlen(d->fx_date) >= 10) {
            snprintf(date_short, sizeof(date_short), "%s", d->fx_date + 5); // MM-DD
        }

        char temp_str[20] = {0};
        snprintf(temp_str, sizeof(temp_str), "%.0f~%.0f℃", d->temp_min, d->temp_max);

        char wind_str[32] = {0};
        if (d->wind_dir_day && d->wind_scale_day) {
            snprintf(wind_str, sizeof(wind_str), "%s %s级", d->wind_dir_day, d->wind_scale_day);
        } else if (d->wind_dir_day) {
            snprintf(wind_str, sizeof(wind_str), "%s", d->wind_dir_day);
        }

        printf("  %s %s %s  %s → %s   %-10s  %s\n",
               weather_icon_of(d->text_day),
               date_short,
               forecast_weekday(d->fx_date),
               d->text_day ? d->text_day : "-",
               d->text_night ? d->text_night : "-",
               temp_str,
               wind_str);
    }

    printf("  ─────────────────────────────────────────────\n");
    if (forecast->update_time) printf("  🕒 预报更新: %s\n", forecast->update_time);
    printf("\n");
}

void weather_forecast_free(weather_forecast_t *forecast) {
    if (!forecast) return;

    SAFE_FREE(forecast->update_time);
    for (int i = 0; i < WEATHER_FORECAST_MAX_DAYS; i++) {
        SAFE_FREE(forecast->daily[i].fx_date);
        SAFE_FREE(forecast->daily[i].text_day);
        SAFE_FREE(forecast->daily[i].text_night);
        SAFE_FREE(forecast->daily[i].wind_dir_day);
        SAFE_FREE(forecast->daily[i].wind_scale_day);
        SAFE_FREE(forecast->daily[i].sunrise);
        SAFE_FREE(forecast->daily[i].sunset);
        SAFE_FREE(forecast->daily[i].sunrise);
        SAFE_FREE(forecast->daily[i].sunset);
    }
    if (forecast->location_info) {
        location_info_free(forecast->location_info);
    }
    SAFE_FREE(forecast);
}

// ==================== 逐小时预报（/v7/weather/24h） ====================

// 解析和风 /v7/weather/24h 响应
static weather_hourly_forecast_t* parse_hefeng_hourly(const char *json, int hours) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return NULL;

    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (!code || !cJSON_IsString(code) || strcmp(code->valuestring, "200") != 0) {
        ESP_LOGE(TAG, "Hourly API error, code=%s",
                 (code && cJSON_IsString(code)) ? code->valuestring : "?");
        cJSON_Delete(root);
        return NULL;
    }

    weather_hourly_forecast_t *hourly = calloc(1, sizeof(weather_hourly_forecast_t));
    if (!hourly) {
        cJSON_Delete(root);
        return NULL;
    }

    const char *update = json_str(root, "updateTime");
    if (update) hourly->update_time = strdup(update);

    cJSON *arr = cJSON_GetObjectItem(root, "hourly");
    if (arr && cJSON_IsArray(arr)) {
        int n = cJSON_GetArraySize(arr);
        if (n > hours) n = hours;
        if (n > WEATHER_HOURLY_MAX_HOURS) n = WEATHER_HOURLY_MAX_HOURS;

        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(arr, i);
            if (!item) continue;
            weather_hourly_t *h = &hourly->hourly[i];

            const char *s;
            if ((s = json_str(item, "fxTime"))) h->fx_time = strdup(s);
            if ((s = json_str(item, "text")))   h->text = strdup(s);

            h->temp = json_num(item, "temp");
            h->pop  = json_num(item, "pop");
        }
        hourly->hour_count = n;
    }

    cJSON_Delete(root);
    return hourly;
}

weather_hourly_forecast_t* weather_get_hourly(weather_config_t *config, int hours) {
    if (!config || config->type != WEATHER_HEFENG) {
        ESP_LOGE(TAG, "Hourly currently supports HEFENG only");
        return NULL;
    }
    if (!config->api_host || !config->api_key) {
        ESP_LOGE(TAG, "HEFENG API host or key is not configured");
        return NULL;
    }
    if (hours < 1) hours = 1;
    if (hours > WEATHER_HOURLY_MAX_HOURS) hours = WEATHER_HOURLY_MAX_HOURS;

    location_info_t *location_info = location_from_config(config);
    if (!location_info) return NULL;

    char *location_id = resolve_hefeng_location_id(config, location_info);
    if (!location_id) {
        ESP_LOGE(TAG, "Failed to get location ID for city: %s", location_info->city);
        location_info_free(location_info);
        return NULL;
    }

    char url[256];
    snprintf(url, sizeof(url), "https://%s/v7/weather/24h?location=%s&key=%s",
            config->api_host, location_id, config->api_key);
    free(location_id);

    char *response = weather_http_request(url);
    if (!response) {
        location_info_free(location_info);
        return NULL;
    }

    weather_hourly_forecast_t *hourly = parse_hefeng_hourly(response, hours);
    free(response);

    if (!hourly) {
        location_info_free(location_info);
        return NULL;
    }
    hourly->location_info = location_info;
    return hourly;
}

void weather_print_hourly(const weather_hourly_forecast_t *hourly) {
    if (!hourly || hourly->hour_count <= 0) return;
    printf("\n  🕐 未来 %d 小时预报\n", hourly->hour_count);
    printf("  ─────────────────────────────────────────────\n");
    for (int i = 0; i < hourly->hour_count; i++) {
        const weather_hourly_t *h = &hourly->hourly[i];
        char hhmm[6] = {0};
        if (h->fx_time && strlen(h->fx_time) >= 16) {
            snprintf(hhmm, sizeof(hhmm), "%s", h->fx_time + 11); // HH:MM
        }
        printf("  %s  %-6s  %.0f℃  降水概率 %.0f%%\n",
               hhmm, h->text ? h->text : "-", h->temp, h->pop);
    }
    printf("  ─────────────────────────────────────────────\n");
}

void weather_hourly_free(weather_hourly_forecast_t *hourly) {
    if (!hourly) return;
    SAFE_FREE(hourly->update_time);
    for (int i = 0; i < WEATHER_HOURLY_MAX_HOURS; i++) {
        SAFE_FREE(hourly->hourly[i].fx_time);
        SAFE_FREE(hourly->hourly[i].text);
    }
    if (hourly->location_info) {
        location_info_free(hourly->location_info);
    }
    SAFE_FREE(hourly);
}
