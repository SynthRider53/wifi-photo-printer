#include "instax.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "dither.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "jpegprint.h"
#include "lwip/sockets.h"
#include "printer.h"
#include "printjob.h"

static const char *TAG = "instax";

#define PORT          8080
#define START_CMD     0x24          // packet from the camera
#define START_RESP    0x2a          // packet from the printer
#define HDR_CMD       12            // start, type, length, session, password, 00 00
#define HDR_RESP      16            // ... return code, ejecting, battery/prints
#define FOOTER        4             // checksum + CR LF

// Message types (instax SHARE SP-2).
#define MSG_SPECS     79
#define MSG_RESET     80
#define MSG_PREP      81
#define MSG_SEND      82
#define MSG_PRINT     83
#define MSG_LOCKSTATE 176
#define MSG_LOCK      179
#define MSG_PASSWORD  182
#define MSG_VERSION   192
#define MSG_COUNT     193
#define MSG_MODEL     194
#define MSG_STATUS    195
#define MSG_PREPRINT  196

#define RC_OK         0             // RTN_E_RCV_FRAME
#define RC_PRINTING   163
#define RC_NO_IMAGE   161

#define BATTERY       3             // 0..7; a full SP-2 answers 3
#define PRINTS_LEFT   10            // 0..15

#define MSG_FUNCTION  64            // "function version", like 196 but another family

#define FMT_JPEG      2             // what a Fujifilm camera sends
#define FMT_RGB       16            // what the instax SHARE app sends

// Picture as the app sends it raw: 800 blocks of 600 R, 600 G, 600 B.
#define IMG_COLS      800
#define IMG_ROWS      600
#define COL_BYTES     (IMG_ROWS * 3)
#define SEG_BYTES     60000
#define OUT_ROWS      (IMG_COLS * PRINTER_WIDTH_PX / IMG_ROWS)   // 512 rows, 64 mm
#define JPEG_MAX      (400 * 1024)
#define RING_BYTES    16384         // JPEG bytes waiting to be decoded

typedef struct {
    bool open;
    int format;
    uint32_t announced, got;

    // A JPEG is decoded while it arrives: the pieces travel through a stream
    // buffer to the decoder task, so the whole file never sits in RAM.
    StreamBufferHandle_t ring;
    SemaphoreHandle_t done;
    volatile bool eof, decoding;
    uint8_t *out_bits;
    int out_rows;
    esp_err_t out_err;

    uint8_t col[COL_BYTES];     // raw RGB, reassembled across segment borders
    int col_len, next_col;
    uint16_t *sum;
    uint8_t *cnt, *gray, *bits;
    int cur_row;                // target row the sums belong to, -1 = none
} img_t;

static img_t s_img = { .cur_row = -1 };
static int64_t s_print_started;
static int s_function_version;      // answer to message 64, tunable from the console
static int s_chunk = 480;           // size of the parts we ask the camera to send

// ---------------------------------------------------------------- picture

// Pulls JPEG bytes for the decoder task; 0 once the camera is done sending.
static int ring_read(void *ctx, uint8_t *buf, size_t len)
{
    (void)ctx;
    for (;;) {
        size_t n = xStreamBufferReceive(s_img.ring, buf, len, pdMS_TO_TICKS(200));
        if (n) return n;
        if (s_img.eof) return 0;
    }
}

static void decode_task(void *arg)
{
    s_img.out_err = jpegprint_decode(ring_read, NULL, &s_img.out_bits, &s_img.out_rows);
    s_img.decoding = false;                 // further bytes from the camera are dropped
    xSemaphoreGive(s_img.done);
    vTaskDelete(NULL);
}

// Lets the decoder task finish and collects what it produced.
static void decoder_stop(void)
{
    if (!s_img.ring) return;
    s_img.eof = true;
    if (s_img.done && xSemaphoreTake(s_img.done, pdMS_TO_TICKS(30000)) != pdTRUE)
        ESP_LOGE(TAG, "the decoder did not finish");
    vStreamBufferDelete(s_img.ring);
    s_img.ring = NULL;
    if (s_img.done) {
        vSemaphoreDelete(s_img.done);
        s_img.done = NULL;
    }
}

static void img_free(void)
{
    decoder_stop();
    s_img.decoding = false;
    free(s_img.out_bits);
    s_img.out_bits = NULL;
    free(s_img.sum);
    free(s_img.cnt);
    free(s_img.gray);
    free(s_img.bits);
    s_img.sum = NULL;
    s_img.cnt = NULL;
    s_img.gray = NULL;
    s_img.bits = NULL;
    if (s_img.open && s_img.format == FMT_RGB) dither_end();
    s_img.open = false;
}

static bool img_begin(int format, uint32_t length)
{
    img_free();
    s_img.format = format;
    s_img.announced = length;
    s_img.got = 0;
    s_img.col_len = 0;
    s_img.next_col = 0;
    s_img.cur_row = -1;
    s_img.open = true;

    if (format == FMT_RGB) {
        s_img.sum = calloc(PRINTER_WIDTH_PX, sizeof *s_img.sum);
        s_img.cnt = calloc(PRINTER_WIDTH_PX, 1);
        s_img.gray = malloc(PRINTER_WIDTH_PX);
        s_img.bits = calloc(OUT_ROWS, PRINTER_ROW_BYTES);
        if (!s_img.sum || !s_img.cnt || !s_img.gray || !s_img.bits || dither_begin(PRINTER_WIDTH_PX) != ESP_OK) {
            ESP_LOGE(TAG, "no memory for a %dx%d picture", PRINTER_WIDTH_PX, OUT_ROWS);
            s_img.format = 0;       // dither_end() must not run, dither_begin failed
            img_free();
            return false;
        }
        return true;
    }
    if (length > JPEG_MAX) {
        ESP_LOGE(TAG, "picture of %u bytes is too big", (unsigned)length);
        img_free();
        return false;
    }
    s_img.eof = false;
    s_img.ring = xStreamBufferCreate(RING_BYTES, 1);
    s_img.done = xSemaphoreCreateBinary();
    if (!s_img.ring || !s_img.done || xTaskCreate(decode_task, "jpegdec", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot start the decoder");
        img_free();
        return false;
    }
    s_img.decoding = true;
    return true;
}

static void img_flush_row(void)
{
    if (s_img.cur_row < 0) return;
    for (int x = 0; x < PRINTER_WIDTH_PX; x++) {
        s_img.gray[x] = s_img.cnt[x] ? s_img.sum[x] / s_img.cnt[x] : 255;
        s_img.sum[x] = 0;
        s_img.cnt[x] = 0;
    }
    dither_row(s_img.gray, s_img.bits + (size_t)s_img.cur_row * PRINTER_ROW_BYTES);
    s_img.cur_row = -1;
}

// One finished block = one row of the upright picture. The blocks arrive
// bottom row first, so the target row index counts down.
static void img_col(void)
{
    int y = IMG_COLS - 1 - s_img.next_col;
    int ty = y * OUT_ROWS / IMG_COLS;
    if (ty != s_img.cur_row) {
        img_flush_row();
        s_img.cur_row = ty;
    }
    const uint8_t *r = s_img.col, *g = r + IMG_ROWS, *b = g + IMG_ROWS;
    for (int h = 0; h < IMG_ROWS; h++) {
        int tx = h * PRINTER_WIDTH_PX / IMG_ROWS;
        s_img.sum[tx] += (r[h] * 77 + g[h] * 150 + b[h] * 29) >> 8;
        s_img.cnt[tx]++;
    }
    s_img.next_col++;
}

static bool img_feed(const uint8_t *buf, size_t n)
{
    if (!s_img.open) return false;
    // The last part is padded out to the full size; the announced length says
    // where the picture really ends.
    if (s_img.got + n > s_img.announced) n = s_img.announced - s_img.got;
    if (!n) return true;
    s_img.got += n;
    if (s_img.format != FMT_RGB) {
        // The decoder stops at the end-of-image marker; anything after it is padding.
        if (s_img.decoding) xStreamBufferSend(s_img.ring, buf, n, pdMS_TO_TICKS(10000));
        return true;
    }
    while (n) {
        size_t want = COL_BYTES - s_img.col_len;
        if (want > n) want = n;
        memcpy(s_img.col + s_img.col_len, buf, want);
        s_img.col_len += want;
        buf += want;
        n -= want;
        if (s_img.col_len == COL_BYTES) {
            s_img.col_len = 0;
            if (s_img.next_col < IMG_COLS) img_col();
        }
    }
    return true;
}

static void img_print(void)
{
    if (!s_img.open) { ESP_LOGW(TAG, "print asked for, but no picture arrived"); return; }
    ESP_LOGI(TAG, "picture complete: %u of %u bytes (heap %u)", (unsigned)s_img.got, (unsigned)s_img.announced,
             (unsigned)esp_get_free_heap_size());

    uint8_t *bits = NULL;
    int rows = 0;
    esp_err_t err = ESP_OK;
    if (s_img.format == FMT_RGB) {
        img_flush_row();
        bits = s_img.bits;
        rows = OUT_ROWS;
        s_img.bits = NULL;                  // printjob takes it over
    } else {
        decoder_stop();
        bits = s_img.out_bits;
        rows = s_img.out_rows;
        err = s_img.out_err;
        s_img.out_bits = NULL;
    }
    img_free();
    if (err == ESP_OK) err = printjob_submit(bits, rows, 1);
    s_print_started = esp_timer_get_time();
    ESP_LOGI(TAG, "print job: %d rows, %s", rows, esp_err_to_name(err));
}

// The camera waits for the print to finish. Our paper takes about half a
// minute, longer than instax film, so stop claiming "printing" after 20 s and
// let the camera close the session while the paper runs out.
static bool still_printing(void)
{
    printjob_status_t st;
    printjob_status(&st);
    return st.busy && esp_timer_get_time() - s_print_started < 20 * 1000000LL;
}

// ---------------------------------------------------------------- packets

static uint32_t be32(const uint8_t *d) { return ((uint32_t)d[0] << 24) | (d[1] << 16) | (d[2] << 8) | d[3]; }
static uint16_t be16(const uint8_t *d) { return (d[0] << 8) | d[1]; }
static void put_be16(uint8_t *d, uint16_t v) { d[0] = v >> 8; d[1] = v; }
static void put_be32(uint8_t *d, uint32_t v) { d[0] = v >> 24; d[1] = v >> 16; d[2] = v >> 8; d[3] = v; }

static int read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        int r = recv(fd, p, n, 0);
        if (r <= 0) return -1;
        p += r;
        n -= r;
    }
    return 0;
}

static int send_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        int r = send(fd, p, n, 0);
        if (r <= 0) return -1;
        p += r;
        n -= r;
    }
    return 0;
}

static int reply(int fd, uint8_t type, uint32_t session, uint8_t rc, const uint8_t *pl, int pl_len)
{
    uint8_t p[64];
    int len = HDR_RESP + pl_len + FOOTER;
    p[0] = START_RESP;
    p[1] = type;
    put_be16(p + 2, len);
    put_be32(p + 4, session);
    memset(p + 8, 0, 4);
    p[12] = rc;
    p[13] = 0;
    p[14] = 0;                                      // not ejecting
    p[15] = (BATTERY << 4) | PRINTS_LEFT;
    if (pl_len) memcpy(p + HDR_RESP, pl, pl_len);
    uint16_t sum = 0;
    for (int i = 0; i < HDR_RESP + pl_len; i++) sum += p[i];
    sum = ~sum;
    put_be16(p + HDR_RESP + pl_len, sum);
    p[HDR_RESP + pl_len + 2] = 13;
    p[HDR_RESP + pl_len + 3] = 10;
    return send_full(fd, p, len);
}

static const char *type_name(uint8_t t)
{
    switch (t) {
    case MSG_SPECS:     return "specifications";
    case MSG_RESET:     return "reset";
    case MSG_PREP:      return "prepare image";
    case MSG_SEND:      return "send image";
    case MSG_PRINT:     return "print";
    case MSG_FUNCTION:  return "function version";
    case MSG_LOCKSTATE: return "lock state";
    case MSG_LOCK:      return "lock";
    case MSG_PASSWORD:  return "change password";
    case MSG_VERSION:   return "version";
    case MSG_COUNT:     return "print count";
    case MSG_MODEL:     return "model name";
    case MSG_STATUS:    return "print status";
    case MSG_PREPRINT:  return "pre-print";
    default:            return "unknown";
    }
}

// Handles one packet. Returns 0 to keep the connection, -1 to drop it.
static int handle_packet(int fd)
{
    uint8_t h[HDR_CMD];
    if (read_full(fd, h, sizeof h) != 0) return -1;
    if (h[0] != START_CMD) {
        ESP_LOGW(TAG, "packet does not start with 0x24 (0x%02x)", h[0]);
        return -1;
    }
    uint8_t type = h[1];
    int len = be16(h + 2);
    uint32_t session = be32(h + 4);
    int pl_len = len - HDR_CMD - FOOTER;
    if (len < HDR_CMD + FOOTER || pl_len < 0) {
        ESP_LOGW(TAG, "%s: bad length %d", type_name(type), len);
        return -1;
    }

    uint8_t out[32] = { 0 };
    int out_len = 0;
    uint8_t rc = RC_OK;

    if (type == MSG_SEND) {
        // Streamed: only the 4-byte sequence number is kept, the pixels go
        // straight into the picture so that megabytes never sit in RAM.
        uint8_t seq[4];
        if (read_full(fd, seq, 4) != 0) return -1;
        bool ok = true;
        for (int left = pl_len - 4; left > 0;) {
            uint8_t buf[512];
            int want = left < (int)sizeof buf ? left : (int)sizeof buf;
            if (read_full(fd, buf, want) != 0) return -1;
            if (ok) ok = img_feed(buf, want);
            left -= want;
        }
        uint8_t foot[FOOTER];
        if (read_full(fd, foot, FOOTER) != 0) return -1;
        uint32_t n = be32(seq);
        ESP_LOGI(TAG, "<- image part %u (%d bytes, %u total)", (unsigned)n, pl_len - 4, (unsigned)s_img.got);
        out[3] = n & 0xFF;
        return reply(fd, type, session, ok ? RC_OK : RC_NO_IMAGE, out, 4);
    }

    uint8_t pl[32] = { 0 };
    int kept = pl_len < (int)sizeof pl ? pl_len : (int)sizeof pl;
    if (kept && read_full(fd, pl, kept) != 0) return -1;
    for (int left = pl_len - kept; left > 0;) {     // nothing we need, drop it
        uint8_t sink[64];
        int want = left < (int)sizeof sink ? left : (int)sizeof sink;
        if (read_full(fd, sink, want) != 0) return -1;
        left -= want;
    }
    uint8_t foot[FOOTER];
    if (read_full(fd, foot, FOOTER) != 0) return -1;

    char hex[3 * 16 + 1];
    int k = 0;
    for (int i = 0; i < kept && i < 16; i++) k += snprintf(hex + k, sizeof hex - k, "%02x ", pl[i]);
    hex[k] = 0;
    ESP_LOGI(TAG, "<- %s (type %u, password %u) [%s]", type_name(type), type, be16(h + 8), hex);

    switch (type) {
    case MSG_VERSION:
        put_be16(out, 254);                         // unknown, as the SP-2 answers
        put_be16(out + 2, 275);                     // firmware 01.13
        put_be16(out + 4, 0);                       // hardware
        out_len = 8;
        break;
    case MSG_MODEL:
        memcpy(out, "SP-2", 4);
        out_len = 4;
        break;
    case MSG_SPECS:
        put_be16(out, IMG_ROWS);                    // picture width
        put_be16(out + 2, IMG_COLS);                // picture height
        put_be16(out + 4, 256);                     // colours
        put_be16(out + 6, 10);
        put_be16(out + 12, s_chunk);                // biggest part we accept
        out[14] = 16;
        out_len = 28;
        break;
    case MSG_COUNT:
        put_be32(out, 20);                          // pictures printed in this printer's life
        out_len = 16;
        break;
    case MSG_PREPRINT: {
        // "Which version of function <n> do you have?" -- the camera keeps the
        // answers and picks how to talk to us from them.
        uint16_t cmd = be16(pl + 2);
        uint16_t resp = cmd >= 6 ? 0 : (cmd >= 4 ? 1 : 2);
        put_be16(out, cmd);
        put_be16(out + 2, resp);
        out_len = 4;
        break;
    }
    case MSG_FUNCTION:
        put_be16(out, be16(pl + 2));
        put_be16(out + 2, s_function_version);
        out_len = 4;
        break;
    case MSG_PREP:
        ESP_LOGI(TAG, "picture announced: format %u, options 0x%02x, %u bytes", pl[0], pl[1],
                 (unsigned)be32(pl + 2));
        if (!img_begin(pl[0], be32(pl + 2))) rc = RC_NO_IMAGE;
        put_be32(out, s_chunk);     // readable both as a 32-bit value and as a 16-bit one at offset 2
        out_len = 4;
        break;
    case MSG_PRINT:
        img_print();
        break;
    case MSG_STATUS:
        rc = still_printing() ? RC_PRINTING : RC_OK;
        break;
    case MSG_LOCKSTATE:
        put_be32(out, 100);
        out_len = 4;
        break;
    case MSG_RESET:
        img_free();
        break;
    case MSG_LOCK:
    case MSG_PASSWORD:
        break;
    default:
        // Unknown to us: echo the payload back, which is what the documented
        // commands of this family do, and hope the camera carries on.
        memcpy(out, pl, kept);
        out_len = kept;
        break;
    }
    return reply(fd, type, session, rc, out, out_len);
}

static void instax_task(void *arg)
{
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = INADDR_ANY };
    if (srv < 0 || bind(srv, (struct sockaddr *)&me, sizeof me) != 0 || listen(srv, 2) != 0) {
        ESP_LOGE(TAG, "cannot listen on port %d", PORT);
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "instax SHARE printer ready on port %d", PORT);

    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int fd = accept(srv, (struct sockaddr *)&from, &fl);
        if (fd < 0) continue;
        struct timeval tv = { .tv_sec = 30 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        ESP_LOGI(TAG, "camera %s connected (heap %u)", inet_ntoa(from.sin_addr), (unsigned)esp_get_free_heap_size());
        while (handle_packet(fd) == 0) { }
        close(fd);
        if (s_img.open && s_img.got < s_img.announced) {    // half-sent picture, let the memory go
            ESP_LOGW(TAG, "connection dropped with %u of %u bytes", (unsigned)s_img.got, (unsigned)s_img.announced);
            img_free();
        }
        ESP_LOGI(TAG, "camera disconnected (heap %u)", (unsigned)esp_get_free_heap_size());
    }
}

void instax_set_function_version(int v)
{
    s_function_version = v;
}

int instax_get_function_version(void)
{
    return s_function_version;
}

void instax_set_chunk(int bytes)
{
    if (bytes > 0 && bytes <= SEG_BYTES) s_chunk = bytes;
}

int instax_get_chunk(void)
{
    return s_chunk;
}

esp_err_t instax_start(void)
{
    return xTaskCreate(instax_task, "instax", 4096, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
