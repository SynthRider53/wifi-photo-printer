#include "webui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "pictbridge.h"
#include "printer.h"
#include "printjob.h"

static const char *TAG = "webui";

extern const char index_html_start[] asm("_binary_index_html_start");

// The PDF viewer the page loads, kept gzipped in the application image.
extern const uint8_t pdf_mjs_start[] asm("_binary_pdf_mjs_gz_start");
extern const uint8_t pdf_mjs_end[] asm("_binary_pdf_mjs_gz_end");
extern const uint8_t pdf_worker_start[] asm("_binary_pdf_worker_mjs_gz_start");
extern const uint8_t pdf_worker_end[] asm("_binary_pdf_worker_mjs_gz_end");

static esp_err_t send_script(httpd_req_t *req, const uint8_t *start, const uint8_t *end)
{
    httpd_resp_set_type(req, "text/javascript");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    for (size_t off = 0, len = end - start; off < len;) {   // flash in bite-sized pieces
        size_t n = len - off < 8192 ? len - off : 8192;
        if (httpd_resp_send_chunk(req, (const char *)start + off, n) != ESP_OK) return ESP_FAIL;
        off += n;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t get_pdfjs(httpd_req_t *req)
{
    return send_script(req, pdf_mjs_start, pdf_mjs_end);
}

static esp_err_t get_pdfworker(httpd_req_t *req)
{
    return send_script(req, pdf_worker_start, pdf_worker_end);
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t get_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, index_html_start);
}

static esp_err_t get_status(httpd_req_t *req)
{
    printjob_status_t st;
    printjob_status(&st);
    char json[200];
    snprintf(json, sizeof json,
             "{\"busy\":%s,\"done\":%d,\"total\":%d,\"copy\":%d,\"copies\":%d,"
             "\"width\":%d,\"maxrows\":%d,\"baud\":%d}",
             st.busy ? "true" : "false", st.rows_done, st.rows_total, st.copy, st.copies,
             PRINTER_WIDTH_PX, JOB_MAX_ROWS, printer_get_baud());
    return send_json(req, "200 OK", json);
}

static int query_int(httpd_req_t *req, const char *key, int fallback)
{
    char q[64], v[12];
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK) return fallback;
    if (httpd_query_key_value(q, key, v, sizeof v) != ESP_OK) return fallback;
    return atoi(v);
}

static esp_err_t post_print(httpd_req_t *req)
{
    int rows = query_int(req, "h", 0);
    int copies = query_int(req, "copies", 1);
    if (copies < 1) copies = 1;
    if (copies > 20) copies = 20;
    size_t want = (size_t)rows * PRINTER_ROW_BYTES;

    if (rows < 1 || rows > JOB_MAX_ROWS || req->content_len != want) {
        return send_json(req, "400 Bad Request", "{\"error\":\"bad size\"}");
    }

    printjob_status_t st;
    printjob_status(&st);
    if (st.busy) return send_json(req, "503 Service Unavailable", "{\"error\":\"busy\"}");

    uint8_t *bits = malloc(want);
    if (!bits) return send_json(req, "507 Insufficient Storage", "{\"error\":\"no memory\"}");

    for (size_t got = 0; got < want;) {
        int n = httpd_req_recv(req, (char *)bits + got, want - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            free(bits);
            ESP_LOGW(TAG, "upload aborted after %u of %u bytes", (unsigned)got, (unsigned)want);
            return ESP_FAIL;
        }
        got += n;
    }

    esp_err_t err = printjob_submit(bits, rows, copies);   // takes the buffer
    if (err == ESP_ERR_INVALID_STATE) return send_json(req, "503 Service Unavailable", "{\"error\":\"busy\"}");
    if (err != ESP_OK) return send_json(req, "500 Internal Server Error", "{\"error\":\"start failed\"}");
    ESP_LOGI(TAG, "print job: %d rows x %d", rows, copies);
    return send_json(req, "200 OK", "{\"ok\":true}");
}

esp_err_t webui_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.recv_wait_timeout = 15;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 12;

    httpd_handle_t srv;
    ESP_RETURN_ON_ERROR(httpd_start(&srv, &cfg), TAG, "start");

    const httpd_uri_t uris[] = {
        { .uri = "/",       .method = HTTP_GET,  .handler = get_index },
        { .uri = "/status", .method = HTTP_GET,  .handler = get_status },
        { .uri = "/print",  .method = HTTP_POST, .handler = post_print },
        { .uri = "/pdf.mjs",        .method = HTTP_GET, .handler = get_pdfjs },
        { .uri = "/pdf.worker.mjs", .method = HTTP_GET, .handler = get_pdfworker },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(srv, &uris[i]), TAG, "register");
    }
    return pictbridge_http_register(srv);
}
