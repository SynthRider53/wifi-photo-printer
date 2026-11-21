#include "printer.h"

#include <string.h>

#include "board.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "printer";

// Rows per GS v 0 command. Cheap printers choke on very tall bitmaps.
#define BAND_ROWS 64
#define BAND_MAX  255

static int s_baud = PRINTER_BAUD;
static int s_band = BAND_ROWS;
static int s_lines_per_sec = 250;       // ~31 mm/s, below the 50-85 mm/s rating
static int s_tail_dots = PRINTER_TAIL_DOTS;
static int s_settle_ms = 1500;          // quiet time before ESC J, see printer_feed()
// ESC 7 defaults (64 dots at once, 800 us, 20 us gap) and DC2 # n density 10 = 100%.
static int s_heat_dots = 7, s_heat_time = 80, s_heat_gap = 2, s_density = 10;
static SemaphoreHandle_t s_lock;

static esp_err_t write_all(const void *data, size_t len)
{
    int sent = uart_write_bytes(PRINTER_UART, data, len);
    return sent == (int)len ? ESP_OK : ESP_FAIL;
}

esp_err_t printer_init(int baud)
{
    s_baud = baud;
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    const uart_config_t cfg = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(PRINTER_UART, 256, 4096, 0, NULL, 0), TAG, "install");
    ESP_RETURN_ON_ERROR(uart_param_config(PRINTER_UART, &cfg), TAG, "config");
    ESP_RETURN_ON_ERROR(uart_set_pin(PRINTER_UART, PRINTER_TX_GPIO, PRINTER_RX_GPIO,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "pins");
    ESP_LOGI(TAG, "UART%d TX=GPIO%d RX=GPIO%d %d baud", PRINTER_UART, PRINTER_TX_GPIO,
             PRINTER_RX_GPIO, baud);
    return ESP_OK;
}

esp_err_t printer_set_baud(int baud)
{
    ESP_RETURN_ON_ERROR(uart_set_baudrate(PRINTER_UART, baud), TAG, "baud");
    s_baud = baud;
    return ESP_OK;
}

int printer_get_baud(void) { return s_baud; }

void printer_set_speed(int lines_per_sec)
{
    if (lines_per_sec >= 20 && lines_per_sec <= 2000) s_lines_per_sec = lines_per_sec;
}

int printer_get_speed(void) { return s_lines_per_sec; }

void printer_set_band(int rows)
{
    if (rows >= 1 && rows <= BAND_MAX) s_band = rows;
}

int printer_get_band(void) { return s_band; }

void printer_set_tail(int dots)
{
    if (dots >= 0 && dots <= 800) s_tail_dots = dots;
}

int printer_get_tail(void) { return s_tail_dots; }

void printer_set_settle(int ms)
{
    if (ms >= 0 && ms <= 10000) s_settle_ms = ms;
}

int printer_get_settle(void) { return s_settle_ms; }

esp_err_t printer_finish(void)
{
    return s_tail_dots > 0 ? printer_feed(s_tail_dots) : ESP_OK;
}

static esp_err_t send_tuning(void)
{
    const uint8_t heat[] = { 0x1B, '7', (uint8_t)s_heat_dots, (uint8_t)s_heat_time, (uint8_t)s_heat_gap };
    const uint8_t dens[] = { 0x12, '#', (uint8_t)s_density };      // DC2 # n
    ESP_RETURN_ON_ERROR(write_all(heat, sizeof heat), TAG, "heat");
    return write_all(dens, sizeof dens);
}

esp_err_t printer_reset(void)
{
    static const uint8_t cmd[] = { 0x1B, '@' };
    ESP_RETURN_ON_ERROR(write_all(cmd, sizeof cmd), TAG, "reset");
    return send_tuning();
}

esp_err_t printer_set_heat(int dots_n, int time, int gap)
{
    ESP_RETURN_ON_FALSE(dots_n >= 0 && dots_n <= 255 && time >= 3 && time <= 255 &&
                        gap >= 0 && gap <= 255, ESP_ERR_INVALID_ARG, TAG, "range");
    s_heat_dots = dots_n; s_heat_time = time; s_heat_gap = gap;
    return send_tuning();
}

esp_err_t printer_set_density(int n)
{
    ESP_RETURN_ON_FALSE(n >= 0 && n <= 31, ESP_ERR_INVALID_ARG, TAG, "range");
    s_density = n;
    return send_tuning();
}

void printer_get_tuning(int *dots_n, int *time, int *gap, int *density)
{
    *dots_n = s_heat_dots; *time = s_heat_time; *gap = s_heat_gap; *density = s_density;
}

esp_err_t printer_raw(const void *data, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = write_all(data, len);
    uart_wait_tx_done(PRINTER_UART, pdMS_TO_TICKS(2000));
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t printer_text(const char *s)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = write_all(s, strlen(s));
    if (err == ESP_OK) err = write_all("\n", 1);
    uart_wait_tx_done(PRINTER_UART, pdMS_TO_TICKS(2000));
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t printer_feed(int lines)
{
    // ESC J and blank raster rows do not move the paper on this printer, while
    // Measured on the QR204: ESC J n feeds n dots (80 -> 9 mm), but only once the
    // printer has finished the previous raster; LF ignores ESC 3 and blank
    // raster rows do not move the paper at all.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(s_settle_ms));
    esp_err_t err = ESP_OK;
    for (int left = lines; left > 0 && err == ESP_OK; left -= 255) {
        int n = left > 255 ? 255 : left;
        const uint8_t cmd[] = { 0x1B, 'J', (uint8_t)n };
        err = write_all(cmd, sizeof cmd);
        uart_wait_tx_done(PRINTER_UART, pdMS_TO_TICKS(2000));
        vTaskDelay(pdMS_TO_TICKS(n * 1000 / s_lines_per_sec + 100));
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t printer_raster(const uint8_t *rows, int height)
{
    ESP_RETURN_ON_FALSE(rows && height > 0, ESP_ERR_INVALID_ARG, TAG, "args");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;

    const int band = s_band;
    for (int y = 0; y < height && err == ESP_OK; y += band) {
        int n = height - y < band ? height - y : band;
        int64_t t0 = esp_timer_get_time();

        // GS v 0 m xL xH yL yH d1...dk
        const uint8_t head[] = { 0x1D, 'v', '0', 0, PRINTER_ROW_BYTES, 0, (uint8_t)n, (uint8_t)(n >> 8) };
        err = write_all(head, sizeof head);
        if (err == ESP_OK) err = write_all(rows + (size_t)y * PRINTER_ROW_BYTES, (size_t)n * PRINTER_ROW_BYTES);
        uart_wait_tx_done(PRINTER_UART, pdMS_TO_TICKS(30000));

        // The line has no flow control: do not outrun the print head.
        int64_t want_us = (int64_t)n * 1000000 / s_lines_per_sec;
        int64_t spent_us = esp_timer_get_time() - t0;
        if (spent_us < want_us) vTaskDelay(pdMS_TO_TICKS((want_us - spent_us) / 1000));
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t printer_status(int n, uint8_t *out, int timeout_ms)
{
    ESP_RETURN_ON_FALSE(n >= 1 && n <= 4 && out, ESP_ERR_INVALID_ARG, TAG, "args");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uart_flush_input(PRINTER_UART);
    const uint8_t cmd[] = { 0x10, 0x04, (uint8_t)n };      // DLE EOT n
    esp_err_t err = write_all(cmd, sizeof cmd);
    if (err == ESP_OK) {
        int got = uart_read_bytes(PRINTER_UART, out, 1, pdMS_TO_TICKS(timeout_ms));
        err = got == 1 ? ESP_OK : ESP_ERR_TIMEOUT;
    }
    xSemaphoreGive(s_lock);
    return err;
}
