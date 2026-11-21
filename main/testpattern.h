#pragma once

#include "esp_err.h"

// Prints a calibration strip: mm ruler, gray ramp, 16-step wedge, radial
// gradient, line/checker patterns and a solid bar. Safe to call from any task.
esp_err_t testpattern_print(void);
