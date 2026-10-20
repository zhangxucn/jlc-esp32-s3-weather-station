#pragma once
// 实战派 ESP32-S3 板载 LCD 驱动（ST7789 320x240 横屏）
// 引脚来自官方例程 szpi-s3-esp/06-lcd：SPI3 MOSI=40 CLK=41 DC=39 背光=42，CS 由 PCA9557 控制
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LCD_H_RES   320
#define LCD_V_RES   240

// RGB565 颜色宏（本屏最终管线，已用 v5/v6/v7 三次实拍反推验证）：
//   SPI 按内存小端发送 → 面板按大端拼接得到 swap16(framebuffer word)
//   → 面板按 BGR565 解释（MADCTL BGR=1）
// 因此 framebuffer 需存 swap16(BGR565(r,g,b))，面板还原后即设计色；
// 白色 0xFFFF 交换后不变，文字不受影响。
#define RGB(r, g, b) ((uint16_t)__builtin_bswap16((uint16_t)((((r) >> 3) & 0x1F) | ((((g) >> 2) & 0x3F) << 5) | ((((b) >> 3) & 0x1F) << 11))))

// 常用色板（彩色天气主题）
#define C_BG        RGB(0x10, 0x18, 0x28)   // 页面背景 深蓝黑（备用）
#define C_PANEL     RGB(0x1A, 0x26, 0x3C)   // 面板
#define C_TEXT      RGB(0xFF, 0xFF, 0xFF)   // 主文字 纯白
#define C_TEXT2     RGB(0xD8, 0xE6, 0xF4)   // 次要文字 浅蓝白
#define C_ACCENT    RGB(0x4E, 0xA3, 0xFF)   // 强调蓝
#define C_SUN       RGB(0xFF, 0xC1, 0x07)   // 太阳金黄
#define C_SUN_HI    RGB(0xFF, 0xE0, 0x82)   // 太阳高光
#define C_SUN_RAY   RGB(0xFF, 0xD5, 0x4F)   // 太阳光芒
#define C_RAIN      RGB(0x64, 0xB5, 0xF6)   // 雨滴蓝
#define C_SNOW      RGB(0xFF, 0xFF, 0xFF)   // 雪白
#define C_THUNDER   RGB(0xFF, 0xD5, 0x4F)   // 雷电金
#define C_WARM      RGB(0xFF, 0x8A, 0x3D)   // 暖色（高温）
#define C_HOT       RGB(0xFF, 0x5A, 0x2D)   // 酷热红橙
#define C_COOL      RGB(0x4F, 0xC3, 0xF7)   // 冷色（低温）
#define C_COLD      RGB(0x29, 0xB6, 0xF6)   // 寒冷蓝
#define C_HINT      RGB(0xD0, 0xDC, 0xEA)   // 提示文字
#define C_SHADOW    RGB(0x10, 0x20, 0x35)   // 文字阴影
#define C_CLOUD     RGB(0xF2, 0xF6, 0xFA)   // 云白
#define C_CLOUD_SH  RGB(0xB8, 0xC6, 0xD4)   // 云影
#define C_FOG       RGB(0xE0, 0xE6, 0xEB)   // 雾

// 初始化 LCD（I2C+PCA9557+SPI+ST7789+背光），并分配帧缓冲
esp_err_t ui_display_init(void);

// 获取 LCD 所用的 I2C 总线句柄（触摸等外设共用）
i2c_master_bus_handle_t ui_display_i2c_bus(void);

// 清屏
void lcd_clear(uint16_t color);

// 矩形填充（framebuffer 坐标）
void lcd_fill_rect(int x, int y, int w, int h, uint16_t color);

// 半透明混合填充（alpha 0-255，0=不变 255=纯色）
void lcd_blend_rect(int x, int y, int w, int h, uint16_t color, int alpha);

// 半透明实心圆
void lcd_blend_circle(int cx, int cy, int r, uint16_t color, int alpha);

// 垂直渐变（y0 到 y1-1，c_top → c_bot 线性过渡）
void lcd_gradient_v(int y0, int y1, uint16_t c_top, uint16_t c_bot);

// 三段垂直渐变（y0..mid_y-1 为 c0→c1，mid_y..y1-1 为 c1→c2）
void lcd_gradient_v3(int y0, int y1, int mid_y, uint16_t c0, uint16_t c1, uint16_t c2);

// 圆角矩形（纯色，r=圆角半径）
void lcd_fill_round_rect(int x, int y, int w, int h, int r, uint16_t color);

// 圆角矩形（半透明混合，玻璃卡用）
void lcd_blend_round_rect(int x, int y, int w, int h, int r, uint16_t color, int alpha);

// 画点
void lcd_draw_pixel(int x, int y, uint16_t color);

// 画线（Bresenham）
void lcd_draw_line(int x0, int y0, int x1, int y1, uint16_t color);

// 实心圆
void lcd_fill_circle(int cx, int cy, int r, uint16_t color);

// 绘制 UTF-8 文本（ASCII 8x16 / 中文 16x16，scale=1/2/3），返回文本宽度(px)
int lcd_draw_text(int x, int y, const char *utf8, uint16_t color, int scale);
int lcd_text_width(const char *utf8, int scale);
// 绘制紧凑 ASCII（8x12），适合小时标签等窄区域
int lcd_draw_text_compact(int x, int y, const char *ascii, uint16_t color);
// 缩小绘制（ASCII 8x12 / 中文 12x12），用于页脚提示
int lcd_draw_text_small(int x, int y, const char *utf8, uint16_t color);

// 带阴影文本（先画深色偏移再画主色），返回文本宽度(px)
int lcd_draw_shadow_text(int x, int y, const char *utf8, uint16_t color, int scale);

// 绘制大数字（48x48 字形，按墨迹宽度比例排字距），返回实际宽度(px)
int lcd_draw_big(int x, int y, const char *utf8, uint16_t color);
int lcd_draw_big_smooth(int x, int y, const char *utf8, uint16_t color);
int lcd_big_text_width(const char *utf8);

// 天气图标（矢量绘制，size 建议 24~64；bg=图标背后底色（月牙挖空用））
void lcd_draw_weather_icon(int cx, int cy, int size, const char *text, bool night, uint16_t bg);

// 把帧缓冲刷到屏幕
void lcd_flush(void);

// 背光（0-100）
void lcd_backlight(int percent);

#ifdef __cplusplus
}
#endif
