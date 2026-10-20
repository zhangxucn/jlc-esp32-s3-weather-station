// 触摸：FT5x06（I2C 0x38，与 LCD 同总线，直连 I2C 读写）+ 滑动手势识别
// 寄存器协议（官方 esp_lcd_touch_ft5x06 驱动同款）：
//   0x02 = 触摸点数；0x03 起 6字节/点：[XH,XL,YH,YL,...]，x=((XH&0x0F)<<8)|XL
// 坐标变换（官方配置 swap_xy=1, mirror_x=1, mirror_y=0，先 mirror 后 swap）：
//   屏幕x = ry；屏幕y = 240 - rx
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "touch.h"
#include "ui_display.h"

static const char *TAG = "TOUCH";

#define FT5X06_ADDR      0x38
#define FT5X06_REG_NUM   0x02
#define FT5X06_REG_P1    0x03
#define FT5X06_RAW_XMAX  240
#define FT5X06_RAW_YMAX  320

static i2c_master_dev_handle_t s_dev = NULL;

static esp_err_t ft5x06_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 100);
}

bool touch_init(void)
{
    i2c_master_bus_handle_t bus = ui_display_i2c_bus();
    if (!bus) {
        ESP_LOGE(TAG, "i2c bus not ready");
        return false;
    }
    const i2c_device_config_t cfg = {
        .device_address = FT5X06_ADDR,
        .scl_speed_hz = 100000,
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device 0x38 failed: %d", err);
        return false;
    }
    uint8_t pts = 0;
    err = ft5x06_read(FT5X06_REG_NUM, &pts, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FT5x06 probe failed: %d", err);
        s_dev = NULL;
        return false;
    }
    ESP_LOGI(TAG, "FT5x06 ready @0x38");
    return true;
}

bool touch_pressed(int *x, int *y)
{
    if (!s_dev) return false;
    uint8_t pts = 0;
    if (ft5x06_read(FT5X06_REG_NUM, &pts, 1) != ESP_OK) return false;
    if (pts == 0 || pts > 5) return false;

    uint8_t d[6] = {0};
    if (ft5x06_read(FT5X06_REG_P1, d, 6) != ESP_OK) return false;

    int rx = ((d[0] & 0x0F) << 8) | d[1];
    int ry = ((d[2] & 0x0F) << 8) | d[3];

    // 官方变换：先 mirror_x（沿 240）再 swap_xy
    int sx = ry;
    int sy = FT5X06_RAW_XMAX - rx;
    if (sx < 0) sx = 0;
    if (sx > LCD_H_RES) sx = LCD_H_RES;
    if (sy < 0) sy = 0;
    if (sy > LCD_V_RES) sy = LCD_V_RES;
    if (x) *x = sx;
    if (y) *y = sy;
    return true;
}

// ================= 滑动手势状态机 =================
#define SWIPE_MIN_DY   24      // 最小滑动距离（px）
#define TAP_MAX_MOVE   16      // 容忍轻微触摸坐标抖动
#define TAP_MAX_MS     900     // 允许较慢的自然点按
#define LONG_PRESS_MS  2000
#define LONG_PRESS_MAX_MOVE 20 // FT5x06 坐标有轻微抖动，长按不应被误判成拖动

typedef enum { GS_IDLE, GS_DOWN } gs_t;
static gs_t s_gs = GS_IDLE;
static int s_sx, s_sy, s_lx, s_ly;
static TickType_t s_st;
static bool s_dragged;
static bool s_long_sent;

touch_gesture_t touch_poll(int *tap_x, int *tap_y)
{
    if (!s_dev) return TOUCH_NONE;
    int x = 0, y = 0;
    bool p = touch_pressed(&x, &y);
    TickType_t now = xTaskGetTickCount();

    switch (s_gs) {
    case GS_IDLE:
        if (p) {
            s_gs = GS_DOWN;
            s_sx = s_lx = x;
            s_sy = s_ly = y;
            s_st = now;
            s_dragged = false;
            s_long_sent = false;
        }
        break;
    case GS_DOWN:
        if (p) {
            s_lx = x;
            s_ly = y;
            // Check the long-press candidate independently from drag state:
            // a few pixels of sensor jitter can set s_dragged before 2 seconds.
            if (!s_long_sent && (now - s_st) >= pdMS_TO_TICKS(LONG_PRESS_MS) &&
                abs(x - s_sx) <= LONG_PRESS_MAX_MOVE &&
                abs(y - s_sy) <= LONG_PRESS_MAX_MOVE) {
                s_long_sent = true;
                if (tap_x) *tap_x = s_sx;
                if (tap_y) *tap_y = s_sy;
                return TOUCH_LONG_PRESS;
            }
            if (!s_dragged && (abs(x - s_sx) >= 8 || abs(y - s_sy) >= 8)) s_dragged = true;
            if (s_dragged) {
                if (tap_x) *tap_x = x;
                if (tap_y) *tap_y = y;
                return TOUCH_DRAG;
            }
        } else {
            s_gs = GS_IDLE;
            if (s_long_sent) { s_long_sent = false; break; }
            int dx = s_lx - s_sx;
            int dy = s_ly - s_sy;
            if (abs(dx) >= SWIPE_MIN_DY && abs(dx) > abs(dy)) {
                if (tap_x) *tap_x = s_lx;
                if (tap_y) *tap_y = s_ly;
                return dx < 0 ? TOUCH_SWIPE_LEFT : TOUCH_SWIPE_RIGHT;
            }
            if (abs(dy) >= SWIPE_MIN_DY && abs(dy) > abs(dx)) {
                if (tap_x) *tap_x = s_lx;
                if (tap_y) *tap_y = s_ly;
                return dy < 0 ? TOUCH_SWIPE_UP : TOUCH_SWIPE_DOWN;
            }
            if (s_dragged) {
                if (tap_x) *tap_x = s_lx;
                if (tap_y) *tap_y = s_ly;
                return TOUCH_DRAG_END;
            }
            if (abs(dx) < TAP_MAX_MOVE && abs(dy) < TAP_MAX_MOVE &&
                (now - s_st) < pdMS_TO_TICKS(TAP_MAX_MS)) {
                if (tap_x) *tap_x = s_lx;
                if (tap_y) *tap_y = s_ly;
                return TOUCH_TAP;
            }
        }
        break;
    }
    return TOUCH_NONE;
}
