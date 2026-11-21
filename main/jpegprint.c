#include "jpegprint.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "dither.h"
#include "esp32c3/rom/tjpgd.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "printer.h"

static const char *TAG = "jpegprint";

#define POOL_BYTES   8192
#define HEAD_BYTES   1024      // start of the file, kept for the EXIF orientation
#define LONG_MAX_PX  1152      // longest side of the dithered picture
#define SLOTS        20        // target rows being accumulated at once (an MCU row touches <= 17)

typedef struct {
    jpegprint_read_fn rd;
    void *ctx;
    uint8_t head[HEAD_BYTES];
    size_t head_len;
    uint8_t scratch[256];

    int sw, sh;                // decoded (descaled) size
    int dw, dh;                // target size in sensor orientation, dw <= sw, dh <= sh
    int rb;                    // bytes per row of `bits`
    uint16_t *sum;             // SLOTS x dw gray sums
    uint8_t *cnt;              // SLOTS x dw pixel counts
    uint8_t *gray;             // one finished row
    uint8_t *bits;             // dh rows of dw bits
    int next_row;              // first target row not yet dithered
    int band_top;
    bool failed;
} jp_t;

static UINT jp_in(JDEC *jd, BYTE *buff, UINT n)
{
    jp_t *j = jd->device;
    UINT done = 0;
    while (done < n) {
        uint8_t *dst = buff ? buff + done : j->scratch;
        size_t want = n - done;
        if (!buff && want > sizeof j->scratch) want = sizeof j->scratch;
        int r = j->rd(j->ctx, dst, want);
        if (r <= 0) break;
        if (j->head_len < HEAD_BYTES) {
            size_t k = HEAD_BYTES - j->head_len;
            if (k > (size_t)r) k = r;
            memcpy(j->head + j->head_len, dst, k);
            j->head_len += k;
        }
        done += r;
    }
    return done;
}

// EXIF orientation 1..8 from the first bytes of the file; 1 if absent.
static int exif_orientation(const uint8_t *p, size_t n)
{
    if (n < 4 || p[0] != 0xFF || p[1] != 0xD8) return 1;
    size_t i = 2;
    while (i + 4 <= n && p[i] == 0xFF) {
        uint8_t marker = p[i + 1];
        size_t len = (p[i + 2] << 8) | p[i + 3];
        if (marker == 0xE1 && i + 10 <= n && memcmp(p + i + 4, "Exif\0\0", 6) == 0) {
            const uint8_t *t = p + i + 10;
            size_t tn = n - (i + 10);
            if (tn < 8) return 1;
            bool le = t[0] == 'I';
            #define U16(o) (le ? (t[o] | (t[(o) + 1] << 8)) : ((t[o] << 8) | t[(o) + 1]))
            #define U32(o) (le ? (t[o] | (t[(o) + 1] << 8) | (t[(o) + 2] << 16) | ((uint32_t)t[(o) + 3] << 24)) \
                               : (((uint32_t)t[o] << 24) | (t[(o) + 1] << 16) | (t[(o) + 2] << 8) | t[(o) + 3]))
            size_t ifd = U32(4);
            if (ifd + 2 > tn) return 1;
            size_t count = U16(ifd);
            for (size_t e = 0; e < count && ifd + 2 + (e + 1) * 12 <= tn; e++) {
                size_t o = ifd + 2 + e * 12;
                if (U16(o) == 0x0112) {
                    int v = U16(o + 8);
                    return v >= 1 && v <= 8 ? v : 1;
                }
            }
            #undef U16
            #undef U32
            return 1;
        }
        i += 2 + len;
    }
    return 1;
}

// Finished target row r: average, dither, store, free the slot.
static void flush_rows(jp_t *j, int upto)
{
    if (upto > j->dh) upto = j->dh;
    while (j->next_row < upto) {
        int r = j->next_row++;
        int slot = r % SLOTS;
        uint16_t *s = j->sum + (size_t)slot * j->dw;
        uint8_t *c = j->cnt + (size_t)slot * j->dw;
        for (int x = 0; x < j->dw; x++) j->gray[x] = c[x] ? s[x] / c[x] : 255;
        dither_row(j->gray, j->bits + (size_t)r * j->rb);
        memset(s, 0, (size_t)j->dw * sizeof *s);
        memset(c, 0, j->dw);
    }
}

static UINT jp_out(JDEC *jd, void *bitmap, JRECT *rc)
{
    jp_t *j = jd->device;
    const uint8_t *px = bitmap;
    int w = rc->right - rc->left + 1;

    if (rc->top != j->band_top) {            // a new row of MCUs begins: everything above is final
        j->band_top = rc->top;
        flush_rows(j, rc->top * j->dh / j->sh);
    }
    for (int y = rc->top; y <= rc->bottom; y++) {
        if (y >= j->sh) { px += w * 3; continue; }
        int slot = (y * j->dh / j->sh) % SLOTS;
        uint16_t *s = j->sum + (size_t)slot * j->dw;
        uint8_t *c = j->cnt + (size_t)slot * j->dw;
        for (int x = 0; x < w; x++, px += 3) {
            int tx = (rc->left + x) * j->dw / j->sw;
            if (tx >= j->dw) continue;
            s[tx] += (px[0] * 77 + px[1] * 150 + px[2] * 29) >> 8;
            c[tx]++;
        }
    }
    return 1;
}

static void free_work(jp_t *j, void *pool)
{
    free(pool);
    free(j->sum);
    free(j->cnt);
    free(j->gray);
    free(j->bits);
    free(j);
    dither_end();
}

esp_err_t jpegprint_decode(jpegprint_read_fn rd, void *ctx, uint8_t **bits_out, int *rows_out)
{
    int64_t t0 = esp_timer_get_time();
    jp_t *j = calloc(1, sizeof *j);
    void *pool = malloc(POOL_BYTES);
    if (!j || !pool) {
        free(j);
        free(pool);
        return ESP_ERR_NO_MEM;
    }
    j->rd = rd;
    j->ctx = ctx;
    j->band_top = -1;

    JDEC jd;
    JRESULT jr = jd_prepare(&jd, jp_in, pool, POOL_BYTES, j);
    if (jr != JDR_OK) {
        ESP_LOGE(TAG, "not a baseline JPEG (jd_prepare=%d)", jr);
        free_work(j, pool);
        return ESP_ERR_INVALID_ARG;
    }

    int w = jd.width, h = jd.height;
    int short_side = w < h ? w : h;
    int scale = 0;
    while (scale < 3 && (short_side >> (scale + 1)) >= PRINTER_WIDTH_PX) scale++;
    j->sw = w >> scale;
    j->sh = h >> scale;

    // Short side onto the paper width (never enlarge), then cap the long side.
    int ss = j->sw < j->sh ? j->sw : j->sh;
    int ls = j->sw < j->sh ? j->sh : j->sw;
    int num = ss > PRINTER_WIDTH_PX ? PRINTER_WIDTH_PX : ss, den = ss;
    if ((int64_t)ls * num / den > LONG_MAX_PX) { num = LONG_MAX_PX; den = ls; }
    j->dw = j->sw * num / den;
    j->dh = j->sh * num / den;
    if (j->dw < 1) j->dw = 1;
    if (j->dh < 1) j->dh = 1;
    j->rb = (j->dw + 7) / 8;

    int orient = exif_orientation(j->head, j->head_len);
    ESP_LOGI(TAG, "%dx%d, EXIF orientation %d, 1/%d -> %dx%d -> %dx%d dots", w, h, orient, 1 << scale,
             j->sw, j->sh, j->dw, j->dh);

    j->sum = calloc((size_t)SLOTS * j->dw, sizeof *j->sum);
    j->cnt = calloc((size_t)SLOTS * j->dw, 1);
    j->gray = malloc(j->dw);
    j->bits = calloc((size_t)j->dh, j->rb);
    if (!j->sum || !j->cnt || !j->gray || !j->bits || dither_begin(j->dw) != ESP_OK) {
        ESP_LOGE(TAG, "no memory for %dx%d", j->dw, j->dh);
        free_work(j, pool);
        return ESP_ERR_NO_MEM;
    }

    jr = jd_decomp(&jd, jp_out, scale);
    if (jr != JDR_OK) {
        ESP_LOGE(TAG, "decode failed (jd_decomp=%d) at row %d of %d", jr, j->next_row, j->dh);
        free_work(j, pool);
        return ESP_FAIL;
    }
    flush_rows(j, j->dh);
    int64_t t1 = esp_timer_get_time();

    // EXIF rotation (clockwise degrees), then turn landscape so its short side runs across the paper.
    static const int exif_rot[9] = { 0, 0, 0, 180, 180, 90, 90, 270, 270 };
    int rot = exif_rot[orient];
    bool sideways = rot == 90 || rot == 270;
    int ow = sideways ? j->dh : j->dw;       // picture size after the EXIF rotation
    int oh = sideways ? j->dw : j->dh;
    if (ow > oh) rot = (rot + 90) % 360;
    sideways = rot == 90 || rot == 270;
    ow = sideways ? j->dh : j->dw;
    int orows = sideways ? j->dw : j->dh;
    if (ow > PRINTER_WIDTH_PX) ow = PRINTER_WIDTH_PX;     // cannot happen, the short side is <= the paper width
    int xoff = (PRINTER_WIDTH_PX - ow) / 2;

    uint8_t *out = calloc(orows, PRINTER_ROW_BYTES);
    if (!out) {
        free_work(j, pool);
        return ESP_ERR_NO_MEM;
    }
    for (int y = 0; y < orows; y++) {
        uint8_t *row = out + (size_t)y * PRINTER_ROW_BYTES;
        for (int x = 0; x < ow; x++) {
            int sx, sy;
            switch (rot) {
            case 90:  sx = y;             sy = j->dh - 1 - x; break;
            case 180: sx = j->dw - 1 - x; sy = j->dh - 1 - y; break;
            case 270: sx = j->dw - 1 - y; sy = x;             break;
            default:  sx = x;             sy = y;             break;
            }
            if (j->bits[(size_t)sy * j->rb + (sx >> 3)] & (0x80 >> (sx & 7))) {
                int ox = x + xoff;
                row[ox >> 3] |= 0x80 >> (ox & 7);
            }
        }
    }
    free_work(j, pool);
    ESP_LOGI(TAG, "decoded in %d ms, +%d ms rotate (%d deg) -> %d rows", (int)((t1 - t0) / 1000),
             (int)((esp_timer_get_time() - t1) / 1000), rot, orows);
    *bits_out = out;
    *rows_out = orows;
    return ESP_OK;
}
