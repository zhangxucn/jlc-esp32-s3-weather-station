// 实战派 ESP32-S3 板载 LCD 驱动实现（ST7789 320x240）
// 初始化参数对齐官方例程 szpi-s3-esp/06-lcd/main/esp32_s3_szp.c
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "ui_display.h"
#include "font_data.h"

static const char *TAG = "UIDISP";

// ================= 硬件参数（实战派） =================
#define BSP_I2C_SDA           (GPIO_NUM_1)
#define BSP_I2C_SCL           (GPIO_NUM_2)
#define BSP_I2C_FREQ_HZ       (100000)
#define PCA9557_ADDR          (0x19)
#define PCA9557_INPUT_PORT    0x00
#define PCA9557_OUTPUT_PORT   0x01
#define PCA9557_CONFIG_PORT   0x03
#define LCD_CS_BIT            BIT(0)   // PCA9557 IO0 = LCD_CS

#define BSP_LCD_SPI_NUM       (SPI3_HOST)
#define BSP_LCD_SPI_MOSI      (GPIO_NUM_40)
#define BSP_LCD_SPI_CLK       (GPIO_NUM_41)
#define BSP_LCD_DC            (GPIO_NUM_39)
#define BSP_LCD_BACKLIGHT     (GPIO_NUM_42)
#define BSP_LCD_PIXEL_CLOCK   (40 * 1000 * 1000)   // 40MHz（官方80MHz，降频取稳）
#define LCD_CMD_BITS          (8)
#define LCD_PARAM_BITS        (8)
#define BL_LEDC_CH            LEDC_CHANNEL_0

// 帧缓冲（PSRAM 双缓冲：DMA 传输与 CPU 绘制各用一块，避免撕裂）
static uint16_t *s_fb_pool[2] = { NULL, NULL };
static uint16_t *s_fb = NULL;               // 当前绘制缓冲（永远指向空闲块）
static SemaphoreHandle_t s_tx_sem = NULL;   // 传输完成信号（on_color_trans_done 给出）
static volatile bool s_tx_pending = false;  // 有帧正在 DMA 传输

// PCA9557 + LCD panel 句柄
static i2c_master_dev_handle_t s_pca = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static i2c_master_bus_handle_t s_bus = NULL;   // I2C 总线（触摸等共用）

// esp_lcd 官方缓冲回收点：最后一块 chunk 传完时触发
static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_tx_sem, &hp);
    return (hp == pdTRUE);
}

// ================= PCA9557 =================
static esp_err_t pca_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_pca, buf, 2, 100);
}

static esp_err_t pca_set_bit(uint8_t bit, int level)
{
    uint8_t reg = PCA9557_OUTPUT_PORT;
    uint8_t val = 0;
    esp_err_t err = i2c_master_transmit_receive(s_pca, &reg, 1, &val, 1, 100);
    if (err != ESP_OK) return err;
    val = level ? (val | bit) : (val & ~bit);
    return pca_write(PCA9557_OUTPUT_PORT, val);
}

static esp_err_t pca9557_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "i2c bus failed");
    s_bus = bus;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCA9557_ADDR,
        .scl_speed_hz = BSP_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_pca), TAG, "pca9557 add failed");

    // 默认值：DVP_PWDN=1 PA_EN=0 LCD_CS=1；IO0-2 输出，其余输入
    ESP_RETURN_ON_ERROR(pca_write(PCA9557_OUTPUT_PORT, 0x05), TAG, "pca out failed");
    ESP_RETURN_ON_ERROR(pca_write(PCA9557_CONFIG_PORT, 0xF8), TAG, "pca cfg failed");
    ESP_LOGI(TAG, "PCA9557 ready");
    return ESP_OK;
}

// ================= 背光（LEDC，低电平点亮） =================
static esp_err_t backlight_init(void)
{
    const ledc_channel_config_t ch = {
        .gpio_num = BSP_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = 1,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = true,     // 低电平点亮
    };
    const ledc_timer_config_t tm = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = 1,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&tm), TAG, "ledc timer");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "ledc ch");
    return ESP_OK;
}

void lcd_backlight(int percent)
{
    if (percent > 100) percent = 100;
    if (percent < 0) percent = 0;
    uint32_t duty = (1023 * (uint32_t)percent) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CH, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CH);
}

// ================= LCD 初始化 =================
static esp_err_t lcd_panel_init(void)
{
    spi_bus_config_t buscfg = {
        .sclk_io_num = BSP_LCD_SPI_CLK,
        .mosi_io_num = BSP_LCD_SPI_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),   // 整帧单笔传输（对齐官方例程，避免分块）
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_SPI_NUM, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BSP_LCD_DC,
        .cs_gpio_num = GPIO_NUM_NC,       // CS 由 PCA9557 控制
        .pclk_hz = BSP_LCD_PIXEL_CLOCK,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 2,                    // 官方例程 spi_mode=2
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_done,   // 帧传输完成回调（双缓冲回收）
        .flags.psram_dma_direct = true,   // DMA 直读 PSRAM，不再走内部内存临时缓冲（内存不足会截帧出彩条）
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_cfg, &io), TAG, "panel io");

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,   // 本屏按 BGR 解释（RGB 顺序会红蓝互换显粉色）
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel), TAG, "st7789");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    pca_set_bit(LCD_CS_BIT, 0);                       // 拉低 CS 并保持
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    esp_lcd_panel_invert_color(s_panel, true);        // 颜色反转（IPS）
    esp_lcd_panel_swap_xy(s_panel, true);             // 横屏
    esp_lcd_panel_mirror(s_panel, true, false);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp on");
    return ESP_OK;
}

esp_err_t ui_display_init(void)
{
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");
    ESP_RETURN_ON_ERROR(pca9557_init(), TAG, "pca9557");

    s_tx_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_tx_sem, ESP_ERR_NO_MEM, TAG, "sem alloc failed");

    ESP_RETURN_ON_ERROR(lcd_panel_init(), TAG, "panel");

    // 双帧缓冲优先 PSRAM，退回内部 RAM（第二块失败则共用一块，退化为等待模式）
    s_fb_pool[0] = heap_caps_malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_fb_pool[1] = heap_caps_malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_fb_pool[0]) {
        s_fb_pool[0] = heap_caps_malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
        s_fb_pool[1] = s_fb_pool[0];
    } else if (!s_fb_pool[1]) {
        s_fb_pool[1] = s_fb_pool[0];
    }
    ESP_RETURN_ON_FALSE(s_fb_pool[0], ESP_ERR_NO_MEM, TAG, "framebuffer alloc failed");
    s_fb = s_fb_pool[0];

    lcd_clear(C_BG);
    lcd_flush();
    lcd_backlight(100);
    ESP_LOGI(TAG, "LCD ready %dx%d fb0=%p fb1=%p", LCD_H_RES, LCD_V_RES, s_fb_pool[0], s_fb_pool[1]);
    return ESP_OK;
}

// ================= 绘制原语（写帧缓冲） =================
void lcd_draw_pixel(int x, int y, uint16_t color)
{
    if (!s_fb || x < 0 || y < 0 || x >= LCD_H_RES || y >= LCD_V_RES) return;
    s_fb[y * LCD_H_RES + x] = color;
}

void lcd_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_fb) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_H_RES) w = LCD_H_RES - x;
    if (y + h > LCD_V_RES) h = LCD_V_RES - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        uint16_t *row = &s_fb[(y + j) * LCD_H_RES + x];
        for (int i = 0; i < w; i++) row[i] = color;
    }
}

void lcd_clear(uint16_t color)
{
    lcd_fill_rect(0, 0, LCD_H_RES, LCD_V_RES, color);
}

void lcd_draw_line(int x0, int y0, int x1, int y1, uint16_t color)
{
    int dx = abs(x1 - x0), dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    while (1) {
        lcd_draw_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

void lcd_fill_circle(int cx, int cy, int r, uint16_t color)
{
    if (r <= 0) return;
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)sqrtf((float)(r * r - dy * dy));
        lcd_fill_rect(cx - dx, cy + dy, 2 * dx + 1, 1, color);
    }
}

// ================= 混合 / 渐变 =================
// framebuffer 中 word 为 swap16(BGR565)，混合前先还原到 BGR565 域运算，结果再转回
static inline uint16_t blend565(uint16_t dst, uint16_t src, int a)
{
    uint16_t dv = __builtin_bswap16(dst);
    uint16_t sv = __builtin_bswap16(src);
    int dr = dv & 0x1F, dg = (dv >> 5) & 0x3F, db = (dv >> 11) & 0x1F;
    int sr = sv & 0x1F, sg = (sv >> 5) & 0x3F, sb = (sv >> 11) & 0x1F;
    int r = (dr * (255 - a) + sr * a) / 255;
    int g = (dg * (255 - a) + sg * a) / 255;
    int b = (db * (255 - a) + sb * a) / 255;
    uint16_t v = (uint16_t)(r | (g << 5) | (b << 11));
    return __builtin_bswap16(v);
}

void lcd_blend_rect(int x, int y, int w, int h, uint16_t color, int alpha)
{
    if (!s_fb || alpha <= 0) return;
    if (alpha > 255) alpha = 255;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_H_RES) w = LCD_H_RES - x;
    if (y + h > LCD_V_RES) h = LCD_V_RES - y;
    if (w <= 0 || h <= 0) return;
    if (alpha == 255) { lcd_fill_rect(x, y, w, h, color); return; }
    for (int j = 0; j < h; j++) {
        uint16_t *row = &s_fb[(y + j) * LCD_H_RES + x];
        for (int i = 0; i < w; i++) row[i] = blend565(row[i], color, alpha);
    }
}

void lcd_blend_circle(int cx, int cy, int r, uint16_t color, int alpha)
{
    if (r <= 0 || alpha <= 0) return;
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)sqrtf((float)(r * r - dy * dy));
        lcd_blend_rect(cx - dx, cy + dy, 2 * dx + 1, 1, color, alpha);
    }
}

void lcd_gradient_v(int y0, int y1, uint16_t c_top, uint16_t c_bot)
{
    int span = y1 - y0;
    if (span <= 0) return;
    for (int j = 0; j < span; j++) {
        uint16_t c = blend565(c_top, c_bot, 255 * j / (span - 1 > 0 ? span - 1 : 1));
        lcd_fill_rect(0, y0 + j, LCD_H_RES, 1, c);
    }
}

void lcd_gradient_v3(int y0, int y1, int mid_y, uint16_t c0, uint16_t c1, uint16_t c2)
{
    if (mid_y <= y0) mid_y = y0 + 1;
    if (mid_y >= y1) mid_y = y1 - 1;
    int span1 = mid_y - y0, span2 = y1 - mid_y;
    for (int j = 0; j < span1; j++) {
        uint16_t c = blend565(c0, c1, 255 * j / (span1 - 1 > 0 ? span1 - 1 : 1));
        lcd_fill_rect(0, y0 + j, LCD_H_RES, 1, c);
    }
    for (int j = 0; j < span2; j++) {
        uint16_t c = blend565(c1, c2, 255 * j / (span2 - 1 > 0 ? span2 - 1 : 1));
        lcd_fill_rect(0, mid_y + j, LCD_H_RES, 1, c);
    }
}

void lcd_fill_round_rect(int x, int y, int w, int h, int r, uint16_t color)
{
    if (r < 0) r = 0;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    lcd_fill_rect(x + r, y, w - 2 * r, h, color);
    lcd_fill_rect(x, y + r, r, h - 2 * r, color);
    lcd_fill_rect(x + w - r, y + r, r, h - 2 * r, color);
    for (int dy = 0; dy < r; dy++) {
        int dx = (int)sqrtf((float)(r * r - dy * dy));
        lcd_fill_rect(x + r - dx, y + r - dy, dx, 1, color);
        lcd_fill_rect(x + w - r, y + r - dy, dx, 1, color);
        lcd_fill_rect(x + r - dx, y + h - r + dy, dx, 1, color);
        lcd_fill_rect(x + w - r, y + h - r + dy, dx, 1, color);
    }
}

void lcd_blend_round_rect(int x, int y, int w, int h, int r, uint16_t color, int alpha)
{
    if (r < 0) r = 0;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    lcd_blend_rect(x + r, y, w - 2 * r, h, color, alpha);
    lcd_blend_rect(x, y + r, r, h - 2 * r, color, alpha);
    lcd_blend_rect(x + w - r, y + r, r, h - 2 * r, color, alpha);
    for (int dy = 0; dy < r; dy++) {
        int dx = (int)sqrtf((float)(r * r - dy * dy));
        lcd_blend_rect(x + r - dx, y + r - dy, dx, 1, color, alpha);
        lcd_blend_rect(x + w - r, y + r - dy, dx, 1, color, alpha);
        lcd_blend_rect(x + r - dx, y + h - r + dy, dx, 1, color, alpha);
        lcd_blend_rect(x + w - r, y + h - r + dy, dx, 1, color, alpha);
    }
}

// ================= 刷屏 =================
i2c_master_bus_handle_t ui_display_i2c_bus(void)
{
    return s_bus;
}

void lcd_flush(void)
{
    if (!s_fb || !s_panel) return;

    // 1) 等上一帧 DMA 传完（on_color_trans_done 给信号），保证待会切回的缓冲可安全改写
    if (s_tx_pending) {
        if (xSemaphoreTake(s_tx_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
            ESP_LOGW(TAG, "flush wait timeout, force continue");
        }
        s_tx_pending = false;
    }

    // 2) 异步发送当前缓冲
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_H_RES, LCD_V_RES, s_fb);
    s_tx_pending = true;

    // 3) 切换绘制缓冲（CPU 画下一块，DMA 读当前块，互不干扰）
    if (s_fb_pool[0] != s_fb_pool[1]) {
        s_fb = (s_fb == s_fb_pool[0]) ? s_fb_pool[1] : s_fb_pool[0];
    }
}

// ================= 文字渲染 =================
static uint32_t utf8_next(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t cp;
    if (p[0] < 0x80) { cp = p[0]; *s += 1; }
    else if ((p[0] & 0xE0) == 0xC0) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); *s += 2; }
    else if ((p[0] & 0xF0) == 0xE0) {
        cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); *s += 3;
    } else { cp = '?'; *s += 1; }
    return cp;
}

// 画单个 1bpp 字形（pitch=每行字节数）
static void draw_glyph(int x, int y, const uint8_t *bmp, int w, int h, int pitch, uint16_t color, int scale)
{
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            if (bmp[j * pitch + (i >> 3)] & (0x80 >> (i & 7))) {
                if (scale == 1) lcd_draw_pixel(x + i, y + j, color);
                else lcd_fill_rect(x + i * scale, y + j * scale, scale, scale, color);
            }
        }
    }
}

static void draw_glyph_coverage(int x, int y, const uint8_t *bmp, int sw, int sh,
                                int pitch, int dw, int dh, uint16_t color)
{
    if (!s_fb || !bmp) return;
    for (int oy = 0; oy < dh; ++oy) {
        int sy0 = oy * sh / dh;
        int sy1 = ((oy + 1) * sh + dh - 1) / dh;
        for (int ox = 0; ox < dw; ++ox) {
            int sx0 = ox * sw / dw;
            int sx1 = ((ox + 1) * sw + dw - 1) / dw;
            int ink = 0, area = (sy1 - sy0) * (sx1 - sx0);
            for (int sy = sy0; sy < sy1; ++sy) {
                for (int sx = sx0; sx < sx1; ++sx) {
                    if (bmp[sy * pitch + (sx >> 3)] & (0x80 >> (sx & 7))) ++ink;
                }
            }
            if (!ink) continue;
            int alpha = (ink * 255 + area / 2) / area;
            if (alpha < 88) alpha = 88;
            int px = x + ox, py = y + oy;
            if (px >= 0 && py >= 0 && px < LCD_H_RES && py < LCD_V_RES) {
                uint16_t *dst = &s_fb[py * LCD_H_RES + px];
                *dst = blend565(*dst, color, alpha);
            }
        }
    }
}

int lcd_draw_text(int x, int y, const char *utf8, uint16_t color, int scale)
{
    if (!utf8 || scale < 1) return 0;
    int x0 = x;
    const char *s = utf8;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        if (cp == '\n') { y += 16 * scale; x = x0; continue; }
        const uint8_t *g = font_ascii16_get(cp);
        if (g) {
            draw_glyph(x, y, g, 12, 16, 2, color, scale);   // ASCII 12px 宽，2字节/行
            x += 12 * scale;
            continue;
        }
        g = font_cjk16_get(cp);
        if (g) {
            draw_glyph(x, y, g, 16, 16, 2, color, scale);
            x += 16 * scale;
        } else {
            lcd_fill_rect(x + 2, y + 2, 12 * scale, 12 * scale, C_HINT); // 缺字占位
            x += 16 * scale;
        }
    }
    return x - x0;
}

int lcd_text_width(const char *utf8, int scale)
{
    if (!utf8) return 0;
    int w = 0;
    const char *s = utf8;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        if (cp == '\n') break;
        w += (cp < 0x80) ? 12 * scale : 16 * scale;
    }
    return w;
}

int lcd_draw_text_compact(int x, int y, const char *ascii, uint16_t color)
{
    if (!ascii) return 0;
    int x0 = x;
    for (const unsigned char *s = (const unsigned char *)ascii; *s; s++) {
        const uint8_t *bmp = font_ascii16_get(*s);
        if (!bmp) { x += 8; continue; }
        // 12x16 字形按覆盖采样缩至 8x12，细笔画不会因简单隔点而丢失。
        for (int oy = 0; oy < 12; oy++) {
            int sy0 = oy * 16 / 12;
            int sy1 = (oy + 1) * 16 / 12;
            for (int ox = 0; ox < 8; ox++) {
                int sx0 = ox * 12 / 8;
                int sx1 = (ox + 1) * 12 / 8;
                bool ink = false;
                for (int sy = sy0; sy < sy1 && !ink; sy++) {
                    for (int sx = sx0; sx < sx1; sx++) {
                        if (bmp[sy * 2 + (sx >> 3)] & (0x80 >> (sx & 7))) {
                            ink = true;
                            break;
                        }
                    }
                }
                if (ink) lcd_draw_pixel(x + ox, y + oy, color);
            }
        }
        x += 8;
    }
    return x - x0;
}

int lcd_draw_text_small(int x, int y, const char *utf8, uint16_t color)
{
    if (!utf8) return 0;
    int x0 = x;
    const char *s = utf8;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        if (cp == '\n') break;
        const uint8_t *bmp = font_ascii16_get(cp);
        int sw = 12, sh = 16, dw = 8, dh = 12;
        if (!bmp) {
            bmp = font_cjk16_get(cp);
            sw = sh = 16; dw = dh = 12;
        }
        if (!bmp) { x += dw; continue; }
        for (int oy = 0; oy < dh; ++oy) {
            int sy0 = oy * sh / dh, sy1 = (oy + 1) * sh / dh;
            for (int ox = 0; ox < dw; ++ox) {
                int sx0 = ox * sw / dw, sx1 = (ox + 1) * sw / dw;
                int ink = 0, area = (sy1 - sy0) * (sx1 - sx0);
                for (int sy = sy0; sy < sy1; ++sy)
                    for (int sx = sx0; sx < sx1; ++sx)
                        if (bmp[sy * 2 + (sx >> 3)] & (0x80 >> (sx & 7))) ++ink;
                if (ink && x + ox >= 0 && x + ox < LCD_H_RES && y + oy >= 0 && y + oy < LCD_V_RES) {
                    int alpha = (ink * 255 + area / 2) / area;
                    if (alpha < 88) alpha = 88;
                    uint16_t *dst = &s_fb[(y + oy) * LCD_H_RES + x + ox];
                    *dst = blend565(*dst, color, alpha);
                }
            }
        }
        x += dw;
    }
    return x - x0;
}

// 墨迹宽度/左边界扫描（大数字比例字距用）
static int glyph_ink(const uint8_t *bmp, int w, int h, int pitch, int *out_left)
{
    int minb = w, maxb = -1;
    for (int i = 0; i < w; i++) {
        bool on = false;
        for (int j = 0; j < h; j++) {
            if (bmp[j * pitch + (i >> 3)] & (0x80 >> (i & 7))) { on = true; break; }
        }
        if (on) { if (i < minb) minb = i; if (i > maxb) maxb = i; }
    }
    if (maxb < 0) { *out_left = 0; return w / 2; }
    *out_left = minb;
    return maxb - minb + 1;
}

#define BIG_TRACK   6      // 大数字字距
#define BIG_SPACE   18     // 空格宽

int lcd_big_text_width(const char *utf8)
{
    if (!utf8) return 0;
    int w = 0;
    const char *s = utf8;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        if (cp == ' ') { w += BIG_SPACE; continue; }
        const uint8_t *g = font_big48_get(cp);
        if (g) {
            int left, ink = glyph_ink(g, 48, 48, 6, &left);
            w += ink + BIG_TRACK;
        } else {
            w += (cp < 0x80) ? 16 : 32;   // 退化 2x 小字
        }
    }
    return w > 0 ? w - BIG_TRACK : 0;     // 末字不留字距
}

int lcd_draw_big(int x, int y, const char *utf8, uint16_t color)
{
    if (!utf8) return 0;
    int x0 = x;
    const char *s = utf8;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        if (cp == ' ') { x += BIG_SPACE; continue; }
        const uint8_t *g = font_big48_get(cp);
        if (g) {
            int left, ink = glyph_ink(g, 48, 48, 6, &left);
            draw_glyph(x - left, y, g, 48, 48, 6, color, 1);   // 墨迹左对齐
            x += ink + BIG_TRACK;
        } else {
            // 大字库里没有的字符，退化用 2x 小字
            const uint8_t *g2 = font_ascii16_get(cp);
            if (g2) {
                draw_glyph(x, y + 16, g2, 12, 16, 2, color, 2);
                x += 24;
            } else {
                const uint8_t *g3 = font_cjk16_get(cp);
                if (g3) {
                    draw_glyph(x, y + 8, g3, 16, 16, 2, color, 2);
                    x += 32;
                }
            }
        }
    }
    return x - x0;
}

int lcd_draw_big_smooth(int x, int y, const char *utf8, uint16_t color)
{
    if (!utf8) return 0;
    const int size = 44;
    int x0 = x;
    const char *s = utf8;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        if (cp == ' ') { x += BIG_SPACE * size / 48; continue; }
        const uint8_t *g = font_big48_get(cp);
        if (g) {
            int left, ink = glyph_ink(g, 48, 48, 6, &left);
            int scaled_left = left * size / 48;
            int scaled_ink = (ink * size + 47) / 48;
            draw_glyph_coverage(x - scaled_left, y, g, 48, 48, 6, size, size, color);
            x += scaled_ink + BIG_TRACK * size / 48;
        } else {
            const uint8_t *g2 = font_ascii16_get(cp);
            if (g2) {
                draw_glyph(x, y + 8, g2, 12, 16, 2, color, 2);
                x += 24;
            } else {
                const uint8_t *g3 = font_cjk16_get(cp);
                if (g3) {
                    draw_glyph(x, y + 4, g3, 16, 16, 2, color, 2);
                    x += 32;
                }
            }
        }
    }
    return x - x0;
}

int lcd_draw_shadow_text(int x, int y, const char *utf8, uint16_t color, int scale)
{
    lcd_draw_text(x + 1, y + 1, utf8, C_SHADOW, scale);
    return lcd_draw_text(x, y, utf8, color, scale);
}

// ================= 天气图标（矢量、仿主流天气 App，尺寸自适应细节） =================
// 太阳：外光晕 + 长短交替光芒 + 本体 + 左上高光（小尺寸减芒、去光晕防糊）
static void icon_sun(int cx, int cy, int s)
{
    int r = s / 5;
    if (r < 3) r = 3;
    if (s >= 24) lcd_blend_circle(cx, cy, r + s / 5, RGB(0xFF, 0xC8, 0x5C), 42);   // 暖光晕
    int rays = (s >= 24) ? 12 : 8;
    for (int a = 0; a < rays; a++) {
        float rad = a * 3.14159265f / (rays / 2);
        int len = (s >= 24) ? ((a % 2 == 0) ? (r + s / 6) : (r + s / 10)) : (r + s / 7);
        int x0 = cx + (int)(cosf(rad) * (r + 1));
        int y0 = cy + (int)(sinf(rad) * (r + 1));
        int x1 = cx + (int)(cosf(rad) * len);
        int y1 = cy + (int)(sinf(rad) * len);
        lcd_draw_line(x0, y0, x1, y1, C_SUN_RAY);
        if (s >= 24) lcd_draw_line(x0 + 1, y0, x1 + 1, y1, C_SUN_RAY);             // 加粗
    }
    lcd_fill_circle(cx, cy, r, C_SUN);
    if (s >= 20) lcd_fill_circle(cx - r / 3, cy - r / 3, r / 2, C_SUN_HI);          // 高光
}

// 月亮：冷光晕 + 月牙（挖空）+ 环形山 + 小星
static void icon_moon(int cx, int cy, int s, uint16_t bg)
{
    int r = s / 3;
    if (r < 3) r = 3;
    lcd_blend_circle(cx, cy, r + s / 6, RGB(0xFF, 0xF3, 0xC4), 32);     // 冷光晕
    lcd_fill_circle(cx, cy, r, RGB(0xFF, 0xF3, 0xC4));
    lcd_fill_circle(cx - r / 4, cy - r / 4, r / 3, RGB(0xFF, 0xFB, 0xEA));  // 亮面
    lcd_fill_circle(cx + r / 3, cy + r / 5, r / 6, RGB(0xE8, 0xDC, 0xB8));  // 暗斑
    lcd_fill_circle(cx + r / 2, cy - r / 3, r - 1, bg);                    // 挖月牙
    // 两颗小星
    lcd_draw_pixel(cx + r + s / 8, cy - r - s / 10, RGB(0xFF, 0xFF, 0xFF));
    lcd_draw_pixel(cx + r + s / 8 + 1, cy - r - s / 10 + 1, RGB(0xFF, 0xFF, 0xFF));
    lcd_draw_pixel(cx + r + s / 6, cy - r + s / 10, RGB(0xE8, 0xEE, 0xF7));
}

// 云朵主体：大尺寸双层体积感（阴影层下移 + 亮层）；小尺寸简化单团防糊
static void icon_cloud(int cx, int cy, int s, uint16_t main, uint16_t shade)
{
    int r  = s / 5 + 1;      // 侧圆半径
    int rm = s / 4 + 1;      // 顶圆半径
    if (r < 2) r = 2;
    if (rm < 3) rm = 3;

    if (s < 20) {
        // 小尺寸：两个圆 + 底条的一团云，干净利落
        int rr = s / 3 + 1;
        lcd_fill_circle(cx - s / 6, cy + s / 12, rr, main);
        lcd_fill_circle(cx + s / 6, cy + s / 12, rr, main);
        lcd_fill_rect(cx - s / 6, cy + s / 12, s / 3 + 1, rr, main);
        lcd_fill_rect(cx - s / 6, cy + s / 12 + rr - 1, s / 3 + 1, 1, shade);
        return;
    }
    // 阴影层（整体下移 2px，深色）
    lcd_fill_circle(cx - s / 4, cy + s / 12 + 2, r, shade);
    lcd_fill_circle(cx + s / 12, cy - s / 8 + 2, rm, shade);
    lcd_fill_circle(cx + s / 4, cy + s / 10 + 2, r - 1, shade);
    lcd_fill_rect(cx - s / 4, cy + s / 12 + 2, s / 2 + s / 8, r, shade);
    // 主体层
    lcd_fill_circle(cx - s / 4, cy + s / 12, r, main);
    lcd_fill_circle(cx + s / 12, cy - s / 8, rm, main);
    lcd_fill_circle(cx + s / 4, cy + s / 10, r - 1, main);
    lcd_fill_rect(cx - s / 4, cy + s / 12, s / 2 + s / 8, r, main);
}

// 雨：云 + 圆头水滴（圆点头部 + 斜向细尾）
static void icon_rain(int cx, int cy, int s)
{
    icon_cloud(cx, cy - s / 8, s, C_CLOUD, C_CLOUD_SH);
    for (int i = -1; i <= 1; i++) {
        int dx = cx + i * s / 4;
        int dy = cy + s / 5;
        lcd_draw_line(dx, dy, dx - s / 14, dy + s / 4, C_RAIN);         // 尾
        if (s >= 20) lcd_draw_line(dx + 1, dy, dx - s / 14 + 1, dy + s / 4, C_RAIN);
        lcd_fill_circle(dx, dy, (s >= 20) ? (2 + s / 48) : 1, C_RAIN);  // 圆头
    }
}

// 雪：云 + 六角雪花（三轴交叉）
static void icon_snow(int cx, int cy, int s)
{
    icon_cloud(cx, cy - s / 8, s, C_CLOUD, C_CLOUD_SH);
    for (int i = -1; i <= 1; i++) {
        int sx = cx + i * s / 4;
        int sy = cy + s / 4;
        int rr = 2 + s / 24;
        if (s < 20) {                                                   // 小尺寸：单点雪花
            lcd_draw_pixel(sx, sy, C_SNOW);
            continue;
        }
        lcd_draw_line(sx - rr, sy, sx + rr, sy, C_SNOW);                // 横轴
        lcd_draw_line(sx - rr / 2, sy - rr, sx + rr / 2, sy + rr, C_SNOW);   // 斜轴 1
        lcd_draw_line(sx - rr / 2, sy + rr, sx + rr / 2, sy - rr, C_SNOW);   // 斜轴 2
    }
}

// 雷：深灰云 + 加粗锯齿闪电
static void icon_thunder(int cx, int cy, int s)
{
    icon_cloud(cx, cy - s / 8, s, RGB(0x9E, 0xAE, 0xBE), RGB(0x76, 0x86, 0x98));
    int bx = cx - s / 24, by = cy + s / 8;
    int pts[6][2] = {
        { 0, 0 }, { -s / 8, s / 5 }, { 0, s / 5 },
        { -s / 7, s / 2 }, { -s / 20, s / 2 }, { -s / 6, s * 2 / 3 },
    };
    for (int i = 0; i < 5; i++) {
        lcd_draw_line(bx + pts[i][0], by + pts[i][1], bx + pts[i + 1][0], by + pts[i + 1][1], C_THUNDER);
        lcd_draw_line(bx + pts[i][0] + 1, by + pts[i][1], bx + pts[i + 1][0] + 1, by + pts[i + 1][ 1], C_THUNDER);
    }
}

// 雾：圆头横线，长短交错（小尺寸减为 3 条防糊）
static void icon_fog(int cx, int cy, int s)
{
    int lines = (s < 20) ? 3 : 4;
    for (int i = 0; i < lines; i++) {
        int ly = cy - s / 3 + i * (s / (lines - 1 > 0 ? lines - 1 : 1)) * 3 / 2;
        int lw = s / 2 - (i % 2 == 0 ? 0 : s / 10);
        int lx = cx - lw / 2;
        uint16_t c = (i % 2 == 0) ? C_FOG : RGB(0xC4, 0xD0, 0xDC);
        int th = 2 + s / 32;
        lcd_fill_rect(lx, ly, lw, th, c);
        lcd_fill_circle(lx, ly + th / 2, th / 2 + 1, c);                 // 左圆头
        lcd_fill_circle(lx + lw, ly + th / 2, th / 2 + 1, c);            // 右圆头
    }
}

// 风：双线流风，尾部上翘
static void icon_wind(int cx, int cy, int s)
{
    for (int i = -1; i <= 1; i++) {
        int y = cy + i * s / 5;
        int x0 = cx - s / 3, x1 = cx + s / 6;
        lcd_draw_line(x0, y, x1, y, C_CLOUD);
        lcd_draw_line(x0, y + 1, x1, y + 1, C_CLOUD);
        lcd_draw_line(x1, y, x1 + s / 12, y - s / 12, C_CLOUD);          // 上翘勾
        lcd_draw_line(x1, y + 1, x1 + s / 12, y - s / 12 + 1, C_CLOUD);
    }
}

void lcd_draw_weather_icon(int cx, int cy, int size, const char *text, bool night, uint16_t bg)
{
    if (!text) { icon_cloud(cx, cy, size, C_CLOUD, C_CLOUD_SH); return; }
    if (strstr(text, "晴")) {
        if (night) icon_moon(cx, cy, size, bg);
        else icon_sun(cx, cy, size);
    } else if (strstr(text, "雷")) icon_thunder(cx, cy, size);
    else if (strstr(text, "雪") || strstr(text, "冰")) icon_snow(cx, cy, size);
    else if (strstr(text, "雨") || strstr(text, "霖")) icon_rain(cx, cy, size);
    else if (strstr(text, "雾") || strstr(text, "霾") || strstr(text, "沙") || strstr(text, "尘")) icon_fog(cx, cy, size);
    else if (strstr(text, "风")) icon_wind(cx, cy, size);
    else if (strstr(text, "阴")) icon_cloud(cx, cy, size, C_CLOUD_SH, RGB(0x96, 0xA6, 0xB8));   // 阴天更灰
    else icon_cloud(cx, cy, size, C_CLOUD, C_CLOUD_SH);
}
