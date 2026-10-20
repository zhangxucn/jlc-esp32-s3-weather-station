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
                    if (bmp[sy * pitch + (s