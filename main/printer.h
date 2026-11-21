#pragma once

#include <stdint.h>
#include "esp_err.h"

// QR204: 58 mm paper, 384 dots across at 203 dpi (8 dots/mm).
#define PRINTER_WIDTH_PX    384
#define PRINTER_ROW_BYTES   (PRINTER_WIDTH_PX / 8)

esp_err_t printer_init(int baud);
esp_err_t printer_set_baud(int baud);
int       printer_get_baud(void);

// Lines per second the host feeds the printer at most; lower it if dark
// pictures come out garbled (printer buffer overflow).
void      printer_set_speed(int lines_per_sec);
int       printer_get_speed(void);

// Rows per GS v 0 command. At 9600 baud the line carries about 20 rows/s while
// the head prints far faster, so the paper moves in bursts of this many rows.
void      printer_set_band(int rows);
int       printer_get_band(void);

// Feed after a finished picture: pushes the last rows out of the case.
void      printer_set_tail(int dots);
int       printer_get_tail(void);
esp_err_t printer_finish(void);
// Quiet time (ms) before ESC J: the printer drops it while still busy with raster.
void      printer_set_settle(int ms);
int       printer_get_settle(void);

// Print quality. ESC 7 n1 n2 n3: n1 = max dots heated at once is 8*(n1+1),
// n2 = heating time in 10 us units (3..255), n3 = gap in 10 us units.
// DC2 # n: density 0..31 (low 5 bits). Re-sent after every printer_reset().
esp_err_t printer_set_heat(int dots_n, int time, int gap);
esp_err_t printer_set_density(int n);
void      printer_get_tuning(int *dots_n, int *time, int *gap, int *density);

esp_err_t printer_raw(const void *data, size_t len);   // bytes as they are, waits for TX
esp_err_t printer_text(const char *s);      // plain text line, for baud probing

esp_err_t printer_reset(void);              // ESC @
esp_err_t printer_feed(int lines);          // ESC J, in dots

// Raster image PRINTER_WIDTH_PX wide, 1 bit per dot, MSB first, 1 = black,
// PRINTER_ROW_BYTES per row. Sent in bands, paced to printer speed.
esp_err_t printer_raster(const uint8_t *rows, int height);

// Real-time status (DLE EOT n, n = 1..4). Needs the printer TXD wired.
// Returns ESP_ERR_TIMEOUT when the printer does not answer.
esp_err_t printer_status(int n, uint8_t *out, int timeout_ms);
