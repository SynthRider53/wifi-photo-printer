#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Pulls JPEG bytes from the source: >0 bytes read, 0 end of data, <0 error.
typedef int (*jpegprint_read_fn)(void *ctx, uint8_t *buf, size_t len);

// Decodes a baseline JPEG of any size straight from a stream (ROM TJpgDec with
// 1/1..1/8 downscale), shrinks it so the short side fits the paper width,
// dithers it row by row and rotates it so the short side runs across the paper.
// Never holds more than a few dozen KB: a 26 MP camera file is fine.
// On success *bits is a heap buffer of *rows x PRINTER_ROW_BYTES (1 = black),
// ready for printjob_submit().
esp_err_t jpegprint_decode(jpegprint_read_fn rd, void *ctx, uint8_t **bits, int *rows);
