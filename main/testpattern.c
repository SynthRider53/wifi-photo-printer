#include "testpattern.h"

#include <stdlib.h>
#include <string.h>

#include "dither.h"
#include "esp_check.h"
#include "esp_log.h"
#include "printer.h"
#include "printjob.h"

static const char *TAG = "testpattern";

#define W PRINTER_WIDTH_PX
#define H 340

#define CIRCLE_CX 192
#define CIRCLE_CY 212
#define CIRCLE_R  78

static int ruler_tick_rows(int x)
{
    if (x % 8) return 0;
    int mm = x / 8;
    if (mm % 10 == 0) return 20;    // centimetre
    if (mm % 5 == 0) return 12;
    return 6;
}

// Gray value 0 (black) .. 255 (white) of the test picture at (x, y).
static uint8_t pattern_gray(int x, int y)
{
    if (x < 2 || x >= W - 2 || y < 2 || y >= H - 2) return 0;       // frame

    if (y < 28) {                                                   // ruler
        return (y - 4) >= 0 && (y - 4) < ruler_tick_rows(x) ? 0 : 255;
    }
    if (y < 76) return x * 255 / (W - 1);                           // ramp
    if (y < 80) return 255;
    if (y < 128) return (x / 24) * 255 / 15;                        // 16-step wedge
    if (y < 132) return 255;
    if (y < 292) {
        int dx = x - CIRCLE_CX, dy = y - CIRCLE_CY;
        if (dx * dx + dy * dy <= CIRCLE_R * CIRCLE_R) {             // radial gradient
            int r2 = dx * dx + dy * dy;
            return (uint8_t)(255 - 255 * (CIRCLE_R * CIRCLE_R - r2) / (CIRCLE_R * CIRCLE_R));
        }
        if (x >= 8 && x < 100) return (y & 1) ? 0 : 255;            // 1-dot lines
        if (x >= 284 && x < 376) return ((x + y) & 1) ? 0 : 255;    // 1-dot checker
        return 255;
    }
    if (y < 296) return 255;
    if (y < 320) {                                                  // vertical bars
        static const uint8_t widths[] = { 1, 2, 3, 4, 6, 8, 12 };
        int pos = 8;
        for (size_t i = 0; i < sizeof widths && pos < W - 8; i++) {
            for (int rep = 0; rep < 2; rep++) {
                if (x >= pos && x < pos + widths[i]) return 0;
                pos += widths[i] * 2;
            }
        }
        return 255;
    }
    if (y < 324) return 255;
    if (y < 336) return (x >= 8 && x < W - 8) ? 0 : 255;            // solid bar
    return 255;
}

esp_err_t testpattern_print(void)
{
    uint8_t *bits = calloc(H, PRINTER_ROW_BYTES);
    uint8_t *gray = malloc(W);
    ESP_RETURN_ON_FALSE(bits && gray, ESP_ERR_NO_MEM, TAG, "no memory");

    esp_err_t err = dither_begin(W);
    if (err == ESP_OK) {
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) gray[x] = pattern_gray(x, y);
            dither_row(gray, bits + (size_t)y * PRINTER_ROW_BYTES);
        }
        dither_end();
        ESP_LOGI(TAG, "%dx%d, %d bytes", W, H, H * PRINTER_ROW_BYTES);
        err = printjob_run(bits, H, 1);
    }
    free(gray);
    free(bits);
    return err;
}
