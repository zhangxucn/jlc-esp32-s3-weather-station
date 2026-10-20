#pragma once
// 触摸（FT5x06，I2C 0x38，与 LCD 同总线）+ 滑动手势
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TOUCH_NONE = 0,
    TOUCH_SWIPE_UP,      // 上下滑动（设置列表滚动）
    TOUCH_SWIPE_DOWN,
    TOUCH_TAP,           // 轻点
    TOUCH_DRAG,          // 按住移动，坐标持续返回
    TOUCH_DRAG_END,      // 拖动结束
    TOUCH_SWIPE_LEFT,    // 左滑 → 下一页
    TOUCH_SWIPE_RIGHT,   // 右滑 → 上一页
    TOUCH_LONG_PRESS,    // 长按 2 秒
} touch_gesture_t;

// 初始化触摸（需在 ui_display_init 之后调用，复用其 I2C 总线）
bool touch_init(void);

// 非阻塞轮询手势（主循环调用）；tap 时通过 tap_x/tap_y 返回释放点坐标（可为 NULL）
touch_gesture_t touch_poll(int *tap_x, int *tap_y);

// 当前是否按住（用于设置页点击判断）
bool touch_pressed(int *x, int *y);

#ifdef __cplusplus
}
#endif
