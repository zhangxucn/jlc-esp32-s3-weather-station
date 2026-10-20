// 自动生成 by tools/gen_font.py —— 请勿手改
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 16x16 1bpp 中文（GB2312 全量），2字节/行
const uint8_t *font_cjk16_get(uint32_t codepoint);
// 12x16 1bpp ASCII，2字节/行
const uint8_t *font_ascii16_get(uint32_t codepoint);
// 48x48 1bpp 大数字，6字节/行
const uint8_t *font_big48_get(uint32_t codepoint);

#ifdef __cplusplus
}
#endif
