#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// One picture at a time. Everything that prints a picture goes through here,
// so the web page, the BOOT button and the console cannot interleave bands.

typedef struct {
    bool busy;
    int  rows_done;     // rows of the current copy already sent
    int  rows_total;    // rows per copy
    int  copy;          // 1-based number of the copy being printed
    int  copies;
} printjob_status_t;

// Prints `copies` copies and blocks until finished. ESP_ERR_INVALID_STATE if busy.
esp_err_t printjob_run(const uint8_t *bits, int rows, int copies);

// Takes ownership of `bits` (heap, freed after printing; also freed on error).
// Returns at once; ESP_ERR_INVALID_STATE if busy.
esp_err_t printjob_submit(uint8_t *bits, int rows, int copies);

void printjob_status(printjob_status_t *st);
