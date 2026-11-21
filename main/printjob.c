#include "printjob.h"

#include <stdbool.h>
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "printer.h"

static const char *TAG = "printjob";

#define CHUNK_ROWS 64

static SemaphoreHandle_t s_free;        // binary: given = idle (may be taken/given from any task)
static volatile printjob_status_t s_st;

typedef struct { uint8_t *bits; int rows, copies; } job_t;

static bool claim(void)
{
    if (!s_free) {
        s_free = xSemaphoreCreateBinary();
        xSemaphoreGive(s_free);
    }
    return xSemaphoreTake(s_free, 0) == pdTRUE;
}

static esp_err_t print_claimed(const uint8_t *bits, int rows, int copies)
{
    esp_err_t err = ESP_OK;
    s_st.rows_total = rows;
    s_st.copies = copies;
    s_st.busy = true;
    for (int c = 1; c <= copies && err == ESP_OK; c++) {
        s_st.copy = c;
        s_st.rows_done = 0;
        for (int y = 0; y < rows && err == ESP_OK; y += CHUNK_ROWS) {
            int n = rows - y < CHUNK_ROWS ? rows - y : CHUNK_ROWS;
            err = printer_raster(bits + (size_t)y * PRINTER_ROW_BYTES, n);
            s_st.rows_done = y + n;
        }
        if (err == ESP_OK) err = printer_finish();
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "print failed: %s", esp_err_to_name(err));
    s_st.busy = false;
    xSemaphoreGive(s_free);
    return err;
}

esp_err_t printjob_run(const uint8_t *bits, int rows, int copies)
{
    if (!claim()) return ESP_ERR_INVALID_STATE;
    return print_claimed(bits, rows, copies);
}

static void job_task(void *arg)
{
    job_t *j = arg;
    print_claimed(j->bits, j->rows, j->copies);
    free(j->bits);
    free(j);
    vTaskDelete(NULL);
}

esp_err_t printjob_submit(uint8_t *bits, int rows, int copies)
{
    job_t *j = malloc(sizeof *j);
    if (!j || !claim()) {
        free(j);
        free(bits);
        return j ? ESP_ERR_INVALID_STATE : ESP_ERR_NO_MEM;
    }
    *j = (job_t){ bits, rows, copies };
    // Mark busy before the task starts so /status never shows a gap.
    s_st.busy = true;
    s_st.rows_total = rows;
    s_st.rows_done = 0;
    s_st.copy = 1;
    s_st.copies = copies;
    if (xTaskCreate(job_task, "printjob", 4096, j, 4, NULL) != pdPASS) {
        s_st.busy = false;
        xSemaphoreGive(s_free);
        free(bits);
        free(j);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void printjob_status(printjob_status_t *st)
{
    st->busy = s_st.busy;
    st->rows_done = s_st.rows_done;
    st->rows_total = s_st.rows_total;
    st->copy = s_st.copy;
    st->copies = s_st.copies;
}
