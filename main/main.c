#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"

#include "weather.h"
#include "app_config.h"
#include "ui_display.h"
#include "weather_ui.h"
#include "touch.h"
#include "settings.h"

static const char *TAG = "APP";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define BTN_PRESSED_BIT    BIT2
#define WIFI_MAX_RETRY     10
#define WEATHER_ALERT_REFRESH_MS (15 * 60 * 1000)

#define BOOT_BUTTON_NUM    GPIO_NUM_0   // 板载 BOOT 键，低电平按下

static EventGroupHandle_t s_wifi_event_group;
static EventGroupHandle_t s_app_event_group;
static int s_retry_num = 0;

// 当前页面：开机默认今日天气
static volatile int s_page = PAGE_TODAY;

// 缓存的天气数据（主线程独占访问）
static weather_info_t *s_now = NULL;
static weather_forecast_t *s_fc = NULL;
static weather_hourly_forecast_t *s_hy = NULL;
static weather_alerts_t *s_alerts = NULL;
static weather_air_quality_t *s_air_quality = NULL;

typedef struct {
    weather_info_t *now;
    weather_forecast_t *forecast;
    weather_hourly_forecast_t *hourly;
    weather_alerts_t *alerts;
    weather_air_quality_t *air_quality;
} weather_snapshot_t;

typedef struct {
    uint32_t generation;
    char city[64];
    char location_id[24];
    char province[64];
    char district[64];
} weather_fetch_job_t;

typedef struct {
    uint32_t generation;
    weather_snapshot_t snapshot;
} weather_fetch_result_t;

static QueueHandle_t s_fetch_jobs;
static QueueHandle_t s_fetch_results;
static bool s_fetch_inflight;
static bool s_fetch_pending;
static uint32_t s_city_generation;

// 息屏状态
static bool s_screen_on = true;
static int s_last_clock_minute = -1;

static void time_sync_event_handler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    const esp_netif_sntp_time_sync_t *sync = event_data;
    time_t synced = sync ? sync->tv.tv_sec : time(NULL);
    struct tm local;
    if (localtime_r(&synced, &local)) {
        ESP_LOGI("CLOCK", "SNTP 时间已同步: %04d-%02d-%02d %02d:%02d:%02d",
                 local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                 local.tm_hour, local.tm_min, local.tm_sec);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "WiFi 断开，重连中 (%d/%d)", s_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "获取到 IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// WiFi 网络列表：主网络失败后自动尝试备选
typedef struct { const char *ssid; const char *pass; } wifi_net_t;
static const wifi_net_t s_nets[] = {
    { APP_WIFI_SSID,  APP_WIFI_PASSWORD },
    { APP_WIFI_SSID2, APP_WIFI_PASSWORD2 },
};
#define WIFI_NET_COUNT  (sizeof(s_nets) / sizeof(s_nets[0]))

static esp_err_t wifi_connect_one(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    wifi_config_t config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_OPEN,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
            .pmf_cfg = { .capable = false, .required = false },
        },
    };
    strlcpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, password ? password : "", sizeof(config.sta.password));
    if (config.sta.password[0]) config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    // The STA_START handler begins connecting immediately. Stop the driver
    // before changing credentials so set_config cannot race an active join.
    esp_err_t stop_err = esp_wifi_stop();
    if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED) return stop_err;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    ESP_LOGI(TAG, "尝试连接 WiFi: %s", ssid);
    ESP_ERROR_CHECK(esp_wifi_start());
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(30000));
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

static esp_err_t wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (settings_wifi_ssid() && wifi_connect_one(settings_wifi_ssid(), settings_wifi_password()) == ESP_OK)
        return ESP_OK;
    for (int n = 0; n < (int)WIFI_NET_COUNT; n++) {
        if (wifi_connect_one(s_nets[n].ssid, s_nets[n].pass) == ESP_OK) return ESP_OK;
        ESP_LOGW(TAG, "WiFi %s 连接失败，%s", s_nets[n].ssid,
                 (n + 1 < (int)WIFI_NET_COUNT) ? "尝试备选网络" : "全部网络失败");
    }
    return ESP_FAIL;
}

static esp_err_t wifi_reconnect_preferred(void)
{
    if (settings_wifi_ssid() && wifi_connect_one(settings_wifi_ssid(), settings_wifi_password()) == ESP_OK)
        return ESP_OK;
    for (int i = 0; i < (int)WIFI_NET_COUNT; i++)
        if (wifi_connect_one(s_nets[i].ssid, s_nets[i].pass) == ESP_OK) return ESP_OK;
    return ESP_FAIL;
}

// Start SNTP after WiFi has an IP. Keep weather startup responsive when NTP
// is temporarily unreachable; the first valid sync will update the clock.
static void time_sync_start(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    config.wait_for_sync = false;
    esp_err_t event_err = esp_event_handler_register(NETIF_SNTP_EVENT, NETIF_SNTP_TIME_SYNC,
                                                     time_sync_event_handler, NULL);
    if (event_err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP 同步事件注册失败: %s", esp_err_to_name(event_err));
    }
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP 初始化失败: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "SNTP 已启动，等待联网校时");
}

static void wifi_scan_update_ui(void)
{
    wifi_scan_config_t scan = { .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    scan.scan_time.active.min = 100;
    scan.scan_time.active.max = 250;
    if (esp_wifi_scan_start(&scan, true) != ESP_OK) {
        ui_settings_wifi_set_scan_results(NULL, 0);
        return;
    }
    uint16_t count = 24;
    wifi_ap_record_t records[24] = {0};
    if (esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) count = 0;
    char ssids[24][33];
    const char *names[24];
    for (int i = 0; i < count; ++i) {
        size_t len = strnlen((const char *)records[i].ssid, sizeof(records[i].ssid));
        if (len > 32) len = 32;
        memcpy(ssids[i], records[i].ssid, len);
        ssids[i][len] = '\0';
        names[i] = ssids[i];
    }
    ui_settings_wifi_set_scan_results(names, count);
}

// BOOT 键中断：只置事件位，去抖在主循环处理
static void IRAM_ATTR btn_isr(void *arg)
{
    BaseType_t hp = pdFALSE;
    xEventGroupSetBitsFromISR(s_app_event_group, BTN_PRESSED_BIT, &hp);
    if (hp == pdTRUE) portYIELD_FROM_ISR();
}

static void button_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_NUM,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BOOT_BUTTON_NUM, btn_isr, NULL);
}

// 按当前页面渲染
static void render_current_page(void)
{
    if (ui_city_manager_open() && !ui_settings_picker_open()) {
        ui_render_city_manager();
        return;
    }
    switch (s_page) {
        case PAGE_TODAY:
            ui_render_today(s_now, s_fc, s_hy, s_alerts, s_air_quality);
            break;
        case PAGE_3DAY:
            ui_render_forecast(s_fc, 3);
            break;
        case PAGE_5DAY:
            ui_render_forecast(s_fc, 5);
            break;
        case PAGE_7DAY:
            ui_render_forecast(s_fc, 7);
            break;
        case PAGE_SETTINGS:
            ui_render_settings();
            break;
        default:
            s_page = PAGE_TODAY;
            ui_render_today(s_now, s_fc, s_hy, s_alerts, s_air_quality);
            break;
    }
}

static weather_snapshot_t fetch_weather_data(weather_config_t *config)
{
    weather_snapshot_t snapshot = {
        .now = weather_get(config),
        .forecast = weather_get_forecast(config, WEATHER_FORECAST_MAX_DAYS),
        .hourly = weather_get_hourly(config, 12),
        .alerts = weather_get_alerts(config),
        .air_quality = weather_get_air_quality(config),
    };

    if (snapshot.now) {
        weather_print_info(snapshot.now); // 串口同步输出，便于调试
    } else {
        ESP_LOGE(TAG, "天气获取失败");
    }
    if (snapshot.forecast) weather_print_forecast(snapshot.forecast);
    if (snapshot.hourly) weather_print_hourly(snapshot.hourly);
    return snapshot;
}

static void free_weather_snapshot(weather_snapshot_t *snapshot)
{
    if (!snapshot) return;
    if (snapshot->now) weather_info_free(snapshot->now);
    if (snapshot->forecast) weather_forecast_free(snapshot->forecast);
    if (snapshot->hourly) weather_hourly_free(snapshot->hourly);
    if (snapshot->alerts) weather_alerts_free(snapshot->alerts);
    if (snapshot->air_quality) weather_air_quality_free(snapshot->air_quality);
    memset(snapshot, 0, sizeof(*snapshot));
}

static void apply_weather_snapshot(weather_snapshot_t *snapshot)
{
    if (s_now) weather_info_free(s_now);
    if (s_fc) weather_forecast_free(s_fc);
    if (s_hy) weather_hourly_free(s_hy);
    if (s_alerts) weather_alerts_free(s_alerts);
    if (s_air_quality) weather_air_quality_free(s_air_quality);
    s_now = snapshot->now;
    s_fc = snapshot->forecast;
    s_hy = snapshot->hourly;
    s_alerts = snapshot->alerts;
    s_air_quality = snapshot->air_quality;
    memset(snapshot, 0, sizeof(*snapshot));
}

static void weather_fetch_worker(void *arg)
{
    (void)arg;
    weather_fetch_job_t job;
    while (xQueueReceive(s_fetch_jobs, &job, portMAX_DELAY) == pdTRUE) {
        weather_config_t config = {
            .api_key = APP_QWEATHER_KEY,
            .api_host = APP_QWEATHER_HOST,
            .city = job.city[0] ? job.city : NULL,
            .location_id = job.location_id[0] ? job.location_id : NULL,
            .province = job.province[0] ? job.province : NULL,
            .district = job.district[0] ? job.district : NULL,
            .type = WEATHER_HEFENG,
        };
        weather_fetch_result_t result = {
            .generation = job.generation,
            .snapshot = fetch_weather_data(&config),
        };
        xQueueSend(s_fetch_results, &result, portMAX_DELAY);
    }
}

static void request_city_weather(void)
{
    if (!s_fetch_jobs || s_fetch_inflight) return;
    weather_fetch_job_t job = { .generation = s_city_generation };
    const char *city = settings_city();
    const char *id = settings_location_id();
    const char *province = settings_province();
    const char *district = settings_district();
    strlcpy(job.city, city ? city : "", sizeof(job.city));
    strlcpy(job.location_id, id ? id : "", sizeof(job.location_id));
    strlcpy(job.province, province ? province : "", sizeof(job.province));
    strlcpy(job.district, district ? district : "", sizeof(job.district));
    if (xQueueSend(s_fetch_jobs, &job, 0) == pdTRUE) {
        s_fetch_inflight = true;
        s_fetch_pending = false;
        ESP_LOGI(TAG, "后台更新城市天气: %s%s%s", job.province,
                 job.province[0] && job.city[0] ? "·" : "", job.city);
    }
}

void app_main(void)
{
    // NVS 初始化（WiFi 驱动依赖）
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_app_event_group = xEventGroupCreate();

    // 加载设置（NVS），再点亮屏幕
    settings_load();

    // 先点亮屏幕（开屏页），再联网
    ESP_ERROR_CHECK(ui_display_init());
    lcd_backlight(g_settings.brightness);
    ui_render_splash("正在连接 WiFi...");

    button_init();
    if (!touch_init()) {
        ESP_LOGW(TAG, "触摸初始化失败，仅 BOOT 键可用");
    }

    if (wifi_init_sta() != ESP_OK) {
        ui_render_splash("WiFi 连接失败，请检查配置");
        return;
    }

    // 手动指定公共 DNS，避免部分运营商 DNS 解析 qweatherapi.com 失败
    dns_setserver(0, (const ip_addr_t *)&(ip_addr_t){ .u_addr.ip4 = { .addr = ipaddr_addr("223.5.5.5") }, .type = IPADDR_TYPE_V4 });
    dns_setserver(1, (const ip_addr_t *)&(ip_addr_t){ .u_addr.ip4 = { .addr = ipaddr_addr("119.29.29.29") }, .type = IPADDR_TYPE_V4 });

    time_sync_start();

    s_fetch_jobs = xQueueCreate(1, sizeof(weather_fetch_job_t));
    s_fetch_results = xQueueCreate(1, sizeof(weather_fetch_result_t));
    if (!s_fetch_jobs || !s_fetch_results) ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    BaseType_t worker_ok = xTaskCreate(weather_fetch_worker, "weather_fetch", 10240,
                                       NULL, 5, NULL);
    if (worker_ok != pdPASS) ESP_ERROR_CHECK(ESP_ERR_NO_MEM);

    ui_render_splash("正在获取天气...");

    weather_config_t config = {
        .api_key  = APP_QWEATHER_KEY,
        .api_host = APP_QWEATHER_HOST,
        .city     = NULL,      // 循环顶部按设置填充
        .location_id = NULL,
        .province = NULL,
        .district = NULL,
        .type     = WEATHER_HEFENG,
    };

    while (1) {
        // 0) 应用设置里的城市（空 = 自动定位）
        const char *sc = settings_city();
        config.city = (char *)((sc && strlen(sc) > 0) ? sc : NULL);   // 组件内只读使用
        config.location_id = (char *)settings_location_id();
        config.province = (char *)settings_province();
        config.district = (char *)settings_district();

        // 1) 拉数据
        weather_snapshot_t initial_snapshot = fetch_weather_data(&config);
        apply_weather_snapshot(&initial_snapshot);

        // 2) 渲染当前页面（开机默认 PAGE_TODAY；息屏时跳过）
        if (s_screen_on) {
            rende