#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成 LCD 点阵字库 → font_data.c / font_data.h
- CJK 16x16 1bpp（GB2312 全覆盖，城市名/天气词任意渲染）
- ASCII 8x16 1bpp
- BIG 48x48 1bpp（温度大数字）
"""
import os
from PIL import Image, ImageDraw, ImageFont

OUT_DIR = os.path.join(os.path.dirname(__file__), '..', 'main')

# 字体选择
CJK_FONT = '/System/Library/Fonts/Hiragino Sans GB.ttc'
CJK_INDEX = 0  # Hiragino Sans GB W3
BIG_FONT = '/System/Library/Fonts/Hiragino Sans GB.ttc'
BIG_INDEX = 0  # Hiragino Sans GB W3（细体，参考手机天气 App 风格）
# ASCII 专用小字号拉丁字体（Hiragino 拉丁字形 8px 二值化后 W/M 笔画断裂，Geneva 15px @12px 清晰）
ASCII_FONT = '/System/Library/Fonts/Geneva.ttf'
ASCII_SIZE = 15
ASCII_W = 12

THRESHOLD = 96

def glyph_bitmap(font, ch, w, h):
    """渲染单字符为 1bpp，返回 bytes（行主序，每行向上取整到字节，MSB 先）"""
    img = Image.new('L', (w, h), 0)
    d = ImageDraw.Draw(img)
    bbox = font.getbbox(ch)
    gw = bbox[2] - bbox[0]
    gh = bbox[3] - bbox[1]
    if gw <= 0 or gh <= 0:
        return bytes(((w + 7) // 8) * h)
    x = (w - gw) // 2 - bbox[0]
    y = (h - gh) // 2 - bbox[1]
    d.text((x, y), ch, font=font, fill=255)
    row_bytes = (w + 7) // 8
    out = bytearray(row_bytes * h)
    for yy in range(h):
        for xx in range(w):
            if img.getpixel((xx, yy)) > THRESHOLD:
                out[yy * row_bytes + xx // 8] |= 0x80 >> (xx % 8)
    return bytes(out)

def gb2312_chars():
    """全部 GB2312 字符（汉字 + 符号）"""
    chars = set()
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode('gb2312')
                if ch.strip():
                    chars.add(ch)
            except UnicodeDecodeError:
                pass
    return sorted(chars)

def main():
    # 探测字体 face
    for idx in range(8):
        try:
            f = ImageFont.truetype(CJK_FONT, 16, index=idx)
            print(f'face[{idx}]:', f.getname())
        except Exception:
            break

    f_cjk = ImageFont.truetype(CJK_FONT, 15, index=CJK_INDEX)
    f_ascii = ImageFont.truetype(ASCII_FONT, ASCII_SIZE)
    f_big = ImageFont.truetype(BIG_FONT, 46, index=BIG_INDEX)

    # ---- CJK 16x16（GB2312 全量）----
    chars = gb2312_chars()
    print(f'GB2312 chars: {len(chars)}')
    cjk_code = []
    cjk_data = bytearray()
    for ch in chars:
        cp = ord(ch)
        cjk_code.append(cp)
        cjk_data += glyph_bitmap(f_cjk, ch, 16, 16)
    order = sorted(range(len(cjk_code)), key=lambda i: cjk_code[i])
    cjk_code = [cjk_code[i] for i in order]
    cjk_data = b''.join(cjk_data[i*32:(i+1)*32] for i in order)

    # ---- ASCII 12x16（2字节/行）----
    ascii_data = bytearray()
    for c in range(0x20, 0x7F):
        ascii_data += glyph_bitmap(f_ascii, chr(c), ASCII_W, 16)

    # ---- BIG 48x48 ----
    BIG_CHARS = "0123456789.~-℃%± "
    big_code = []
    big_data = bytearray()
    for ch in BIG_CHARS:
        big_code.append(ord(ch))
        big_data += glyph_bitmap(f_big, ch, 48, 48)

    # ---- 输出 C ----
    def hex_block(name, data, cols=16):
        lines = [f'static const uint8_t {name}[{len(data)}] = {{']
        for i in range(0, len(data), cols):
            chunk = ', '.join(f'0x{b:02x}' for b in data[i:i+cols])
            lines.append(f'    {chunk},')
        lines.append('};')
        return '\n'.join(lines)

    def u32_block(name, arr):
        lines = [f'static const uint32_t {name}[{len(arr)}] = {{']
        for i in range(0, len(arr), 8):
            chunk = ', '.join(f'0x{v:05x}' for v in arr[i:i+8])
            lines.append(f'    {chunk},')
        lines.append('};')
        return '\n'.join(lines)

    c_src = f'''// 自动生成 by tools/gen_font.py —— 请勿手改
#include "font_data.h"

#define CJK16_COUNT   {len(cjk_code)}
#define ASCII16_COUNT 95
#define BIG48_COUNT   {len(big_code)}
#define CJK16_BYTES   32
#define ASCII16_BYTES {(ASCII_W + 7) // 8 * 16}
#define BIG48_BYTES   288

{hex_block("cjk16_data", cjk_data)}
{u32_block("cjk16_code", cjk_code)}
{hex_block("ascii16_data", ascii_data)}
{hex_block("big48_data", big_data)}
{u32_block("big48_code", big_code)}

const uint8_t *font_cjk16_get(uint32_t cp)
{{
    int lo = 0, hi = CJK16_COUNT - 1;
    while (lo <= hi) {{
        int mid = (lo + hi) / 2;
        if (cjk16_code[mid] == cp) return &cjk16_data[mid * CJK16_BYTES];
        if (cjk16_code[mid] < cp) lo = mid + 1;
        else hi = mid - 1;
    }}
    return 0;
}}

const uint8_t *font_ascii16_get(uint32_t cp)
{{
    if (cp < 0x20 || cp > 0x7E) return 0;
    return &ascii16_data[(cp - 0x20) * ASCII16_BYTES];
}}

const uint8_t *font_big48_get(uint32_t cp)
{{
    for (int i = 0; i < BIG48_COUNT; i++) {{
        if (big48_code[i] == cp) return &big48_data[i * BIG48_BYTES];
    }}
    return 0;
}}
'''

    h_src = '''// 自动生成 by tools/gen_font.py —— 请勿手改
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
'''

    with open(os.path.join(OUT_DIR, 'font_data.c'), 'w') as f:
        f.write(c_src)
    with open(os.path.join(OUT_DIR, 'font_data.h'), 'w') as f:
        f.write(h_src)

    total = len(cjk_data) + len(ascii_data) + len(big_data) + len(cjk_code)*4 + len(big_code)*4
    print(f'OK: cjk={len(cjk_code)} glyphs ({len(cjk_data)}B), big={len(big_code)} glyphs')
    print(f'total font payload ≈ {total/1024:.1f} KB')
    print(f'written: {os.path.join(OUT_DIR, "font_data.c")} / font_data.h')

if __name__ == '__main__':
    main()
