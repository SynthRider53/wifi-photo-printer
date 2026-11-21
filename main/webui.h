#pragma once

#include "esp_err.h"

// HTTP server: GET / (the page), GET /status, POST /print?h=<rows>&copies=<n>
// with a raw body of rows * 48 bytes (1 bit per dot, MSB first, 1 = black).
esp_err_t webui_start(void);
