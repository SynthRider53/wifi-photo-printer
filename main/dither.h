#pragma once

#include <stdint.h>
#include "esp_err.h"

// Streaming Floyd-Steinberg: feed 8-bit gray rows top to bottom, get packed
// 1-bit rows (MSB first, 1 = black). Keeps only two rows of error in RAM, so
// a picture can be dithered while it is being decoded.
typedef enum { DITHER_FS, DITHER_BAYER } dither_mode_t;

// Floyd-Steinberg gives the finest detail; Bayer 8x8 ordered dither is more
// even on thermal heads, where isolated dots print darker than their neighbours.
void      dither_set_mode(dither_mode_t mode);
dither_mode_t dither_get_mode(void);

esp_err_t dither_begin(int width);
void      dither_row(const uint8_t *gray, uint8_t *bits);
void      dither_end(void);
