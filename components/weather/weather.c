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
    if (result-