// 天气页面渲染：简洁版 + 设置页
// 浅蓝色背景 + 白色文字，静态布局无动画（稳定优先）
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "weather_ui.h"
#include "ui_display.h"
#include "settings.h"
#include "region_data.h"

// ================= 配色（简单两色） =================
#define C_BG_BLUE   RGB(0x17, 0x3B, 0x67)
#define C_CARD      RGB(0x24, 0x5B, 0x87)
#define C_SEP       RGB(0x74, 0xB4, 0xDB)
#define C_WHITE     RGB(0xFF, 0xFF, 0xFF)
#define C_WHITE2    RGB(0xDC, 0xEC, 0xFA)   // 次要白

// ================= 小工具 =================
static const char *weekday_of(const char *fx_date)
{
    int y = 0, m = 0, d = 0;
    if (!fx_date || sscanf(fx_date, "%d-%d-%d", &y, &m, &d) != 3) return "";
    if (m < 3) { m += 12; y -= 1; }
    int k = y % 100, j = y / 100;
    int h = (d + 13 * (m + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
    static const char *names[] = {"周六", "周日", "周一", "周二", "周三", "周四", "周五"};
    return names[h % 7];
}

static bool hour_is_night(const char *iso)
{
    if (!iso || strlen(iso) < 13) return false;
    int h = (iso[11] - '0') * 10 + (iso[12] - '0');
    return (h < 6 || h >= 19);
}

static int parse_clock_minutes(const char *clock)
{
    int hour = 0, minute = 0;
    if (!clock || sscanf(clock, "%2d:%2d", &hour, &minute) != 2 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59) return -1;
    return hour * 60 + minute;
}

static bool minute_is_night(int minute, const weather_daily_t *today)
{
    int sunrise = today ? parse_clock_minutes(today->sunrise) : -1;
    int sunset = today ? parse_clock_minutes(today->sunset) : -1;
    if (sunrise >= 0 && sunset >= 0) return minute < sunrise || minute >= sunset;
    return minute < 360 || minute >= 1080;
}

static bool current_time_is_night(const weather_forecast_t *fc, const char *fallback_iso)
{
    time_t now = time(NULL);
    struct tm local_now;
    if (now >= 1577836800 && localtime_r(&now, &local_now))
        return minute_is_night(local_now.tm_hour * 60 + local_now.tm_min,
                               fc && fc->day_count > 0 ? &fc->daily[0] : NULL);
    return hour_is_night(fallback_iso);
}

static bool forecast_time_is_night(const char *iso, const weather_forecast_t *fc)
{
    if (!iso || strlen(iso) < 16) return false;
    int hour = 0, minute = 0;
    if (sscanf(iso + 11, "%2d:%2d", &hour, &minute) != 2) return false;
    return minute_is_night(hour * 60 + minute,
                           fc && fc->day_count > 0 ? &fc->daily[0] : NULL);
}

// 温度格式化（跟随设置的单位）
static void fmt_temp(char *buf, size_t n, float celsius)
{
    float t = g_settings.unit_f ? celsius * 9.0f / 5.0f + 32.0f : celsius;
    snprintf(buf, n, "%.0f°", t);
}

// Packed lunar month data (2019–2059); base date is 2019-02-05, lunar New Year.
// Each year encodes leap month, month lengths, and leap-month length.
static const uint32_t s_lunar_years[] = {
    0x0A930, 0x07954, 0x06AA0, 0x0AD50, 0x05B52, 0x04B60, 0x0A6E6, 0x0A4E0,
    0x0D260, 0x0EA65, 0x0D530, 0x05AA0, 0x076A3, 0x096D0, 0x04AFB, 0x04AD0,
    0x0A4D0, 0x1D0B6, 0x0D250, 0x0D520, 0x0DD45, 0x0B5A0, 0x056D0, 0x055B2,
    0x049B0, 0x0A577, 0x0A4B0, 0x0AA50, 0x1B255, 0x06D20, 0x0ADA0, 0x14B63,
    0x09370, 0x049F8, 0x04970, 0x064B0, 0x168A6, 0x0EA50, 0x06B20, 0x1A6C4,
    0x0AAE0,
};

static int civil_days(int year, int month, int day)
{
    year -= month <= 2;
    int era = (year >= 0 ? year : year - 399) / 400;
    unsigned yoe = (unsigned)(year - era * 400);
    unsigned mp = (unsigned)(month + (month > 2 ? -3 : 9));
    unsigned doy = (153 * mp + 2) / 5 + (unsigned)day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int)doe;
}

static int lunar_leap_month(int year) { return (int)(s_lunar_years[year - 2019] & 0x0F); }
static int lunar_month_days(int year, int month)
{
    return (s_lunar_years[year - 2019] & (0x10000u >> month)) ? 30 : 29;
}
static int lunar_year_days(int year)
{
    uint32_t code = s_lunar_years[year - 2019];
    int days = 348;
    for (uint32_t mask = 0x8000; mask > 0x8; mask >>= 1)
        if (code & mask) ++days;
    int leap = (int)(code & 0x0F);
    if (leap) days += (code & 0x10000) ? 30 : 29;
    return days;
}

static bool lunar_date(int year, int month, int day, int *lunar_year,
                       int *lunar_month, int *lunar_day, bool *is_leap)
{
    if (year < 2019 || year > 2059 || month < 1 || month > 12 || day < 1 || day > 31)
        return false;
    int offset = civil_days(year, month, day) - civil_days(2019, 2, 5);
    if (offset < 0) return false;
    int ly = 2019;
    while (ly <= 2059 && offset >= lunar_year_days(ly)) {
        offset -= lunar_year_days(ly++);
    }
    if (ly > 2059) return false;
    int leap_month = lunar_leap_month(ly);
    bool leap = false;
    int lm = 1;
    for (; lm <= 12; ++lm) {
        int days = lunar_month_days(ly, lm);
        if (offset < days) break;
        offset -= days;
        if (lm == leap_month) {
            int leap_days = (s_lunar_years[ly - 2019] & 0x10000) ? 30 : 29;
            if (offset < leap_days) { leap = true; break; }
            offset -= leap_days;
        }
    }
    if (lm > 12) return false;
    *lunar_year = ly;
    *lunar_month = lm;
    *lunar_day = offset + 1;
    *is_leap = leap;
    return true;
}

static void format_lunar(int year, int month, int day, char *out, size_t capacity,
                         const char **moon_phase)
{
    static const char *const months[] = {"正","二","三","四","五","六","七","八","九","十","冬","腊"};
    static const char *const small_days[] = {"初一","初二","初三","初四","初五","初六","初七","初八","初九","初十"};
    bool leap = false;
    int ly, lm, ld;
    if (!lunar_date(year, month, day, &ly, &lm, &ld, &leap)) {
        strlcpy(out, "农历日期不可用", capacity);
        *moon_phase = "月相未知";
        return;
    }
    char day_name[12];
    if (ld <= 10) strlcpy(day_name, small_days[ld - 1], sizeof(day_name));
    else if (ld < 20) snprintf(day_name, sizeof(day_name), "十%s", small_days[ld - 11] + 3);
    else if (ld == 20) strlcpy(day_name, "二十", sizeof(day_name));
    else if (ld < 30) snprintf(day_name, sizeof(day_name), "廿%s", small_days[ld - 21] + 3);
    else strlcpy(day_name, "三十", sizeof(day_name));
    snprintf(out, capacity, "农历%s%s月%s", leap ? "闰" : "", months[lm - 1], day_name);

    if (ld <= 2 || ld >= 29) *moon_phase = "新月";
    else if (ld <= 6) *moon_phase = "娥眉月";
    else if (ld <= 9) *moon_phase = "上弦月";
    else if (ld <= 13) *moon_phase = "盈凸月";
    else if (ld <= 16) *moon_phase = "满月";
    else if (ld <= 20) *moon_phase = "亏凸月";
    else if (ld <= 23) *moon_phase = "下弦月";
    else *moon_phase = "残月";
}

static bool parse_weather_datetime(const char *stamp, int *year, int *month, int *day, int *hour, int *minute)
{
    if (!stamp || strlen(stamp) < 16) return false;
    return sscanf(stamp, "%4d-%2d-%2dT%2d:%2d", year, month, day, hour, minute) == 5;
}

// 度数环（°）
static void draw_degree_ring(int cx, int cy, int r, uint16_t color)
{
    for (int a = 0; a < 16; a++) {
        float rad = a * 3.14159265f / 8;
        lcd_draw_pixel(cx + (int)(cosf(rad) * r), cy + (int)(sinf(rad) * r), color);
    }
}

static void fit_text_width(char *out, size_t capacity, const char *text, int max_width)
{
    if (!capacity) return;
    strlcpy(out, text ? text : "", capacity);
    bool truncated = false;
    while (out[0] && lcd_text_width(out, 1) > max_width) {
        size_t len = strlen(out);
        if (!len) break;
        --len;
        while (len && (((unsigned char)out[len] & 0xC0) == 0x80)) --len;
        out[len] = '\0';
        truncated = true;
    }
    if (truncated && capacity > 4 && max_width >= 16) {
        while (out[0] && lcd_text_width(out, 1) + 16 > max_width) {
            size_t len = strlen(out);
            --len;
            while (len && (((unsigned char)out[len] & 0xC0) == 0x80)) --len;
            out[len] = '\0';
        }
        strlcat(out, "…", capacity);
    }
}

static int small_text_width(const char *text)
{
    int width = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (p && *p) {
        if (*p < 0x80) { width += 8; ++p; }
        else {
            width += 12;
            ++p;
            while ((*p & 0xC0) == 0x80) ++p;
        }
    }
    return width;
}

static void fit_small_text_width(char *out, size_t capacity, const char *text, int max_width)
{
    if (!capacity) return;
    strlcpy(out, text ? text : "", capacity);
    bool truncated = false;
    while (out[0] && small_text_width(out) > max_width) {
        size_t len = strlen(out);
        --len;
        while (len && (((unsigned char)out[len] & 0xC0) == 0x80)) --len;
        out[len] = '\0';
        truncated = true;
    }
    if (truncated && capacity > 4 && max_width >= 12) {
        while (out[0] && small_text_width(out) + 12 > max_width) {
            size_t len = strlen(out);
            --len;
            while (len && (((unsigned char)out[len] & 0xC0) == 0x80)) --len;
            out[len] = '\0';
        }
        strlcat(out, "…", capacity);
    }
}

static uint16_t alert_badge_color(const weather_alert_item_t *alert)
{
    if (strcmp(alert->color, "red") == 0 || strcmp(alert->color, "purple") == 0 ||
        strcmp(alert->severity, "extreme") == 0 || strcmp(alert->severity, "severe") == 0)
        return RGB(0xA9, 0x31, 0x3B);
    if (strcmp(alert->color, "orange") == 0 || strcmp(alert->color, "amber") == 0 ||
        strcmp(alert->color, "yellow") == 0 || strcmp(alert->severity, "moderate") == 0)
        return RGB(0xA5, 0x64, 0x1B);
    if (strcmp(alert->color, "blue") == 0) return RGB(0x2D, 0x5D, 0x9C);
    return RGB(0x2D, 0x5A, 0x61);
}

static uint16_t air_quality_color(const char *category)
{
    if (!category || !category[0]) return C_WHITE2;
    if (strstr(category, "优")) return RGB(0x76, 0xE3, 0xA2);
    if (strstr(category, "良")) return RGB(0xFF, 0xD7, 0x7A);
    if (strstr(category, "轻")) return RGB(0xFF, 0xA0, 0x66);
    if (strstr(category, "中")) return RGB(0xFF, 0x70, 0x70);
    return RGB(0xD8, 0x8B, 0xE8);
}

// 小三角箭头
static int draw_arrow_up(int x, int y, uint16_t color)
{
    for (int j = 0; j < 5; j++) lcd_fill_rect(x + 2 - j, y - 2 + j, 2 * j + 1, 1, color);
    return 6;
}

static int draw_arrow_down(int x, int y, uint16_t color)
{
    for (int j = 0; j < 5; j++) lcd_fill_rect(x + j, y - 2 + j, 5 - 2 * j + 1, 1, color);
    return 6;
}

static void draw_page_dots(int active)
{
    const int cx = LCD_H_RES / 2, gap = 15, y = 232;
    for (int i = 0; i < PAGE_MAX; ++i) {
        int x = cx + (i - 2) * gap;
        if (i == active) lcd_fill_circle(x, y, 4, C_WHITE);
        else lcd_blend_circle(x, y, 2, C_WHITE, 125);
    }
}

// ================= 开屏页 =================
void ui_render_splash(const char *msg)
{
    lcd_clear(C_BG_BLUE);
    const char *title = "天气站";
    int w = lcd_text_width(title, 2);
    lcd_draw_text((LCD_H_RES - w) / 2, 96, title, C_WHITE, 2);
    if (msg) {
        int mw = lcd_text_width(msg, 1);
        lcd_draw_text((LCD_H_RES - mw) / 2, 140, msg, C_WHITE2, 1);
    }
    lcd_flush();
}

// ================= 今日天气页 =================
void ui_render_today(const weather_info_t *now, const weather_forecast_t *fc,
                     const weather_hourly_forecast_t *hy, const weather_alerts_t *alerts,
                     const weather_air_quality_t *air_quality)
{
    // Use the live SNTP clock rather than the API's last-update timestamp;
    // the latter can be many minutes old around sunset.
    bool night = now && current_time_is_night(fc, now->update_time);
    uint16_t top = night ? RGB(0x09,0x16,0x31) : RGB(0x13,0x4D,0x78);
    uint16_t mid = night ? RGB(0x15,0x2C,0x50) : RGB(0x21,0x7A,0xA7);
    uint16_t bot = night ? RGB(0x0D,0x24,0x43) : RGB(0x12,0x4A,0x76);
    lcd_gradient_v3(0, LCD_V_RES, 104, top, mid, bot);

    // Header: two compact date rows on the left; province and city are stacked
    // at the far right, with a reserved gutter between the two text columns.
    const location_info_t *loc = now ? now->location_info : NULL;
    const char *province = loc && loc->province && loc->province[0] ? loc->province : "省份待定位";
    const char *city = loc && loc->city && loc->city[0] ? loc->city : "城市待定位";
    int year = 0, month = 0, day = 0, hour = 0, minute = 0;
    bool have_datetime = false;
    time_t clock_now = time(NULL);
    if (clock_now >= 1577836800) {
        struct tm local_now;
        if (localtime_r(&clock_now, &local_now)) {
            year = local_now.tm_year + 1900;
            month = local_now.tm_mon + 1;
            day = local_now.tm_mday;
            hour = local_now.tm_hour;
            minute = local_now.tm_min;
            have_datetime = true;
        }
    }
    if (!have_datetime)
        have_datetime = parse_weather_datetime(now ? now->update_time : NULL,
                                               &year, &month, &day, &hour, &minute);
    if (!have_datetime && fc && fc->day_count > 0 && fc->daily[0].fx_date &&
        sscanf(fc->daily[0].fx_date, "%4d-%2d-%2d", &year, &month, &day) == 3) {
        hour = minute = 0;
        have_datetime = true;
    }
    char solar_line[32], lunar_line[64], lunar_raw[64], province_line[96], city_line[96];
    const char *phase = "月相未知";
    if (have_datetime) {
        snprintf(solar_line, sizeof(solar_line), "%04d年%02d月%02d日", year, month, day);
        char lunar_date[48];
        format_lunar(year, month, day, lunar_date, sizeof(lunar_date), &phase);
        snprintf(lunar_raw, sizeof(lunar_raw), "%02d:%02d  %s", hour, minute, lunar_date);
    } else {
        strlcpy(solar_line, "等待网络校时", sizeof(solar_line));
        strlcpy(lunar_raw, "农历日期不可用", sizeof(lunar_raw));
    }
    fit_text_width(lunar_line, sizeof(lunar_line), lunar_raw, 196);
    lcd_draw_shadow_text(12, 5, solar_line, C_WHITE, 1);
    lcd_draw_text(12, 22, lunar_line, C_WHITE2, 1);
    fit_text_width(province_line, sizeof(province_line), province, 88);
    fit_text_width(city_line, sizeof(city_line), city, 88);
    lcd_draw_text(LCD_H_RES - 12 - lcd_text_width(province_line, 1), 5, province_line, C_WHITE, 1);
    lcd_draw_text(LCD_H_RES - 12 - lcd_text_width(city_line, 1), 22, city_line, C_WHITE, 1);
    lcd_fill_rect(12, 46, 296, 1, C_SEP);

    if (!now) {
        int w = lcd_text_width("正在获取天气...", 1);
        lcd_draw_text((LCD_H_RES - w) / 2, 95, "正在获取天气...", C_WHITE, 1);
        draw_page_dots(PAGE_TODAY);
        lcd_flush();
        return;
    }

    lcd_blend_circle(270, 77, 29, night ? RGB(0x62,0x91,0xD2) : RGB(0xFF,0xD1,0x70), 42);

    // Hero weather: large temperature and a larger, high-contrast vector icon.
    char big[12];
    float tcur = g_settings.unit_f ? now->temperature * 9.0f / 5.0f + 32.0f : now->temperature;
    snprintf(big, sizeof(big), "%.0f", tcur);
    int tw = lcd_draw_big_smooth(14, 55, big, C_WHITE);
    draw_degree_ring(14 + tw + 8, 63, 5, C_WHITE);
    int weather_x = 14 + tw + 25;
    const int metrics_x = 144;
    const weather_alert_item_t *active_alert =
        (alerts && alerts->available && alerts->count > 0) ? &alerts->items[0] : NULL;
    char metric_raw[64], metric_line[64];
    if (now->weather) {
        char weather_label[64];
        // Reserve a separate vertical column for humidity, wind and AQI.
        int max_weather_w = metrics_x - weather_x - 8;
        if (max_weather_w < 0) max_weather_w = 0;
        fit_text_width(weather_label, sizeof(weather_label), now->weather, max_weather_w);
        lcd_draw_text(weather_x, 66, weather_label, C_WHITE, 1);
    }
    if (active_alert) {
        const char *event = active_alert->event[0] ? active_alert->event : "恶劣天气";
        fit_small_text_width(metric_line, sizeof(metric_line), event,
                             metrics_x - weather_x - 8);
        lcd_draw_text_small(weather_x, 84, metric_line, alert_badge_color(active_alert));
    }

    // Compact live conditions sit to the right of the weather label, stacked
    // in a 92px column that ends before the hero icon begins.
    snprintf(metric_raw, sizeof(metric_raw), "湿度 %.0f%%", now->humidity);
    fit_small_text_width(metric_line, sizeof(metric_line), metric_raw, 92);
    lcd_draw_text_small(metrics_x, 50, metric_line, C_WHITE2);

    if (now->wind_dir && now->wind_dir[0])
        snprintf(metric_raw, sizeof(metric_raw), "%s %.0fkm/h", now->wind_dir, now->wind_speed);
    else
        snprintf(metric_raw, sizeof(metric_raw), "风速 %.0fkm/h", now->wind_speed);
    fit_small_text_width(metric_line, sizeof(metric_line), metric_raw, 92);
    lcd_draw_text_small(metrics_x, 64, metric_line, C_WHITE2);

    if (air_quality && air_quality->available) {
        snprintf(metric_raw, sizeof(metric_raw), "AQI %s %s",
                 air_quality->aqi[0] ? air_quality->aqi : "--",
                 air_quality->category[0] ? air_quality->category : "");
        fit_small_text_width(metric_line, sizeof(metric_line), metric_raw, 92);
        lcd_draw_text_small(metrics_x, 78, metric_line, air_quality_color(air_quality->category));
    } else {
        lcd_draw_text_small(metrics_x, 78, "AQI --", C_WHITE2);
    }
    snprintf(metric_raw, sizeof(metric_raw), "气压 %.0fhPa", now->pressure);
    fit_small_text_width(metric_line, sizeof(metric_line), metric_raw, 92);
    lcd_draw_text_small(metrics_x, 92, metric_line, C_WHITE2);
    lcd_draw_weather_icon(270, 77, 52, now->weather, night, mid);

    // High/low and feels-like temperature stay readable under the hero temperature.
    float t_max = (fc && fc->day_count > 0) ? fc->daily[0].temp_max : 20;
    float t_min = (fc && fc->day_count > 0) ? fc->daily[0].temp_min : 10;
    int x = 14;
    x += draw_arrow_up(x, 111, RGB(0xFF,0xD7,0x7A));
    {
        char tmax[12];
        fmt_temp(tmax, sizeof(tmax), t_max);
        x += lcd_draw_text(x, 103, tmax, C_WHITE, 1);
    }
    x += lcd_draw_text(x + 2, 90, " · ", C_WHITE2, 1);
    x += draw_arrow_down(x, 111, RGB(0x82,0xD7,0xFF));
    {
        char tmin[12];
        fmt_temp(tmin, sizeof(tmin), t_min);
        x += lcd_draw_text(x, 103, tmin, C_WHITE, 1);
    }
    {
        char feels[32];
        char ft[12];
        fmt_temp(ft, sizeof(ft), now->feels_like);
        snprintf(feels, sizeof(feels), "体感 %s", ft);
        lcd_draw_text_small(168, 104, feels, C_WHITE2);
    }

    // 12-hour curve fills the space freed by the removed today/tomorrow rows.
    const int hy0 = 118, hx = 8, hw = 304, hh = 108;
    lcd_blend_round_rect(hx, hy0, hw, hh, 13, RGB(0x14,0x3E,0x62), 225);

    if (!hy || hy->hour_count <= 0) {
        int w = lcd_text_width("暂无小时预报", 1);
        lcd_draw_text(hx + (hw - w) / 2, hy0 + 37, "暂无小时预报", C_WHITE2, 1);
    } else {
        int n = hy->hour_count < 12 ? hy->hour_count : 12;
        float tmin = 1e9f, tmax = -1e9f;
        for (int i = 0; i < n; i++) {
            if (hy->hourly[i].temp < tmin) tmin = hy->hourly[i].temp;
            if (hy->hourly[i].temp > tmax) tmax = hy->hourly[i].temp;
        }
        float range = tmax - tmin;
        if (range < 1.0f) range = 1.0f;

        int curve_y0 = hy0 + 70, curve_y1 = hy0 + 98;
        for (int gy = 0; gy < 3; ++gy)
            lcd_blend_rect(hx + 10, curve_y0 + gy * 14, hw - 20, 1, C_WHITE, 35);
        int prev_px = 0, prev_py = 0;

        for (int i = 0; i < n; i++) {
            const weather_hourly_t *h = &hy->hourly[i];
            int cx = n > 1 ? hx + 18 + i * (hw - 36) / (n - 1) : hx + hw / 2;

            if ((i % 3) == 0) {
                char hhmm[6] = {0};
                if (h->fx_time && strlen(h->fx_time) >= 16) snprintf(hhmm, sizeof(hhmm), "%.5s", h->fx_time + 11);
                int lw = (int)strlen(hhmm) * 8;
                int label_x = cx - lw / 2;
                if (label_x < hx + 10) label_x = hx + 10;
                if (label_x + lw > hx + hw - 10) label_x = hx + hw - 10 - lw;
                lcd_draw_text_compact(label_x, hy0 + 4, hhmm, C_WHITE2);
            }

            lcd_draw_weather_icon(cx, hy0 + 27, 17, h->text,
                                 forecast_time_is_night(h->fx_time, fc), RGB(0x14,0x3E,0x62));

            int temp = (int)lroundf(g_settings.unit_f ? h->temp * 9.0f / 5.0f + 32.0f : h->temp);
            char tstr[8]; snprintf(tstr, sizeof(tstr), "%d", temp);
            int twd = (int)strlen(tstr) * 8;
            lcd_draw_text_compact(cx - twd / 2, hy0 + 45, tstr, C_WHITE);

            int py = curve_y1 - (int)((h->temp - tmin) / range * (curve_y1 - curve_y0));
            if (i > 0) {
                lcd_draw_line(prev_px, prev_py + 1, cx, py + 1, RGB(0x52,0xC9,0xF4));
                lcd_draw_line(prev_px, prev_py, cx, py, C_WHITE);
            }
            prev_px = cx; prev_py = py;
        }
        for (int i = 0; i < n; i++) {
            const weather_hourly_t *h = &hy->hourly[i];
            int cx = n > 1 ? hx + 18 + i * (hw - 36) / (n - 1) : hx + hw / 2;
            int py = curve_y1 - (int)((h->temp - tmin) / range * (curve_y1 - curve_y0));
            lcd_fill_circle(cx, py, 3, RGB(0x52,0xC9,0xF4));
            lcd_fill_circle(cx, py, 2, C_WHITE);
        }
    }

    draw_page_dots(PAGE_TODAY);

    lcd_flush();
}

// ================= N 天预报页 =================
void ui_render_forecast(const weather_forecast_t *fc, int days)
{
    lcd_gradient_v3(0, LCD_V_RES, 100, RGB(0x12,0x2B,0x50), RGB(0x1C,0x68,0x91), RGB(0x0F,0x2E,0x55));

    char title[32];
    snprintf(title, sizeof(title), "未来 %d 天预报", days);
    lcd_draw_shadow_text(12, 7, title, C_WHITE, 1);

    if (fc && fc->location_info && fc->location_info->city) {
        int title_w = lcd_text_width(title, 1);
        int max_city_w = LCD_H_RES - 24 - title_w - 12;
        if (max_city_w < 0) max_city_w = 0;
        char city_label[96];
        fit_text_width(city_label, sizeof(city_label), fc->location_info->city, max_city_w);
        int w = lcd_text_width(city_label, 1);
        lcd_draw_text(LCD_H_RES - w - 12, 14, city_label, C_WHITE2, 1);
    }

    if (!fc || fc->day_count <= 0) {
        int w = lcd_text_width("无预报数据", 1);
        lcd_draw_text((LCD_H_RES - w) / 2, 120, "无预报数据", C_WHITE, 1);
        draw_page_dots(days == 3 ? PAGE_3DAY : days == 5 ? PAGE_5DAY : PAGE_7DAY);
        lcd_flush();
        return;
    }

    int show = days < fc->day_count ? days : fc->day_count;
    int top = 38, bottom = 222;
    int row_h = (bottom - top) / show;

    lcd_blend_round_rect(8, top - 2, 304, row_h * show + 4, 12, C_CARD, 195);

    for (int i = 0; i < show; i++) {
        const weather_daily_t *d = &fc->daily[i];
        int y = top + i * row_h;
        int cy = y + row_h / 2;

        if (i > 0) lcd_fill_rect(20, y, 280, 1, C_SEP);

        // 日期 + 星期（七行统一格式：周X MM-DD）
        char dstr[24];
        snprintf(dstr, sizeof(dstr), "%s %s",
                 d->fx_date ? weekday_of(d->fx_date) : "",
                 d->fx_date ? d->fx_date + 5 : "");
        lcd_draw_text(20, cy - 8, dstr, C_WHITE, 1);

        char tmax[12], tmin[12];
        fmt_temp(tmax, sizeof(tmax), d->temp_max);
        fmt_temp(tmin, sizeof(tmin), d->temp_min);
        int wmin = lcd_text_width(tmin, 1);
        int wmax = lcd_text_width(tmax, 1);
        int temp_min_x = 304 - wmin;
        int temp_max_x = temp_min_x - 8 - wmax;
        int icon_sz = (row_h >= 44) ? 30 : (row_h >= 28 ? 22 : 16);
        int icon_x = row_h < 28 ? 132 : 150;
        int icon_gap = row_h < 28 ? 7 : 6;
        lcd_draw_weather_icon(icon_x, cy, icon_sz, d->text_day, false, C_CARD);
        lcd_draw_weather_icon(icon_x + icon_sz + icon_gap, cy, icon_sz, d->text_night, true, C_CARD);

        int text_x = row_h < 28 ? icon_x + 2 * icon_sz + icon_gap + 3 : icon_x + 2 * icon_sz + 18;
        int label_width = temp_max_x - 6 - text_x;
        if (d->text_day && label_width >= 16) {
            char weather_label[48];
            fit_text_width(weather_label, sizeof(weather_label), d->text_day, label_width);
            lcd_draw_text(text_x, cy - 8, weather_label, C_WHITE, 1);
        }
        lcd_draw_text(304 - wmin, cy - 8, tmin, C_WHITE2, 1);
        lcd_draw_text(temp_max_x, cy - 8, tmax, C_WHITE, 1);

        if (row_h >= 44 && d->wind_dir_day) {
            char wstr[32];
            snprintf(wstr, sizeof(wstr), "%s %s级", d->wind_dir_day, d->wind_scale_day ? d->wind_scale_day : "");
            lcd_draw_text(20, cy + 9, wstr, C_WHITE2, 1);
        }
    }

    draw_page_dots(days == 3 ? PAGE_3DAY : days == 5 ? PAGE_5DAY : PAGE_7DAY);
    lcd_flush();
}

// ================= 设置与地区选择 =================
#define SET_CITY_Y       45
#define SET_BRIGHT_Y     98
#define SET_LIST_Y       164
#define PICK_ROWS        5
#define PICK_ROW_H       30
#define PICK_LIST_Y      62

typedef enum { PICK_PROVINCE, PICK_CITY, PICK_DISTRICT } picker_level_t;
static bool s_picker_open;
static bool s_wifi_open, s_wifi_upper, s_wifi_symbols, s_wifi_notice;
static bool s_wifi_show_scan;
static int s_wifi_field;
static char s_wifi_ssid[SETTINGS_WIFI_SSID_LEN];
static char s_wifi_password[SETTINGS_WIFI_PASSWORD_LEN];
static char s_wifi_scan_ssids[24][33];
static int s_wifi_scan_count, s_wifi_scan_scroll;
static picker_level_t s_picker_level;
static int s_pick_province, s_pick_city, s_pick_scroll;
static bool s_slider_dirty;
static bool s_city_manager, s_picker_add_mode;
static int s_city_scroll;

static const char *selected_region(void)
{
    if (settings_location_id()) {
        static char label[160];
        snprintf(label, sizeof(label), "%s · %s", g_settings.city, g_settings.district);
        return label;
    }
    return "自动定位";
}

static void draw_setting_background(void)
{
    lcd_gradient_v3(0, LCD_V_RES, 74, RGB(0x10,0x2B,0x50), RGB(0x12,0x55,0x83), RGB(0x0D,0x2C,0x52));
    lcd_blend_circle(286, 22, 48, RGB(0x38,0xA8,0xDC), 55);
}

static void draw_brightness_slider(void)
{
    const int x0 = 115, x1 = 285, y = SET_BRIGHT_Y + 35;
    lcd_fill_round_rect(x0, y - 3, x1 - x0, 6, 3, RGB(0x78,0xA9,0xC6));
    int px = x0 + (x1 - x0) * g_settings.brightness / 100;
    if (px > x0) lcd_fill_round_rect(x0, y - 3, px - x0, 6, 3, RGB(0x78,0xD7,0xFF));
    lcd_fill_circle(px, y, 8, C_WHITE);
    lcd_fill_circle(px, y, 4, RGB(0x44,0xB8,0xEC));
}

bool ui_render_settings(void)
{
    if (s_wifi_open) {
        draw_setting_background();
        lcd_draw_text(10, 3, "Wi-Fi 设置", C_WHITE, 2);
        if (s_wifi_show_scan) {
            lcd_draw_text(228, 11, "键盘", C_WHITE2, 1);
            lcd_draw_text(276, 11, "返回", C_WHITE2, 1);
            lcd_blend_round_rect(8, 37, 304, 181, 10, RGB(0x10,0x3E,0x68), 215);
            if (!s_wifi_scan_count) {
                lcd_draw_text(88, 115, "未找到热点", C_WHITE2, 1);
            } else {
                int first = s_wifi_scan_scroll;
                if (first < 0) first = 0;
                if (first > s_wifi_scan_count - 7) first = s_wifi_scan_count > 7 ? s_wifi_scan_count - 7 : 0;
                s_wifi_scan_scroll = first;
                for (int i = 0; i < 7 && first + i < s_wifi_scan_count; ++i) {
                    int y = 43 + i * 25;
                    if (i) lcd_blend_rect(20, y, 280, 1, C_WHITE, 30);
                    char ssid_label[40];
                    fit_text_width(ssid_label, sizeof(ssid_label), s_wifi_scan_ssids[first + i], 246);
                    lcd_draw_text(22, y + 4, ssid_label, C_WHITE, 1);
                    lcd_draw_text(282, y + 4, ">", C_WHITE2, 1);
                }
            }
            lcd_draw_text_small(104, 225, "点选网络 · 上下滑动浏览", C_WHITE2);
            lcd_flush();
            return false;
        }
        lcd_draw_text(228, 11, "扫描", C_WHITE2, 1);
        lcd_draw_text(276, 11, "返回", C_WHITE2, 1);
        lcd_blend_round_rect(6, 32, 308, 29, 7, RGB(0x10,0x3E,0x68), 220);
        lcd_blend_round_rect(6, 64, 308, 29, 7, RGB(0x10,0x3E,0x68), 220);
        lcd_draw_text(12, 38, "SSID", C_WHITE2, 1);
        lcd_draw_text(12, 70, "密码", C_WHITE2, 1);
        lcd_fill_round_rect(62, 33 + s_wifi_field * 32, 2, 27, 1, RGB(0x8A,0xE2,0xFF));
        char show_ssid[SETTINGS_WIFI_SSID_LEN + 1];
        fit_text_width(show_ssid, sizeof(show_ssid), s_wifi_ssid, 232);
        lcd_draw_text(70, 38, show_ssid, C_WHITE, 1);
        int plen = (int)strlen(s_wifi_password);
        if (plen > 20) plen = 20;
        char masked[24]; memset(masked, '*', (size_t)plen); masked[plen] = '\0';
        lcd_draw_text(70, 70, masked, C_WHITE, 1);
        if (s_wifi_notice) lcd_draw_text_small(204, 72, "请输入名称", C_WARM);

        static const char *const lower[] = { "1234567890", "qwertyuiop", "asdfghjkl;", "zxcvbnm,./" };
        static const char *const upper[] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL:", "ZXCVBNM<>?" };
        static const char *const symbols[] = { "!@#$%^&*()", "-_+=[]{}\\", ":;\"'<>?~`", "|,./\\\\%^" };
        const char *const *layout = s_wifi_symbols ? symbols : (s_wifi_upper ? upper : lower);
        for (int row = 0; row < 4; ++row) {
            int y = 99 + row * 25;
            for (int col = 0; col < 10; ++col) {
                int x = 5 + col * 31;
                lcd_blend_round_rect(x, y, 28, 22, 4, RGB(0x2D,0x6A,0x91), 220);
                char key[2] = { layout[row][col], 0 };
                int kw = lcd_text_width(key, 1);
                lcd_draw_text(x + (28 - kw) / 2, y + 3, key, C_WHITE, 1);
            }
        }
        static const char *actions[] = { "大小", "符号", "删除", "空格", "下项", "保存", "取消" };
        for (int i = 0; i < 7; ++i) {
            int x = 4 + i * 44;
            lcd_blend_round_rect(x, 198, 41, 27, 5, i == 5 ? RGB(0x17,0x8B,0xB9) : RGB(0x2D,0x6A,0x91), 230);
            lcd_draw_text(x + 4, 203, actions[i], C_WHITE, 1);
        }
        lcd_draw_text_small(52, 227, "可扫描中文网络，键盘输入密码", C_WHITE2);
        lcd_flush();
        return false;
    }
    if (s_picker_open) {
        draw_setting_background();
        lcd_draw_shadow_text(14, 8, s_picker_add_mode ? "添加城市" : "选择地区", C_WHITE, 1);
        const char *crumb = s_picker_level == PICK_PROVINCE ? "中国大陆" :
                            s_picker_level == PICK_CITY ? region_province_name(s_pick_province) :
                            region_city_name(s_pick_province, s_pick_city);
        lcd_draw_text(16, 37, crumb ? crumb : "", C_WHITE2, 1);
        lcd_blend_round_rect(250, 5, 60, 27, 8, RGB(0x2D,0x6A,0x91), 230);
        lcd_draw_text(263, 10, "返回", C_WHITE, 1);
        lcd_blend_round_rect(9, 55, 302, 165, 12, RGB(0x10,0x3E,0x68), 205);
        int count = s_picker_level == PICK_PROVINCE ? region_province_count() + 1 :
                    s_picker_level == PICK_CITY ? region_city_count(s_pick_province) :
                    region_district_count(s_pick_province, s_pick_city);
        int first = s_pick_scroll;
        if (first < 0) first = 0;
        if (first > count - PICK_ROWS) first = count > PICK_ROWS ? count - PICK_ROWS : 0;
        s_pick_scroll = first;
        for (int i = 0; i < PICK_ROWS && first + i < count; ++i) {
            int index = first + i, y = PICK_LIST_Y + i * PICK_ROW_H;
            if (i) lcd_blend_rect(20, y, 280, 1, C_WHITE, 30);
            const char *name = s_picker_level == PICK_PROVINCE ? (index == 0 ? "自动定位" : region_province_name(index - 1)) :
                               s_picker_level == PICK_CITY ? region_city_name(s_pick_province, index) :
                               region_district_name(s_pick_province, s_pick_city, index);
            char name_label[96];
            fit_text_width(name_label, sizeof(name_label), name ? name : "", 246);
            lcd_draw_text(22, y + 6, name_label, C_WHITE, 1);
            lcd_draw_text(282, y + 6, ">", C_WHITE2, 1);
        }
        lcd_draw_text_small(96, 225, "上下滑动浏览 · 点击选择", C_WHITE2);
        lcd_flush();
        return false;
    }

    draw_setting_background();
    lcd_draw_text(14, 6, "设置", C_WHITE, 2);
    // Small Latin glyphs keep the brand mark within the 320px display.
    lcd_draw_text_small(258, 16, "WEATHER", C_WHITE2);

    // 地区卡：单击进入逐级选择，不再循环城市
    lcd_blend_round_rect(10, SET_CITY_Y, 300, 46, 12, RGB(0x1A,0x4E,0x78), 220);
    lcd_draw_text(22, SET_CITY_Y + 7, "位置", C_WHITE2, 1);
    char selected_label[160];
    fit_text_width(selected_label, sizeof(selected_label), selected_region(), 246);
    lcd_draw_text(22, SET_CITY_Y + 24, selected_label, C_WHITE, 1);
    lcd_draw_text(278, SET_CITY_Y + 16, ">", C_WHITE, 1);

    // 亮度卡：拖动滑杆实时预览，松手后保存
    lcd_blend_round_rect(10, SET_BRIGHT_Y, 300, 52, 12, RGB(0x1A,0x4E,0x78), 220);
    lcd_draw_text(22, SET_BRIGHT_Y + 7, "亮度", C_WHITE, 1);
    char val[12]; snprintf(val, sizeof(val), "%d%%", g_settings.brightness);
    lcd_draw_text(73, SET_BRIGHT_Y + 7, val, C_WHITE2, 1);
    draw_brightness_slider();

    const char *labels[] = { "Wi-Fi", "刷新", "温度", "息屏" };
    char values[4][20];
    snprintf(values[0], sizeof(values[0]), "%s", g_settings.wifi_ssid[0] ? "已设置" : "设置");
    snprintf(values[1], sizeof(values[1]), "%d分钟", g_settings.refresh_min);
    snprintf(values[2], sizeof(values[2]), "%s", g_settings.unit_f ? "°F" : "°C");
    snprintf(values[3], sizeof(values[3]), "%s", g_settings.sleep_sec ? (g_settings.sleep_sec == 30 ? "30秒" : "60秒") : "常亮");
    for (int i = 0; i < 4; ++i) {
        int x = 8 + i * 77;
        lcd_blend_round_rect(x, SET_LIST_Y, 72, 42, 9, RGB(0x1A,0x4E,0x78), 220);
        lcd_draw_text(x + 7, SET_LIST_Y + 5, labels[i], C_WHITE2, 1);
        lcd_draw_text(x + 7, SET_LIST_Y + 22, values[i], C_WHITE, 1);
    }
    lcd_draw_text_small(137, 216, "v1.2.2 · 点击设置", C_WHITE2);
    draw_page_dots(PAGE_SETTINGS);
    lcd_flush();
    return false;
}

bool ui_city_manager_open(void) { return s_city_manager; }
void ui_city_manager_show(void) { s_city_manager = true; s_city_scroll = 0; }
void ui_city_manager_cancel(void) { s_city_manager = false; s_picker_add_mode = false; s_picker_open = false; }
bool ui_city_manager_scroll(int direction)
{
    if (!s_city_manager || !direction) return false;
    s_city_scroll += direction * 4;
    int max = settings_city_count() > 6 ? settings_city_count() - 6 : 0;
    if (s_city_scroll < 0) s_city_scroll = 0;
    if (s_city_scroll > max) s_city_scroll = max;
    return true;
}
bool ui_city_manager_delete_at(int x, int y)
{
    (void)x;
    if (!s_city_manager || y < 40 || y >= 202) return false;
    int index = s_city_scroll + (y - 40) / 27;
    if (index < 0 || index >= settings_city_count()) return false;
    settings_remove_city(index);
    return true;
}
void ui_render_city_manager(void)
{
    draw_setting_background();
    char title[32];
    snprintf(title, sizeof(title), "城市管理 %d/10", settings_city_count());
    // Keep the title at the font's native pixel size; scaling the bitmap made
    // the Chinese strokes look blocky on the 320x240 panel.
    lcd_draw_shadow_text(12, 8, title, C_WHITE, 1);
    lcd_draw_text_small(275, 15, "完成", C_WHITE2);
    int count = settings_city_count();
    int first = s_city_scroll;
    if (first > count - 6) first = count > 6 ? count - 6 : 0;
    for (int row = 0; row < 6 && first + row < count; ++row) {
        int index = first + row, y = 40 + row * 27;
        const saved_city_t *c = settings_city_at(index);
        bool active = index == settings_active_city();
        lcd_blend_round_rect(8, y, 304, 24, 7, active ? RGB(0x1A,0x6F,0x9D) : RGB(0x10,0x3E,0x68), 220);
        char location[160], name[160];
        if (c && c->location_id[0])
            snprintf(location, sizeof(location), "%s · %s", c->province, c->city);
        else
            strlcpy(location, "自动定位", sizeof(location));
        // Fit by rendered pixel width and trim only whole UTF-8 code points.
        // Byte-count precision formats (e.g. %.14s) can split Chinese glyphs.
        fit_text_width(name, sizeof(name), location, 226);
        lcd_draw_text(16, y + 4, name, C_WHITE, 1);
        if (active) lcd_draw_text(256, y + 4, "当前", C_WHITE2, 1);
        lcd_draw_text(286, y + 4, "×", C_WARM, 1);
    }
    lcd_blend_round_rect(9, 207, 142, 23, 7, RGB(0x17,0x78,0xA4), 230);
    lcd_draw_text(22, 211, "+ 添加城市", C_WHITE, 1);
    lcd_draw_text_small(177, 215, "点选城市切换 · × 删除", C_WHITE2);
    lcd_flush();
}
settings_tap_result_t ui_city_manager_tap(int x, int y)
{
    if (!s_city_manager) return SETTINGS_TAP_NONE;
    if (y < 34 && x > 255) { ui_city_manager_cancel(); return SETTINGS_TAP_CHANGED; }
    if (y >= 207 && x < 155) {
        if (settings_city_count() >= SETTINGS_MAX_CITIES) return SETTINGS_TAP_NONE;
        s_picker_open = true; s_picker_add_mode = true; s_picker_level = PICK_PROVINCE; s_pick_scroll = 0;
        return SETTINGS_TAP_CHANGED;
    }
    if (y < 40 || y >= 202) return SETTINGS_TAP_NONE;
    int index = s_city_scroll + (y - 40) / 27;
    if (index < 0 || index >= settings_city_count()) return SETTINGS_TAP_NONE;
    if (x >= 276) {
        settings_remove_city(index);
        return SETTINGS_TAP_CHANGED;
    }
    bool changed = settings_activate_city(index);
    ui_city_manager_cancel();
    return changed ? SETTINGS_TAP_REFETCH : SETTINGS_TAP_CHANGED;
}

bool ui_settings_picker_open(void)
{
    return s_picker_open;
}

void ui_settings_picker_cancel(void)
{
    s_picker_open = false;
    s_picker_add_mode = false;
}

bool ui_settings_wifi_open(void) { return s_wifi_open; }
bool ui_settings_wifi_scanning(void) { return s_wifi_open && s_wifi_show_scan; }
bool ui_settings_wifi_scroll(int direction)
{
    if (!ui_settings_wifi_scanning() || !direction) return false;
    s_wifi_scan_scroll += direction * 5;
    if (s_wifi_scan_scroll < 0) s_wifi_scan_scroll = 0;
    return true;
}
void ui_settings_wifi_set_scan_results(const char *const ssids[], int count)
{
    s_wifi_scan_count = count < 0 ? 0 : count > 24 ? 24 : count;
    s_wifi_scan_scroll = 0;
    for (int i = 0; i < s_wifi_scan_count; ++i)
        strlcpy(s_wifi_scan_ssids[i], ssids && ssids[i] ? ssids[i] : "", sizeof(s_wifi_scan_ssids[i]));
    s_wifi_show_scan = true;
}
void ui_settings_wifi_cancel(void) { s_wifi_open = false; s_wifi_notice = false; }
const char *ui_settings_wifi_ssid(void) { return s_wifi_ssid; }
const char *ui_settings_wifi_password(void) { return s_wifi_password; }

static void wifi_append(char *buffer, size_t capacity, char ch)
{
    size_t len = strlen(buffer);
    if (len + 1 < capacity) { buffer[len] = ch; buffer[len + 1] = '\0'; }
}

settings_tap_result_t ui_settings_wifi_tap(int x, int y)
{
    if (!s_wifi_open) return SETTINGS_TAP_NONE;
    if (s_wifi_show_scan) {
        if (y < 30 && x >= 270) { s_wifi_open = false; return SETTINGS_TAP_WIFI_EDITED; }
        if (y < 30 && x >= 220) { s_wifi_show_scan = false; return SETTINGS_TAP_WIFI_EDITED; }
        if (y >= 43 && y < 218 && s_wifi_scan_count) {
            int index = s_wifi_scan_scroll + (y - 43) / 25;
            if (index >= 0 && index < s_wifi_scan_count) {
                strlcpy(s_wifi_ssid, s_wifi_scan_ssids[index], sizeof(s_wifi_ssid));
                s_wifi_show_scan = false;
                s_wifi_field = 1;
                return SETTINGS_TAP_WIFI_EDITED;
            }
        }
        return SETTINGS_TAP_NONE;
    }
    if (y < 30 && x >= 270) { ui_settings_wifi_cancel(); return SETTINGS_TAP_WIFI_EDITED; }
    if (y < 30 && x >= 220) return SETTINGS_TAP_WIFI_SCAN;
    if (y >= 32 && y < 61) { s_wifi_field = 0; return SETTINGS_TAP_WIFI_EDITED; }
    if (y >= 64 && y < 93) { s_wifi_field = 1; return SETTINGS_TAP_WIFI_EDITED; }
    if (y >= 99 && y < 196 && x >= 5 && x < 315) {
        int row = (y - 99) / 25;
        int col = (x - 5) / 31;
        static const char *const lower[] = { "1234567890", "qwertyuiop", "asdfghjkl;", "zxcvbnm,./" };
        static const char *const upper[] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL:", "ZXCVBNM<>?" };
        static const char *const symbols[] = { "!@#$%^&*()", "-_+=[]{}\\", ":;\"'<>?~`", "|,./\\\\%^" };
        const char *const *layout = s_wifi_symbols ? symbols : (s_wifi_upper ? upper : lower);
        char *target = s_wifi_field ? s_wifi_password : s_wifi_ssid;
        size_t capacity = s_wifi_field ? sizeof(s_wifi_password) : sizeof(s_wifi_ssid);
        if (row >= 0 && row < 4 && col >= 0 && col < 10) {
            wifi_append(target, capacity, layout[row][col]);
            s_wifi_notice = false;
            return SETTINGS_TAP_WIFI_EDITED;
        }
    }
    if (y >= 198 && y < 226 && x >= 4 && x < 312) {
        int action = (x - 4) / 44;
        char *target = s_wifi_field ? s_wifi_password : s_wifi_ssid;
        switch (action) {
        case 0: s_wifi_upper = !s_wifi_upper; break;
        case 1: s_wifi_symbols = !s_wifi_symbols; break;
        case 2: {
            size_t n = strlen(target);
            if (n) {
                --n;
                while (n && (((unsigned char)target[n] & 0xC0) == 0x80)) --n;
                target[n] = '\0';
            }
            break;
        }
        case 3: wifi_append(target, s_wifi_field ? sizeof(s_wifi_password) : sizeof(s_wifi_ssid), ' '); break;
        case 4: s_wifi_field = !s_wifi_field; break;
        case 5:
            if (!s_wifi_ssid[0]) { s_wifi_notice = true; return SETTINGS_TAP_WIFI_EDITED; }
            s_wifi_open = false;
            s_wifi_show_scan = false;
            return SETTINGS_TAP_WIFI_SAVE;
        case 6: ui_settings_wifi_cancel(); break;
        default: break;
        }
        return SETTINGS_TAP_WIFI_EDITED;
    }
    return SETTINGS_TAP_NONE;
}

bool ui_settings_picker_scroll(int direction)
{
    if (!s_picker_open || !direction) return false;
    s_pick_scroll += direction * (PICK_ROWS - 1);
    if (s_pick_scroll < 0) s_pick_scroll = 0;
    return true;
}

settings_tap_result_t ui_settings_picker_tap(int x, int y)
{
    if (!s_picker_open) return SETTINGS_TAP_NONE;
    if (y < 38 && x >= 245) {
        if (s_picker_level == PICK_DISTRICT) s_picker_level = PICK_CITY;
        else if (s_picker_level == PICK_CITY) s_picker_level = PICK_PROVINCE;
        else { s_picker_open = false; s_picker_add_mode = false; }
        s_pick_scroll = 0;
        return SETTINGS_TAP_CHANGED;
    }
    if (y < PICK_LIST_Y || y >= PICK_LIST_Y + PICK_ROWS * PICK_ROW_H) return SETTINGS_TAP_NONE;
    int index = s_pick_scroll + (y - PICK_LIST_Y) / PICK_ROW_H;
    if (s_picker_level == PICK_PROVINCE) {
        if (index == 0) {
            if (s_picker_add_mode) settings_add_region("", "", "", "");
            else settings_use_auto_location();
            s_picker_open = false; s_picker_add_mode = false; s_city_manager = false;
            return SETTINGS_TAP_REFETCH;
        }
        // “自动定位”作为列表首项，其余省份索引减一
        index -= 1;
        if (index < 0 || index >= region_province_count()) return SETTINGS_TAP_NONE;
        s_pick_province = index; s_picker_level = PICK_CITY; s_pick_scroll = 0;
    } else if (s_picker_level == PICK_CITY) {
        if (index < 0 || index >= region_city_count(s_pick_province)) return SETTINGS_TAP_NONE;
        s_pick_city = index; s_picker_level = PICK_DISTRICT; s_pick_scroll = 0;
    } else {
        if (index < 0 || index >= region_district_count(s_pick_province, s_pick_city)) return SETTINGS_TAP_NONE;
        const char *province = region_province_name(s_pick_province);
        const char *city = region_city_name(s_pick_province, s_pick_city);
        const char *district = region_district_name(s_pick_province, s_pick_city, index);
        const char *id = region_location_id(s_pick_province, s_pick_city, index);
        if (s_picker_add_mode) settings_add_region(province, city, district, id);
        else settings_set_region(province, city, district, id);
        s_picker_open = false; s_picker_add_mode = false; s_city_manager = false;
        return SETTINGS_TAP_REFETCH;
    }
    return SETTINGS_TAP_CHANGED;
}

bool ui_settings_brightness_drag(int x, int y)
{
    if (y < SET_BRIGHT_Y + 22 || y > SET_BRIGHT_Y + 52 || x < 105 || x > 295) return false;
    s_slider_dirty = true;
    int value = (x - 115) * 100 / 170;
    if (value < 1) value = 1;
    if (value > 100) value = 100;
    if (value == g_settings.brightness) return false;
    g_settings.brightness = value;
    lcd_backlight(value);
    return true;
}

bool ui_settings_brightness_drag_end(void)
{
    bool changed = s_slider_dirty;
    if (changed) settings_save();
    s_slider_dirty = false;
    return changed;
}

static int next_option(const int *values, int count, int current)
{
    for (int i = 0; i < count; ++i) {
        if (values[i] == current) return values[(i + 1) % count];
    }
    // Recover to the first supported value if NVS contains an obsolete value.
    return values[0];
}

settings_tap_result_t ui_settings_tap(int x, int y)
{
    if (x < 8 || x >= 310) return SETTINGS_TAP_NONE;
    if (y >= SET_CITY_Y && y < SET_CITY_Y + 46) {
        s_picker_open = true; s_picker_level = PICK_PROVINCE; s_pick_scroll = 0;
        return SETTINGS_TAP_CHANGED;
    }
    if (y >= SET_BRIGHT_Y && y < SET_BRIGHT_Y + 52) return SETTINGS_TAP_NONE;
    if (y < SET_LIST_Y || y >= SET_LIST_Y + 42) return SETTINGS_TAP_NONE;
    int cell = -1;
    for (int i = 0; i < 4; ++i) {
        int left = 8 + i * 77;
        if (x >= left && x < left + 72) { cell = i; break; }
    }
    switch (cell) {
    case 0:
        s_wifi_open = true;
        s_wifi_field = 0; s_wifi_symbols = false; s_wifi_upper = false;
        s_wifi_notice = false; s_wifi_show_scan = false;
        strlcpy(s_wifi_ssid, g_settings.wifi_ssid, sizeof(s_wifi_ssid));
        strlcpy(s_wifi_password, g_settings.wifi_password, sizeof(s_wifi_password));
        return SETTINGS_TAP_WIFI_EDITED;
    case 1:
        g_settings.refresh_min = next_option(REFRESH_LEVELS, REFRESH_COUNT, g_settings.refresh_min);
        return SETTINGS_TAP_REFETCH;
    case 2: g_settings.unit_f = !g_settings.unit_f; return SETTINGS_TAP_CHANGED;
    case 3:
        g_settings.sleep_sec = next_option(SLEEP_LEVELS, SLEEP_COUNT, g_settings.sleep_sec);
        return SETTINGS_TAP_CHANGED;
    default: return SETTINGS_TAP_NONE;
    }
}
