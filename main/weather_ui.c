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
        const char *event = active_alert->even