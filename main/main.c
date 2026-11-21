#include "board.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "instax.h"
#include "pictbridge.h"
#include "printer.h"
#include "shell.h"
#include "testpattern.h"
#include "webui.h"
#include "wifi.h"

static const char *TAG = "main";

// BOOT button: one press prints the calibration strip.
static void button_task(void *arg)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cfg);

    for (;;) {
        if (gpio_get_level(BUTTON_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(30));
            if (gpio_get_level(BUTTON_GPIO) == 0) {
                ESP_LOGI(TAG, "button -> test print");
                testpattern_print();
                while (gpio_get_level(BUTTON_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(printer_init(PRINTER_BAUD));
    printer_reset();
    xTaskCreate(button_task, "button", 4096, NULL, 5, NULL);
    ESP_ERROR_CHECK(wifi_start());
    ESP_ERROR_CHECK(webui_start());
    ESP_ERROR_CHECK(pictbridge_start());
    ESP_ERROR_CHECK(instax_start());
    ESP_ERROR_CHECK(shell_start());
    ESP_LOGI(TAG, "ready: join \"%s\" (no password) and open http://" WIFI_AP_IP "/; "
             "BOOT button or 'test' prints the calibration strip", wifi_ap_ssid());
}
