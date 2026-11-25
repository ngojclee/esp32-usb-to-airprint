// 1-bit conversion shared by the monochrome drivers (thermal, PCL): a page
// line of gray or RGB pixels into packed bits, 1 = black, MSB first, with
// an 8x8 ordered (Bayer) dither so greys come out as halftones.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const uint8_t MONO_BAYER8[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42},    {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44, 4, 36, 14, 46, 6, 38},   {60, 28, 52, 20, 62, 30, 54, 22},
    {3, 35, 11, 43, 1, 33, 9, 41},    {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37},   {63, 31, 55, 23, 61, 29, 53, 21},
};

// Packs `count` source pixels starting at pixel `x0` of line `y` into `out`
// (count / 8 rounded up bytes). px NULL = white line. Source pixels past
// src_w are white. Returns true if any bit is black.
static inline bool mono_pack_line(const uint8_t *px, int src_w, int bytes_pp, int y, int x0, int count, uint8_t *out)
{
    int nbytes = (count + 7) / 8;
    memset(out, 0, (size_t)nbytes);
    if (!px)
        return false;
    bool any = false;
    const uint8_t *row = MONO_BAYER8[y & 7];
    for (int i = 0; i < count; i++) {
        int x = x0 + i;
        if (x < 0 || x >= src_w)
            continue;
        int g;
        if (bytes_pp == 3) {
            const uint8_t *p = px + (size_t)x * 3;
            g = (p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8;
        } else {
            g = px[x];
        }
        // black when darker than the threshold of this cell: 0..255 vs 2..254
        if (g < row[x & 7] * 4 + 2) {
            out[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
            any = true;
        }
    }
    return any;
}
