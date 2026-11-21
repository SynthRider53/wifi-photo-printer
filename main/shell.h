#pragma once

#include "esp_err.h"

// Console on USB Serial/JTAG: test, feed, status, baud, speed, init.
esp_err_t shell_start(void);
