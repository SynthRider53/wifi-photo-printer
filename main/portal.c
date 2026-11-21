#include "portal.h"

#include <string.h>

#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "portal";

#define DNS_PORT   53
#define DNS_MAX    512
#define ANSWER_LEN 16

// What a phone shows in its "sign in to network" window. Deliberately not the
// printing page: that window is a cut-down browser which cannot open files.
static const char PAGE[] =
    "<!doctype html><html lang=\"ru\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Фотопринтер</title></head>"
    "<body style=\"font:17px/1.5 system-ui,sans-serif;margin:0;padding:24px;text-align:center\">"
    "<h1 style=\"font-size:20px\">Принтер подключён</h1>"
    "<p>Это окно можно закрыть.</p>"
    "<p>Чтобы выбрать фото, текст или PDF, откройте в обычном браузере:</p>"
    "<p style=\"font-size:22px;font-weight:600\">http://" WIFI_AP_IP "/</p>"
    "</body></html>";

// Answers every question with the board's own address.
static void dns_task(void *arg)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(DNS_PORT), .sin_addr.s_addr = INADDR_ANY };
    if (s < 0 || bind(s, (struct sockaddr *)&me, sizeof me) != 0) {
        ESP_LOGE(TAG, "cannot listen on port %d", DNS_PORT);
        vTaskDelete(NULL);
    }
    uint32_t ip = ipaddr_addr(WIFI_AP_IP);
    uint8_t buf[DNS_MAX];
    ESP_LOGI(TAG, "name server ready, everything points at " WIFI_AP_IP);

    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(s, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n < 17 || (buf[2] & 0x80)) continue;        // too short, or already an answer

        int q = 12;                                     // header, then the question's labels
        while (q < n && buf[q]) q += buf[q] + 1;
        q += 5;                                         // closing zero, type, class
        if (q > n || q + ANSWER_LEN > (int)sizeof buf) continue;
        bool is_a = buf[q - 4] == 0 && buf[q - 3] == 1; // only plain addresses

        buf[2] |= 0x84;                                 // this is an answer, from the authority
        buf[3] = 0;
        buf[6] = 0;
        buf[7] = is_a ? 1 : 0;
        memset(buf + 8, 0, 4);                          // no other sections
        if (!is_a) {
            sendto(s, buf, q, 0, (struct sockaddr *)&from, fl);
            continue;
        }
        uint8_t *a = buf + q;
        a[0] = 0xC0; a[1] = 0x0C;                       // the name asked about
        a[2] = 0x00; a[3] = 0x01;                       // address
        a[4] = 0x00; a[5] = 0x01;                       // internet
        a[6] = a[7] = a[8] = 0; a[9] = 60;              // keep for a minute
        a[10] = 0x00; a[11] = 0x04;
        memcpy(a + 12, &ip, 4);
        sendto(s, buf, q + ANSWER_LEN, 0, (struct sockaddr *)&from, fl);
    }
}

// A phone's internet check wants an empty "204" answer; a page instead tells it
// the network needs a sign-in, and it stays connected.
static esp_err_t sign_in_page(httpd_req_t *req, httpd_err_code_t err)
{
    ESP_LOGI(TAG, "internet check: %s", req->uri);
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

esp_err_t portal_http_register(httpd_handle_t srv)
{
    return httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, sign_in_page);
}

esp_err_t portal_start(void)
{
    return xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
