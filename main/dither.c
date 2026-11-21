#include "dither.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static int s_width;
static int16_t *s_cur;      // error carried into the current row, index x + 1
static int16_t *s_next;

static dither_mode_t s_mode = DITHER_FS;
static int s_y;

void dither_set_mode(dither_mode_t mode) { s_mode = mode; }
dither_mode_t dither_get_mode(void) { return s_mode; }

// Bayer 8x8 value 0..63 by bit-interleaving (x ^ y) and y, reversed.
static int bayer8(int x, int y)
{
    int a = y & 7, b = (x ^ y) & 7;
    return ((b & 1) << 5) | ((a & 1) << 4) | ((b & 2) << 2) | ((a & 2) << 1) |
           ((b & 4) >> 1) | ((a & 4) >> 2);
}

esp_err_t dither_begin(int width)
{
    dither_end();
    s_y = 0;
    s_cur = calloc(width + 2, sizeof(int16_t));
    s_next = calloc(width + 2, sizeof(int16_t));
    if (!s_cur || !s_next) {
        dither_end();
        return ESP_ERR_NO_MEM;
    }
    s_width = width;
    return ESP_OK;
}

void dither_row(const uint8_t *gray, uint8_t *bits)
{
    memset(bits, 0, (s_width + 7) / 8);
    if (s_mode == DITHER_BAYER) {
        for (int x = 0; x < s_width; x++) {
            if (gray[x] < bayer8(x, s_y) * 4 + 2) bits[x >> 3] |= 0x80 >> (x & 7);
        }
        s_y++;
        return;
    }
    // Serpentine scan: alternate direction per row, which breaks up the
    // vertical "worms" plain left-to-right Floyd-Steinberg leaves on smooth ramps.
    static bool rtl;
    int step = rtl ? -1 : 1;
    for (int i = 0; i < s_width; i++) {
        int x = rtl ? s_width - 1 - i : i;
        int v = gray[x] + s_cur[x + 1];
        int out = v < 128 ? 0 : 255;
        int err = v - out;
        if (out == 0) bits[x >> 3] |= 0x80 >> (x & 7);

        s_cur[x + 1 + step] += err * 7 / 16;
        s_next[x + 1 - step] += err * 3 / 16;
        s_next[x + 1]        += err * 5 / 16;
        s_next[x + 1 + step] += err * 1 / 16;
    }
    rtl = !rtl;
    int16_t *t = s_cur;
    s_cur = s_next;
    s_next = t;
    memset(s_next, 0, (s_width + 2) * sizeof(int16_t));
}

void dither_end(void)
{
    free(s_cur);
    free(s_next);
    s_cur = s_next = NULL;
}
