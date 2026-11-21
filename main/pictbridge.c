#include "pictbridge.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jpegprint.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "printjob.h"

static const char *TAG = "pictbridge";

#define SSDP_GROUP   "239.255.255.250"
#define SSDP_PORT    1900
#define PTP_PORT     15740
#define DEV_TYPE     "urn:schemas-cipa-jp:device:DPSPrinter:1"
#define SVC_TYPE     "urn:schemas-cipa-jp:service:DPSConnectionManager:1"
#define SVC_ID       "urn:schemas-cipa-jp:serviceId:DPSConnectionManager"
#define CONTROL_URL  "/root/soap_folder"
#define BUF_CAP      4096
#define XML_CAP      3072

static char s_udn[64];
static volatile bool s_active;      // a PTP-IP session is running

// ---------------------------------------------------------------- helpers

static void put16(uint8_t *d, uint16_t v) { d[0] = v; d[1] = v >> 8; }
static void put32(uint8_t *d, uint32_t v) { for (int i = 0; i < 4; i++) d[i] = v >> (8 * i); }
static uint16_t get16(const uint8_t *d) { return d[0] | (d[1] << 8); }
static uint32_t get32(const uint8_t *d) { return d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t)d[3] << 24); }

// PTP string: count of UTF-16 units including the terminator, then the units.
static size_t put_ptp_str(uint8_t *d, const char *s)
{
    size_t n = strlen(s);
    if (!n) { d[0] = 0; return 1; }
    d[0] = n + 1;
    for (size_t i = 0; i <= n; i++) { d[1 + 2 * i] = i < n ? s[i] : 0; d[2 + 2 * i] = 0; }
    return 1 + 2 * (n + 1);
}

static void get_ptp_str(const uint8_t *d, size_t len, size_t off, char *out, size_t cap)
{
    out[0] = 0;
    if (off >= len) return;
    size_t n = d[off], i = 0;
    for (; i + 1 < n && off + 1 + 2 * i + 1 < len && i + 1 < cap; i++) out[i] = d[off + 1 + 2 * i];
    out[i] = 0;
}

static bool netif_addr(const char *key, uint32_t *ip, uint32_t *mask)
{
    esp_netif_t *n = esp_netif_get_handle_from_ifkey(key);
    esp_netif_ip_info_t info;
    if (!n || esp_netif_get_ip_info(n, &info) != ESP_OK || info.ip.addr == 0) return false;
    *ip = info.ip.addr;
    *mask = info.netmask.addr;
    return true;
}

// Our address on the network `peer` (network byte order) lives on; the access point if unsure.
static void local_ip_for(uint32_t peer, char *out, size_t n)
{
    uint32_t ip = 0, mask = 0;
    if (netif_addr("WIFI_STA_DEF", &ip, &mask) && (ip & mask) == (peer & mask)) {
        // station side
    } else if (!netif_addr("WIFI_AP_DEF", &ip, &mask)) {
        ip = 0;
    }
    struct in_addr a = { .s_addr = ip };
    snprintf(out, n, "%s", inet_ntoa(a));
}

// Value of an HTTP-style header in a UDP message; "" when absent.
static void header_value(const char *msg, const char *name, char *out, size_t cap)
{
    out[0] = 0;
    size_t nl = strlen(name);
    for (const char *p = strstr(msg, "\r\n"); p; p = strstr(p + 2, "\r\n")) {
        const char *l = p + 2;
        if (strncasecmp(l, name, nl) == 0 && l[nl] == ':') {
            l += nl + 1;
            while (*l == ' ') l++;
            size_t i = 0;
            while (l[i] && l[i] != '\r' && l[i] != '\n' && i + 1 < cap) { out[i] = l[i]; i++; }
            out[i] = 0;
            return;
        }
    }
}

// ---------------------------------------------------------------- SSDP

static const char *ssdp_targets[] = { "upnp:rootdevice", NULL /* UDN */, DEV_TYPE, SVC_TYPE };

static void usn_for(const char *st, char *out, size_t n)
{
    if (strcmp(st, s_udn) == 0) snprintf(out, n, "%s", s_udn);
    else snprintf(out, n, "%s::%s", s_udn, st);
}

static void ssdp_reply(int s, const struct sockaddr_in *to, const char *ip, const char *st)
{
    char usn[160], msg[480];
    usn_for(st, usn, sizeof usn);
    int n = snprintf(msg, sizeof msg,
                     "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nEXT:\r\n"
                     "LOCATION: http://%s:80/desc.xml\r\nSERVER: ESP32/1.0 UPnP/1.0 PhotoPrinter/1.0\r\n"
                     "ST: %s\r\nUSN: %s\r\n\r\n", ip, st, usn);
    sendto(s, msg, n, 0, (const struct sockaddr *)to, sizeof *to);
}

static void ssdp_notify(int s, uint32_t ifip)
{
    struct in_addr a = { .s_addr = ifip };
    char ip[16];
    snprintf(ip, sizeof ip, "%s", inet_ntoa(a));
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &a, sizeof a);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(SSDP_PORT) };
    inet_aton(SSDP_GROUP, &to.sin_addr);
    for (size_t i = 0; i < sizeof ssdp_targets / sizeof *ssdp_targets; i++) {
        const char *nt = ssdp_targets[i] ? ssdp_targets[i] : s_udn;
        char usn[160], msg[480];
        usn_for(nt, usn, sizeof usn);
        int n = snprintf(msg, sizeof msg,
                         "NOTIFY * HTTP/1.1\r\nHOST: " SSDP_GROUP ":1900\r\nCACHE-CONTROL: max-age=1800\r\n"
                         "LOCATION: http://%s:80/desc.xml\r\nNT: %s\r\nNTS: ssdp:alive\r\n"
                         "SERVER: ESP32/1.0 UPnP/1.0 PhotoPrinter/1.0\r\nUSN: %s\r\n\r\n", ip, nt, usn);
        sendto(s, msg, n, 0, (struct sockaddr *)&to, sizeof to);
    }
}

static void ssdp_task(void *arg)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(SSDP_PORT), .sin_addr.s_addr = INADDR_ANY };
    if (s < 0 || bind(s, (struct sockaddr *)&me, sizeof me) != 0) {
        ESP_LOGE(TAG, "SSDP socket failed");
        vTaskDelete(NULL);
    }
    struct timeval tv = { .tv_sec = 2 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    static const char *keys[] = { "WIFI_AP_DEF", "WIFI_STA_DEF" };
    uint32_t joined[2] = { 0, 0 };
    int64_t last_notify = 0;
    char buf[768];

    for (;;) {
        for (int i = 0; i < 2; i++) {
            uint32_t ip, mask;
            if (netif_addr(keys[i], &ip, &mask) && ip != joined[i]) {
                struct ip_mreq mr = { .imr_interface.s_addr = ip };
                inet_aton(SSDP_GROUP, &mr.imr_multiaddr);
                if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr) == 0) {
                    joined[i] = ip;
                    ESP_LOGI(TAG, "SSDP listening on %s", keys[i]);
                    ssdp_notify(s, ip);
                }
            }
        }
        int64_t now = esp_timer_get_time();
        if (now - last_notify > 25 * 1000000LL) {
            last_notify = now;
            for (int i = 0; i < 2; i++) if (joined[i]) ssdp_notify(s, joined[i]);
        }

        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(s, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        buf[n] = 0;
        if (strncmp(buf, "M-SEARCH", 8) != 0) continue;

        char st[160], ua[96], ip[16];
        header_value(buf, "ST", st, sizeof st);
        header_value(buf, "USER-AGENT", ua, sizeof ua);
        local_ip_for(from.sin_addr.s_addr, ip, sizeof ip);
        ESP_LOGI(TAG, "M-SEARCH from %s ST=%s UA=%s", inet_ntoa(from.sin_addr), st, ua);

        if (strcmp(st, "ssdp:all") == 0) {
            for (size_t i = 0; i < sizeof ssdp_targets / sizeof *ssdp_targets; i++)
                ssdp_reply(s, &from, ip, ssdp_targets[i] ? ssdp_targets[i] : s_udn);
        } else {
            for (size_t i = 0; i < sizeof ssdp_targets / sizeof *ssdp_targets; i++) {
                const char *t = ssdp_targets[i] ? ssdp_targets[i] : s_udn;
                if (strcmp(st, t) == 0) ssdp_reply(s, &from, ip, t);
            }
        }
    }
}

// ---------------------------------------------------------------- PTP-IP

enum { PKT_INIT_CMD_REQ = 1, PKT_INIT_CMD_ACK, PKT_INIT_EVT_REQ, PKT_INIT_EVT_ACK, PKT_INIT_FAIL,
       PKT_OP_REQ, PKT_OP_RESP, PKT_EVENT, PKT_START_DATA, PKT_DATA, PKT_CANCEL, PKT_END_DATA,
       PKT_PROBE_REQ, PKT_PROBE_RESP };

#define OP_GET_DEVICE_INFO   0x1001
#define OP_OPEN_SESSION      0x1002
#define OP_CLOSE_SESSION     0x1003
#define OP_GET_STORAGE_IDS   0x1004
#define OP_GET_STORAGE_INFO  0x1005
#define OP_GET_NUM_OBJECTS   0x1006
#define OP_GET_OBJ_HANDLES   0x1007
#define OP_GET_OBJ_INFO      0x1008
#define OP_GET_OBJECT        0x1009
#define OP_SEND_OBJ_INFO     0x100C
#define OP_SEND_OBJECT       0x100D
#define RC_OK                0x2001
#define EVT_REQ_OBJ_TRANSFER 0x4009
#define FMT_SCRIPT           0x3002

typedef struct {
    int cmd, evt;
    uint32_t tid;
    uint32_t storage;
    uint8_t *buf;               // object scratch, BUF_CAP
    char *xml;                  // outgoing DPS script, XML_CAP
    bool dead;
    bool job_active;
    bool printing;
    const char *job_end;        // jobEndReason code
    const char *err_status;     // errorStatus code
} sess_t;

typedef struct { uint16_t code; uint32_t p[5]; int np; } ptp_resp_t;

typedef struct {
    sess_t *s;
    uint32_t tid;
    uint32_t pkt_left;          // payload bytes of the current Data packet still on the wire
    bool data_done;             // End_Data seen
    bool have_resp;
    ptp_resp_t rsp;
} xfer_t;

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

// One packet: 8-byte header + `a` (<= 72 bytes, sent together with the header) + `b` (bulk).
static int pkt_send(int fd, uint32_t type, const uint8_t *a, size_t alen, const uint8_t *b, size_t blen)
{
    uint8_t h[80];
    put32(h, 8 + alen + blen);
    put32(h + 4, type);
    if (alen) memcpy(h + 8, a, alen);
    if (send_full(fd, h, 8 + alen) != 0) return -1;
    return blen ? send_full(fd, b, blen) : 0;
}

static int skip_bytes(int fd, size_t n)
{
    uint8_t t[64];
    while (n) {
        size_t k = n < sizeof t ? n : sizeof t;
        if (read_full(fd, t, k) != 0) return -1;
        n -= k;
    }
    return 0;
}

// Reads one whole small packet; the payload beyond `cap` is dropped.
static int pkt_recv(int fd, uint32_t *type, uint8_t *pl, size_t cap, size_t *plen)
{
    uint8_t h[8];
    if (read_full(fd, h, 8) != 0) return -1;
    uint32_t len = get32(h);
    *type = get32(h + 4);
    if (len < 8) return -1;
    size_t n = len - 8, k = n < cap ? n : cap;
    if (read_full(fd, pl, k) != 0) return -1;
    if (skip_bytes(fd, n - k) != 0) return -1;
    if (plen) *plen = k;
    return 0;
}

static int tcp_connect(const char *ip, uint16_t port)
{
    for (int attempt = 0; attempt < 6; attempt++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
        inet_aton(ip, &a.sin_addr);
        struct timeval tv = { .tv_sec = 5 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
            tv.tv_sec = 20;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            return fd;
        }
        close(fd);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return -1;
}

// Handles one packet while a transaction is open: data goes to the caller, Op_Response ends it.
static int xfer_next_packet(xfer_t *x)
{
    sess_t *s = x->s;
    uint8_t h[8], b[40];
    if (read_full(s->cmd, h, 8) != 0) return -1;
    uint32_t len = get32(h), type = get32(h + 4);
    if (len < 8) return -1;
    size_t pl = len - 8;
    switch (type) {
    case PKT_START_DATA:
        return skip_bytes(s->cmd, pl);
    case PKT_DATA:
    case PKT_END_DATA:
        if (pl < 4 || read_full(s->cmd, b, 4) != 0) return -1;
        x->pkt_left = pl - 4;
        if (type == PKT_END_DATA) x->data_done = true;
        return 0;
    case PKT_OP_RESP: {
        size_t k = pl < sizeof b ? pl : sizeof b;
        if (k < 6 || read_full(s->cmd, b, k) != 0 || skip_bytes(s->cmd, pl - k) != 0) return -1;
        x->rsp.code = get16(b);
        x->rsp.np = (k - 6) / 4 > 5 ? 5 : (k - 6) / 4;
        for (int i = 0; i < x->rsp.np; i++) x->rsp.p[i] = get32(b + 6 + 4 * i);
        x->have_resp = true;
        return 0;
    }
    case PKT_PROBE_REQ:
        skip_bytes(s->cmd, pl);
        return pkt_send(s->cmd, PKT_PROBE_RESP, NULL, 0, NULL, 0);
    default:
        ESP_LOGW(TAG, "unexpected packet type %u (%u bytes) on the command channel", (unsigned)type, (unsigned)pl);
        return skip_bytes(s->cmd, pl);
    }
}

// Data phase, streamed: bytes read, 0 at the end of the data, <0 on error.
static int xfer_read(xfer_t *x, uint8_t *dst, size_t n)
{
    while (x->pkt_left == 0) {
        if (x->data_done || x->have_resp) return 0;
        if (xfer_next_packet(x) != 0) return -1;
    }
    size_t k = n < x->pkt_left ? n : x->pkt_left;
    if (read_full(x->s->cmd, dst, k) != 0) return -1;
    x->pkt_left -= k;
    return k;
}

static int xfer_finish(xfer_t *x)
{
    while (!x->have_resp) {
        if (x->pkt_left) {
            if (skip_bytes(x->s->cmd, x->pkt_left) != 0) return -1;
            x->pkt_left = 0;
        } else if (xfer_next_packet(x) != 0) {
            return -1;
        }
    }
    return 0;
}

static int ptp_start(sess_t *s, xfer_t *x, uint16_t op, const uint32_t *p, int np, bool data_out)
{
    uint8_t a[34];
    put32(a, data_out ? 2 : 1);
    put16(a + 4, op);
    put32(a + 6, s->tid);
    for (int i = 0; i < np; i++) put32(a + 10 + 4 * i, p[i]);
    memset(x, 0, sizeof *x);
    x->s = s;
    x->tid = s->tid;
    if (s->tid) s->tid++;               // the session starts with transaction 0 (OpenSession)
    if (pkt_send(s->cmd, PKT_OP_REQ, a, 10 + 4 * np, NULL, 0) != 0) { s->dead = true; return -1; }
    return 0;
}

static int ptp_data_out(sess_t *s, uint32_t tid, const uint8_t *d, size_t n)
{
    uint8_t a[12] = { 0 };
    put32(a, tid);
    put32(a + 4, n);
    if (pkt_send(s->cmd, PKT_START_DATA, a, 12, NULL, 0) != 0) return -1;
    return pkt_send(s->cmd, PKT_END_DATA, a, 4, d, n);
}

// A whole transaction with small data phases; extra incoming data is read and dropped.
static int ptp_call(sess_t *s, uint16_t op, const uint32_t *p, int np, const uint8_t *out, size_t out_n,
                    uint8_t *in, size_t in_cap, size_t *in_n, ptp_resp_t *rsp)
{
    xfer_t x;
    if (ptp_start(s, &x, op, p, np, out != NULL) != 0) return -1;
    if (out && ptp_data_out(s, x.tid, out, out_n) != 0) { s->dead = true; return -1; }
    size_t got = 0;
    for (;;) {
        uint8_t sink[64];
        int r;
        if (in && got < in_cap) { r = xfer_read(&x, in + got, in_cap - got); if (r > 0) got += r; }
        else r = xfer_read(&x, sink, sizeof sink);
        if (r < 0) { s->dead = true; return -1; }
        if (r == 0) break;
    }
    if (xfer_finish(&x) != 0) { s->dead = true; return -1; }
    if (in_n) *in_n = got;
    if (rsp) *rsp = x.rsp;
    return 0;
}

// ObjectInfo dataset for a script file: 52 fixed bytes, then filename and three empty strings.
static size_t build_objinfo(uint8_t *d, uint32_t storage, uint32_t size, const char *name)
{
    memset(d, 0, 52);
    put32(d, storage);
    put16(d + 4, FMT_SCRIPT);
    put32(d + 8, size);
    size_t n = 52 + put_ptp_str(d + 52, name);
    d[n++] = 0; d[n++] = 0; d[n++] = 0;
    return n;
}

static int send_script(sess_t *s, const char *name, const char *data, size_t len)
{
    uint8_t oi[160];
    size_t n = build_objinfo(oi, s->storage, len, name);
    uint32_t p[2] = { 0, 0 };
    ptp_resp_t r;
    if (ptp_call(s, OP_SEND_OBJ_INFO, p, 2, oi, n, NULL, 0, NULL, &r) != 0) return -1;
    if (r.code != RC_OK) { ESP_LOGW(TAG, "SendObjectInfo(%s) -> 0x%04x", name, r.code); return -1; }
    if (ptp_call(s, OP_SEND_OBJECT, NULL, 0, (const uint8_t *)data, len, NULL, 0, NULL, &r) != 0) return -1;
    if (r.code != RC_OK) { ESP_LOGW(TAG, "SendObject(%s) -> 0x%04x", name, r.code); return -1; }
    return 0;
}

// ---------------------------------------------------------------- DPS

#define DPS_HEAD "<?xml version=\"1.0\"?>\n<dps xmlns=\"http://www.cipa.jp/dps/schema/\">\n"

static int dps_reply(sess_t *s, const char *result, const char *inner)
{
    int n = snprintf(s->xml, XML_CAP, DPS_HEAD "<output>\n<result>%s</result>\n%s</output>\n</dps>\n", result, inner);
    ESP_LOGI(TAG, "-> HRSPONSE.DPS result %s", result);
    return send_script(s, "HRSPONSE.DPS", s->xml, n);
}

static int dps_notify(sess_t *s, const char *inner)
{
    int n = snprintf(s->xml, XML_CAP, DPS_HEAD "<input>\n%s</input>\n</dps>\n", inner);
    return send_script(s, "HREQUEST.DPS", s->xml, n);
}

static int notify_device(sess_t *s)
{
    char in[560];
    snprintf(in, sizeof in,
             "<notifyDeviceStatus>\n<dpsPrintServiceStatus>%s</dpsPrintServiceStatus>\n"
             "<jobEndReason>%s</jobEndReason>\n<errorStatus>%s</errorStatus>\n<errorReason>73000000</errorReason>\n"
             "<disconnectEnable>%s</disconnectEnable>\n<capabilityChanged>75000000</capabilityChanged>\n"
             "<newJobOK>%s</newJobOK>\n</notifyDeviceStatus>\n",
             s->printing ? "70000000" : "70010000", s->job_end, s->err_status,
             s->printing ? "74000000" : "74010000", s->printing ? "76000000" : "76010000");
    ESP_LOGI(TAG, "-> notifyDeviceStatus (printing=%d end=%s)", s->printing, s->job_end);
    return dps_notify(s, in);
}

static int notify_job(sess_t *s, int done, int total)
{
    char in[200];
    snprintf(in, sizeof in, "<notifyJobStatus>\n<progress>%03d/%03d</progress>\n<imagesPrinted>%03d</imagesPrinted>\n"
             "</notifyJobStatus>\n", done, total, done);
    ESP_LOGI(TAG, "-> notifyJobStatus %d/%d", done, total);
    return dps_notify(s, in);
}

// Text of <tag>...</tag> (a tag without attributes), trimmed; "" when absent.
static void xml_text(const char *xml, const char *tag, char *out, size_t cap)
{
    char open[40];
    snprintf(open, sizeof open, "<%s>", tag);
    out[0] = 0;
    const char *p = strstr(xml, open);
    if (!p) return;
    p += strlen(open);
    while (isspace((unsigned char)*p)) p++;
    size_t i = 0;
    while (p[i] && p[i] != '<' && i + 1 < cap) { out[i] = p[i]; i++; }
    while (i && isspace((unsigned char)out[i - 1])) i--;
    out[i] = 0;
}

static const struct { const char *tag; const char *values; bool by_paper; } CAPS[] = {
    { "qualities",      "50000000", false },
    { "paperSizes",     "51000000", false },
    { "paperTypes",     "52000000", true },
    { "fileTypes",      "53000000 53010000 53030000", false },
    { "datePrints",     "54000000 54010000", false },
    { "fileNamePrints", "55000000 55010000", false },
    { "imageOptimizes", "56000000 56010000", false },
    { "layouts",        "57000000", true },
    { "fixedSizes",     "58000000", false },
    { "croppings",      "59000000", false },
};

static int dps_get_capability(sess_t *s, const char *req)
{
    char *in = malloc(1024);
    if (!in) return -1;
    int n = snprintf(in, 1024, "<getCapability>\n<capability>\n");
    for (size_t i = 0; i < sizeof CAPS / sizeof *CAPS; i++) {
        char open[24];
        snprintf(open, sizeof open, "<%s", CAPS[i].tag);
        const char *p = strstr(req, open);
        if (!p) continue;
        char attr[48] = "";
        if (CAPS[i].by_paper) {
            const char *a = strstr(p, "paperSize=\"");
            const char *end = strchr(p, '>');
            if (a && end && a < end && strlen(a) >= 19) snprintf(attr, sizeof attr, " paperSize=\"%.8s\"", a + 11);
        }
        n += snprintf(in + n, 1024 - n, "<%s%s>%s</%s>\n", CAPS[i].tag, attr, CAPS[i].values, CAPS[i].tag);
    }
    snprintf(in + n, 1024 - n, "</capability>\n</getCapability>\n");
    int r = dps_reply(s, "10000000", in);
    free(in);
    return r;
}

static int dps_device_status(sess_t *s)
{
    char in[560];
    snprintf(in, sizeof in,
             "<getDeviceStatus>\n<dpsPrintServiceStatus>%s</dpsPrintServiceStatus>\n"
             "<jobEndReason>%s</jobEndReason>\n<errorStatus>%s</errorStatus>\n<errorReason>73000000</errorReason>\n"
             "<disconnectEnable>%s</disconnectEnable>\n<capabilityChanged>75000000</capabilityChanged>\n"
             "<newJobOK>%s</newJobOK>\n</getDeviceStatus>\n",
             s->printing ? "70000000" : "70010000", s->job_end, s->err_status,
             s->printing ? "74000000" : "74010000", s->printing ? "76000000" : "76010000");
    return dps_reply(s, "10000000", in);
}

static void service_events(sess_t *s, int wait_ms);

static int xfer_read_cb(void *ctx, uint8_t *buf, size_t len)
{
    return xfer_read(ctx, buf, len);
}

// The picture the camera offered: stream it out of the camera and into the dither.
static esp_err_t fetch_picture(sess_t *s, uint32_t handle, uint8_t **bits, int *rows)
{
    ptp_resp_t r;
    size_t n;
    uint32_t p[1] = { handle };
    if (ptp_call(s, OP_GET_OBJ_INFO, p, 1, NULL, 0, s->buf, 512, &n, &r) != 0) return ESP_FAIL;
    if (r.code != RC_OK || n < 54) { ESP_LOGE(TAG, "GetObjectInfo(0x%x) -> 0x%04x", (unsigned)handle, r.code); return ESP_FAIL; }
    char name[40];
    get_ptp_str(s->buf, n, 52, name, sizeof name);
    ESP_LOGI(TAG, "picture %s: format 0x%04x, %u bytes", name, get16(s->buf + 4), (unsigned)get32(s->buf + 8));

    xfer_t x;
    if (ptp_start(s, &x, OP_GET_OBJECT, p, 1, false) != 0) return ESP_FAIL;
    esp_err_t err = jpegprint_decode(xfer_read_cb, &x, bits, rows);
    // Whatever the decoder left unread still has to leave the wire, or the next transaction is garbage.
    uint8_t sink[256];
    int r2;
    while ((r2 = xfer_read(&x, sink, sizeof sink)) > 0) { }
    if (r2 < 0 || xfer_finish(&x) != 0) { s->dead = true; if (err == ESP_OK) { free(*bits); err = ESP_FAIL; } }
    return err;
}

static void run_job(sess_t *s, uint32_t handle, int copies)
{
    s->job_active = true;
    s->printing = true;
    s->job_end = "71000000";
    s->err_status = "72000000";
    notify_device(s);

    uint8_t *bits = NULL;
    int rows = 0;
    esp_err_t err = fetch_picture(s, handle, &bits, &rows);
    if (err == ESP_OK && !s->dead) {
        err = printjob_submit(bits, rows, copies);      // takes the buffer
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "printing %d rows x %d", rows, copies);
            int64_t last = 0;
            int last_done = -1;
            for (int64_t t0 = esp_timer_get_time(); !s->dead && esp_timer_get_time() - t0 < 600 * 1000000LL;) {
                printjob_status_t st;
                printjob_status(&st);
                if (!st.busy) break;
                int done = st.copy - 1;
                if (done != last_done && esp_timer_get_time() - last > 2 * 1000000LL) {
                    notify_job(s, done, copies);
                    last_done = done;
                    last = esp_timer_get_time();
                }
                service_events(s, 300);
            }
        }
    }
    s->printing = false;
    s->job_end = err == ESP_OK ? "71010000" : "71020000";
    s->err_status = err == ESP_OK ? "72000000" : "72020000";
    if (!s->dead) {
        notify_job(s, err == ESP_OK ? copies : 0, copies);
        notify_device(s);
    }
    s->job_active = false;
    ESP_LOGI(TAG, "job finished: %s", esp_err_to_name(err));
}

static void on_dps_request(sess_t *s, const char *xml)
{
    ESP_LOGI(TAG, "<- DREQUEST.DPS\n%s", xml);
    if (strstr(xml, "<configurePrintService")) {
        dps_reply(s, "10000000",
                  "<configurePrintService>\n<printServiceAvailable>30010000</printServiceAvailable>\n"
                  "<dpsVersions>01.00</dpsVersions>\n<vendorName>PhotoPrinter</vendorName>\n"
                  "<vendorSpecificVersion>01.00</vendorSpecificVersion>\n<productName>QR204</productName>\n"
                  "<serialNo>0001</serialNo>\n</configurePrintService>\n");
    } else if (strstr(xml, "<getCapability")) {
        dps_get_capability(s, xml);
        // croppings is the last capability the camera asks for; it then waits for the printer to
        // announce itself ready. Without this notification "print" fails with a communication error.
        if (strstr(xml, "<croppings")) notify_device(s);
    } else if (strstr(xml, "<getDeviceStatus") || strstr(xml, "<getJobStatus")) {
        dps_device_status(s);
    } else if (strstr(xml, "<startJob")) {
        char id[16], cp[8];
        xml_text(xml, "fileID", id, sizeof id);
        xml_text(xml, "copies", cp, sizeof cp);
        uint32_t handle = strtoul(id, NULL, 16);
        int copies = atoi(cp);
        if (copies < 1) copies = 1;
        if (copies > 20) copies = 20;
        if (s->job_active || handle == 0) {
            dps_reply(s, "10040000", "<startJob/>\n");
            return;
        }
        dps_reply(s, "10000000", "<startJob/>\n");
        run_job(s, handle, copies);
    } else if (strstr(xml, "<abortJob")) {
        dps_reply(s, "10000000", "<abortJob/>\n");
    } else if (strstr(xml, "<continueJob")) {
        dps_reply(s, "10000000", "<continueJob/>\n");
    } else {
        ESP_LOGW(TAG, "unknown DPS request");
        dps_reply(s, "10020000", "");
    }
}

// The camera announced an object (RequestObjectTransfer): read it and act on it.
static void on_transfer_request(sess_t *s, uint32_t handle)
{
    ptp_resp_t r;
    size_t n;
    uint32_t p[1] = { handle };
    if (ptp_call(s, OP_GET_OBJ_INFO, p, 1, NULL, 0, s->buf, 512, &n, &r) != 0 || r.code != RC_OK || n < 54) {
        ESP_LOGW(TAG, "GetObjectInfo(0x%x) failed", (unsigned)handle);
        return;
    }
    char name[40];
    get_ptp_str(s->buf, n, 52, name, sizeof name);
    uint32_t size = get32(s->buf + 8);
    if (size >= BUF_CAP) { ESP_LOGW(TAG, "%s is %u bytes, ignored", name, (unsigned)size); return; }
    if (ptp_call(s, OP_GET_OBJECT, p, 1, NULL, 0, s->buf, BUF_CAP - 1, &n, &r) != 0) {
        ESP_LOGE(TAG, "GetObject(%s) lost the connection", name);
        return;
    }
    if (r.code != RC_OK) { ESP_LOGW(TAG, "GetObject(%s) -> 0x%04x", name, r.code); return; }
    s->buf[n] = 0;
    if (strcmp(name, "DREQUEST.DPS") == 0) {
        on_dps_request(s, (const char *)s->buf);
    } else if (strcmp(name, "DRSPONSE.DPS") == 0) {
        ESP_LOGI(TAG, "<- DRSPONSE.DPS\n%s", (const char *)s->buf);
    } else {
        ESP_LOGW(TAG, "<- unexpected object %s (%u bytes)", name, (unsigned)n);
    }
}

static void service_events(sess_t *s, int wait_ms)
{
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(s->evt, &rf);
    FD_SET(s->cmd, &rf);
    struct timeval tv = { .tv_sec = wait_ms / 1000, .tv_usec = (wait_ms % 1000) * 1000 };
    int mx = (s->evt > s->cmd ? s->evt : s->cmd) + 1;
    if (select(mx, &rf, NULL, NULL, &tv) <= 0) return;

    uint8_t pl[40];
    size_t plen;
    uint32_t type;
    if (FD_ISSET(s->cmd, &rf)) {                    // nothing is expected here unless the camera hangs up
        char b;
        if (recv(s->cmd, &b, 1, MSG_PEEK) <= 0) { ESP_LOGI(TAG, "camera closed the command channel"); s->dead = true; return; }
        if (pkt_recv(s->cmd, &type, pl, sizeof pl, &plen) != 0) { s->dead = true; return; }
        if (type == PKT_PROBE_REQ) pkt_send(s->cmd, PKT_PROBE_RESP, NULL, 0, NULL, 0);
        else ESP_LOGW(TAG, "unsolicited packet type %u on the command channel", (unsigned)type);
    }
    if (FD_ISSET(s->evt, &rf)) {
        if (pkt_recv(s->evt, &type, pl, sizeof pl, &plen) != 0) { ESP_LOGI(TAG, "camera closed the event channel"); s->dead = true; return; }
        if (type == PKT_PROBE_REQ) {
            pkt_send(s->evt, PKT_PROBE_RESP, NULL, 0, NULL, 0);
        } else if (type == PKT_EVENT && plen >= 6) {
            uint16_t code = get16(pl);
            uint32_t p1 = plen >= 10 ? get32(pl + 6) : 0;
            ESP_LOGI(TAG, "event 0x%04x(0x%x)", code, (unsigned)p1);
            if (code == EVT_REQ_OBJ_TRANSFER) on_transfer_request(s, p1);
        } else {
            ESP_LOGW(TAG, "event channel: packet type %u", (unsigned)type);
        }
    }
}

// Connects to the camera and does the PTP-IP handshake + DPS discovery. 0 = ready.
static int session_open(sess_t *s, const char *ip)
{
    uint8_t pl[96];
    size_t plen;
    uint32_t type;

    // The camera sends "a.b.c.d:15740"; the port part is optional.
    char host[24];
    uint16_t port = PTP_PORT;
    snprintf(host, sizeof host, "%s", ip);
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = 0;
        if (atoi(colon + 1) > 0) port = atoi(colon + 1);
    }
    ip = host;

    s->cmd = tcp_connect(ip, port);
    if (s->cmd < 0) { ESP_LOGE(TAG, "cannot connect to %s:%d", ip, port); return -1; }

    uint8_t req[16 + 28 + 4];
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    memcpy(req, "PhotoPrinter-ID!", 16);
    memcpy(req + 10, mac, 6);
    for (int i = 0; i < 13; i++) { req[16 + 2 * i] = "PhotoPrinter"[i]; req[17 + 2 * i] = 0; }   // includes the NUL
    put32(req + 16 + 26, 0x00010000);
    if (pkt_send(s->cmd, PKT_INIT_CMD_REQ, req, 16 + 26 + 4, NULL, 0) != 0 ||
        pkt_recv(s->cmd, &type, pl, sizeof pl, &plen) != 0) {
        ESP_LOGE(TAG, "no answer to Init_Command_Request");
        return -1;
    }
    if (type != PKT_INIT_CMD_ACK || plen < 4) {
        ESP_LOGE(TAG, "Init_Command_Request refused (packet type %u, reason %u)", (unsigned)type,
                 plen >= 4 ? (unsigned)get32(pl) : 0);
        return -1;
    }
    uint32_t conn = get32(pl);
    ESP_LOGI(TAG, "command channel up, connection %u", (unsigned)conn);

    s->evt = tcp_connect(ip, port);
    if (s->evt < 0) { ESP_LOGE(TAG, "cannot open the event channel"); return -1; }
    uint8_t er[4];
    put32(er, conn);
    if (pkt_send(s->evt, PKT_INIT_EVT_REQ, er, 4, NULL, 0) != 0 ||
        pkt_recv(s->evt, &type, pl, sizeof pl, &plen) != 0 || type != PKT_INIT_EVT_ACK) {
        ESP_LOGE(TAG, "event channel refused (type %u)", (unsigned)type);
        return -1;
    }
    ESP_LOGI(TAG, "PTP-IP connection established");

    ptp_resp_t r;
    size_t n;
    s->tid = 0;
    if (ptp_call(s, OP_GET_DEVICE_INFO, NULL, 0, NULL, 0, s->buf, BUF_CAP, &n, &r) != 0) return -1;
    ESP_LOGI(TAG, "GetDeviceInfo -> 0x%04x, %u bytes", r.code, (unsigned)n);
    uint32_t sess_id[1] = { 1 };
    if (ptp_call(s, OP_OPEN_SESSION, sess_id, 1, NULL, 0, NULL, 0, NULL, &r) != 0) return -1;
    ESP_LOGI(TAG, "OpenSession -> 0x%04x", r.code);
    s->tid = 1;

    s->storage = 0x00010001;
    if (ptp_call(s, OP_GET_STORAGE_IDS, NULL, 0, NULL, 0, s->buf, 64, &n, &r) != 0) return -1;
    if (n >= 8 && get32(s->buf) >= 1) s->storage = get32(s->buf + 4);
    ESP_LOGI(TAG, "GetStorageIDs -> 0x%04x, storage 0x%08x", r.code, (unsigned)s->storage);
    uint32_t sp[1] = { s->storage };
    if (ptp_call(s, OP_GET_STORAGE_INFO, sp, 1, NULL, 0, NULL, 0, NULL, &r) != 0) return -1;

    uint32_t q[3] = { 0xFFFFFFFF, FMT_SCRIPT, 0 };
    if (ptp_call(s, OP_GET_NUM_OBJECTS, q, 3, NULL, 0, NULL, 0, NULL, &r) != 0) return -1;
    ESP_LOGI(TAG, "GetNumObjects(scripts) -> 0x%04x, %u", r.code, r.np ? (unsigned)r.p[0] : 0);
    if (ptp_call(s, OP_GET_OBJ_HANDLES, q, 3, NULL, 0, s->buf, 256, &n, &r) != 0) return -1;
    uint32_t count = n >= 4 ? get32(s->buf) : 0;
    if (count > 30) count = 30;
    uint32_t handles[30];
    for (uint32_t i = 0; i < count && 4 + 4 * i + 4 <= n; i++) handles[i] = get32(s->buf + 4 + 4 * i);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t h[1] = { handles[i] };
        if (ptp_call(s, OP_GET_OBJ_INFO, h, 1, NULL, 0, s->buf, 512, &n, &r) != 0) return -1;
        char name[40];
        get_ptp_str(s->buf, n, 52, name, sizeof name);
        ESP_LOGI(TAG, "object 0x%x: %s (format 0x%04x)", (unsigned)h[0], name, n >= 6 ? get16(s->buf + 4) : 0);
        if (strcmp(name, "DDISCVRY.DPS") == 0) {
            // Answer with the same ObjectInfo, filename HDISCVRY.DPS: this completes the discovery.
            uint8_t oi[160];
            memcpy(oi, s->buf, 52);
            size_t on = 52 + put_ptp_str(oi + 52, "HDISCVRY.DPS");
            oi[on++] = 0; oi[on++] = 0; oi[on++] = 0;
            uint32_t sp2[2] = { 0, 0 };
            if (ptp_call(s, OP_SEND_OBJ_INFO, sp2, 2, oi, on, NULL, 0, NULL, &r) != 0) return -1;
            ESP_LOGI(TAG, "DPS discovery: SendObjectInfo(HDISCVRY.DPS) -> 0x%04x", r.code);
            return r.code == RC_OK ? 0 : -1;
        }
    }
    ESP_LOGE(TAG, "no DDISCVRY.DPS among %u script objects", (unsigned)count);
    return -1;
}

static void session_task(void *arg)
{
    char *ip = arg;
    sess_t *s = calloc(1, sizeof *s);
    if (s) {
        s->cmd = s->evt = -1;
        s->buf = malloc(BUF_CAP);
        s->xml = malloc(XML_CAP);
        s->job_end = "71000000";
        s->err_status = "72000000";
    }
    ESP_LOGI(TAG, "session with camera %s (heap %u)", ip, (unsigned)esp_get_free_heap_size());
    if (s && s->buf && s->xml && session_open(s, ip) == 0) {
        int64_t idle = esp_timer_get_time();
        while (!s->dead && esp_timer_get_time() - idle < 300 * 1000000LL) {
            service_events(s, 1000);
            if (s->job_active) idle = esp_timer_get_time();
        }
        if (!s->dead) {
            ptp_resp_t r;
            ptp_call(s, OP_CLOSE_SESSION, NULL, 0, NULL, 0, NULL, 0, NULL, &r);
        }
    }
    if (s) {
        if (s->cmd >= 0) close(s->cmd);
        if (s->evt >= 0) close(s->evt);
        free(s->buf);
        free(s->xml);
        free(s);
    }
    free(ip);
    ESP_LOGI(TAG, "session closed (heap %u)", (unsigned)esp_get_free_heap_size());
    s_active = false;
    vTaskDelete(NULL);
}

static bool session_start(const char *ip)
{
    if (s_active) return false;
    char *copy = strdup(ip);
    if (!copy) return false;
    s_active = true;
    if (xTaskCreate(session_task, "ptpip", 10240, copy, 5, NULL) != pdPASS) {
        free(copy);
        s_active = false;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- UPnP over HTTP

static void peer_ip(httpd_req_t *req, char *out, size_t n)
{
    struct sockaddr_storage a;                  // lwIP sockets are dual-stack: IPv4 peers come back as ::ffff:a.b.c.d
    socklen_t l = sizeof a;
    out[0] = 0;
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&a, &l) != 0) return;
    struct in_addr v4 = { 0 };
    if (a.ss_family == AF_INET6) memcpy(&v4, (uint8_t *)&((struct sockaddr_in6 *)&a)->sin6_addr + 12, 4);
    else v4 = ((struct sockaddr_in *)&a)->sin_addr;
    snprintf(out, n, "%s", inet_ntoa(v4));
}

static void log_request(httpd_req_t *req)
{
    char ip[16], ua[96] = "", sa[160] = "";
    peer_ip(req, ip, sizeof ip);
    httpd_req_get_hdr_value_str(req, "USER-AGENT", ua, sizeof ua);
    httpd_req_get_hdr_value_str(req, "SOAPACTION", sa, sizeof sa);
    ESP_LOGI(TAG, "HTTP %s %s from %s UA=\"%s\" SOAPACTION=%s", http_method_str(req->method), req->uri, ip, ua, sa);
}

// The camera's HTTP client resets the connection on the stock esp_http_server headers, so the whole
// response is written by hand in the style of the UPnP DA 1.0 examples (upper-case names, EXT, SERVER).
static esp_err_t send_xml(httpd_req_t *req, const char *body)
{
    char head[256];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 200 OK\r\nCONTENT-TYPE: text/xml; charset=\"utf-8\"\r\nCONTENT-LENGTH: %u\r\n"
                     "EXT:\r\nSERVER: ESP32/1.0 UPnP/1.0 PhotoPrinter/1.0\r\nCONNECTION: close\r\n\r\n",
                     (unsigned)strlen(body));
    if (httpd_send(req, head, n) < 0 || httpd_send(req, body, strlen(body)) < 0) return ESP_FAIL;
    return ESP_OK;
}

static esp_err_t get_desc(httpd_req_t *req)
{
    log_request(req);
    char *xml = malloc(1500);
    if (!xml) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    snprintf(xml, 1500,
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\r\n"
             "<specVersion><major>1</major><minor>0</minor></specVersion>\r\n<device>\r\n"
             "<deviceType>" DEV_TYPE "</deviceType>\r\n<friendlyName>PhotoPrinter</friendlyName>\r\n"
             "<manufacturer>DIY</manufacturer>\r\n<manufacturerURL>http://" WIFI_AP_IP "/</manufacturerURL>\r\n"
             "<modelDescription>PhotoPrinter</modelDescription>\r\n<modelName>QR204</modelName>\r\n"
             "<modelNumber>1</modelNumber>\r\n<modelURL>http://" WIFI_AP_IP "/</modelURL>\r\n"
             "<serialNumber>0001</serialNumber>\r\n<UDN>%s</UDN>\r\n<presentationURL>/</presentationURL>\r\n"
             "<serviceList>\r\n<service>\r\n<serviceType>" SVC_TYPE "</serviceType>\r\n"
             "<serviceId>" SVC_ID "</serviceId>\r\n<SCPDURL>/dpsconn.xml</SCPDURL>\r\n"
             "<controlURL>" CONTROL_URL "</controlURL>\r\n<eventSubURL>/event</eventSubURL>\r\n"
             "</service>\r\n</serviceList>\r\n</device>\r\n</root>\r\n", s_udn);
    esp_err_t e = send_xml(req, xml);
    ESP_LOGI(TAG, "desc.xml: %u bytes sent -> %s", (unsigned)strlen(xml), esp_err_to_name(e));
    free(xml);
    return e;
}

static esp_err_t get_scpd(httpd_req_t *req)
{
    log_request(req);
    return send_xml(req,
        "<?xml version=\"1.0\"?>\r\n<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\r\n"
        "<specVersion><major>1</major><minor>0</minor></specVersion>\r\n<actionList>\r\n<action>\r\n"
        "<name>ConnectionRequest</name>\r\n<argumentList>\r\n"
        "<argument><name>PTPResponderIPAddress</name><direction>in</direction>"
        "<relatedStateVariable>A_ARG_TYPE_IPAddress</relatedStateVariable></argument>\r\n"
        "<argument><name>RequestResult</name><direction>out</direction>"
        "<relatedStateVariable>A_ARG_TYPE_Result</relatedStateVariable></argument>\r\n"
        "</argumentList>\r\n</action>\r\n</actionList>\r\n<serviceStateTable>\r\n"
        "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_IPAddress</name><dataType>string</dataType></stateVariable>\r\n"
        "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Result</name><dataType>string</dataType></stateVariable>\r\n"
        "</serviceStateTable>\r\n</scpd>\r\n");
}

static esp_err_t post_soap(httpd_req_t *req)
{
    log_request(req);
    size_t len = req->content_len;
    if (len == 0 || len > 3000) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
    char *body = malloc(len + 1);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    for (size_t got = 0; got < len;) {
        int n = httpd_req_recv(req, body + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) { free(body); return ESP_FAIL; }
        got += n;
    }
    body[len] = 0;
    ESP_LOGI(TAG, "SOAP body:\n%s", body);

    const char *result = "REJECT";
    char cam[32] = "";
    const char *tag = strstr(body, "PTPResponderIPAddress>");
    if (tag) {
        tag += strlen("PTPResponderIPAddress>");
        size_t i = 0;
        while (tag[i] && tag[i] != '<' && i + 1 < sizeof cam) { cam[i] = tag[i]; i++; }
        cam[i] = 0;
    }
    if (!cam[0]) peer_ip(req, cam, sizeof cam);
    free(body);
    if (session_start(cam)) result = "ACCEPT";
    ESP_LOGI(TAG, "ConnectionRequest(camera %s) -> %s", cam, result);

    char resp[480];
    snprintf(resp, sizeof resp,
             "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n<s:Body>\r\n"
             "<u:ConnectionRequestResponse xmlns:u=\"" SVC_TYPE "\">\r\n<RequestResult>%s</RequestResult>\r\n"
             "</u:ConnectionRequestResponse>\r\n</s:Body>\r\n</s:Envelope>\r\n", result);
    return send_xml(req, resp);
}

static esp_err_t http_unknown(httpd_req_t *req, httpd_err_code_t err)
{
    log_request(req);
    return httpd_resp_send_err(req, err, NULL);
}

// Dev aid: POST a JPEG to /jpeg?copies=N to run the same decode+print path as the camera does.
static int body_read_cb(void *ctx, uint8_t *buf, size_t len)
{
    httpd_req_t *req = ctx;
    for (;;) {
        int n = httpd_req_recv(req, (char *)buf, len);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        return n;
    }
}

static esp_err_t post_jpeg(httpd_req_t *req)
{
    log_request(req);
    int copies = 1;
    char q[32], v[8];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && httpd_query_key_value(q, "copies", v, sizeof v) == ESP_OK)
        copies = atoi(v);
    if (copies < 1) copies = 1;
    if (copies > 20) copies = 20;

    uint8_t *bits = NULL;
    int rows = 0;
    esp_err_t err = jpegprint_decode(body_read_cb, req, &bits, &rows);
    if (err == ESP_OK) err = printjob_submit(bits, rows, copies);
    char out[64];
    snprintf(out, sizeof out, "%s rows=%d\n", esp_err_to_name(err), rows);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, out);
}

esp_err_t pictbridge_http_register(httpd_handle_t srv)
{
    const httpd_uri_t uris[] = {
        { .uri = "/jpeg",        .method = HTTP_POST, .handler = post_jpeg },
        { .uri = "/desc.xml",    .method = HTTP_GET,  .handler = get_desc },
        { .uri = "/dpsconn.xml", .method = HTTP_GET,  .handler = get_scpd },
        { .uri = CONTROL_URL,    .method = HTTP_POST, .handler = post_soap },
    };
    for (size_t i = 0; i < sizeof uris / sizeof *uris; i++) {
        esp_err_t e = httpd_register_uri_handler(srv, &uris[i]);
        if (e != ESP_OK) return e;
    }
    httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, http_unknown);
    httpd_register_err_handler(srv, HTTPD_405_METHOD_NOT_ALLOWED, http_unknown);
    return ESP_OK;
}

esp_err_t pictbridge_start(void)
{
    static const char *const httpd_tags[] = { "httpd", "httpd_parse", "httpd_txrx", "httpd_uri", "httpd_sess" };
    for (size_t i = 0; i < sizeof httpd_tags / sizeof *httpd_tags; i++) esp_log_level_set(httpd_tags[i], ESP_LOG_DEBUG);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_udn, sizeof s_udn, "uuid:50686f74-6f50-5269-6e74-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return xTaskCreate(ssdp_task, "ssdp", 4096, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
