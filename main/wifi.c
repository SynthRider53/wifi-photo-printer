#include "wifi.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "esp_check.h"
#include "dhcpserver/dhcpserver.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wifi";

static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static char s_ap_ssid[24];
static char s_sta_ip[16];
static bool s_sta_wanted;
static bool s_sta_connected;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START && s_sta_wanted) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        s_sta_ip[0] = 0;
        if (s_sta_wanted) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        snprintf(s_sta_ip, sizeof s_sta_ip, IPSTR, IP2STR(&ev->ip_info.ip));
        s_sta_connected = true;
        ESP_LOGI(TAG, "joined home network, http://%s/", s_sta_ip);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "client joined the access point");
    }
}

static bool load_station(wifi_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return false;
    size_t ssid_len = sizeof cfg->sta.ssid, pass_len = sizeof cfg->sta.password;
    bool ok = nvs_get_str(h, "ssid", (char *)cfg->sta.ssid, &ssid_len) == ESP_OK && cfg->sta.ssid[0];
    if (ok && nvs_get_str(h, "pass", (char *)cfg->sta.password, &pass_len) != ESP_OK) cfg->sta.password[0] = 0;
    nvs_close(h);
    return ok;
}

esp_err_t wifi_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_t *ap_netif = s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    // instax clients look for the printer at a fixed address, so the access
    // point takes it and hands out leases well clear of it.
    esp_netif_ip_info_t ap_ip = { 0 };
    esp_netif_str_to_ip4(WIFI_AP_IP, &ap_ip.ip);
    esp_netif_str_to_ip4(WIFI_AP_IP, &ap_ip.gw);
    esp_netif_str_to_ip4("255.255.255.0", &ap_ip.netmask);
    esp_log_level_set("dhcps", ESP_LOG_VERBOSE);    // while the handing out of addresses is in doubt
    dhcps_lease_t lease = { .enable = true };       // the pool uses lwIP's own address type
    lease.start_ip.addr = ipaddr_addr(WIFI_AP_DHCP_FIRST);
    lease.end_ip.addr = ipaddr_addr(WIFI_AP_DHCP_LAST);
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_stop(ap_netif), TAG, "dhcps stop");
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(ap_netif, &ap_ip), TAG, "ap ip");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS,
                                               &lease, sizeof lease), TAG, "dhcps range");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(ap_netif), TAG, "dhcps start");

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    uint32_t serial = ((uint32_t)mac[2] << 24) | (mac[3] << 16) | (mac[4] << 8) | mac[5];
    snprintf(s_ap_ssid, sizeof s_ap_ssid, WIFI_AP_SSID_PREFIX "%08u", (unsigned)(serial % 100000000u));

    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL), TAG, "ev");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL), TAG, "ev");

    wifi_config_t ap = {
        .ap = {
            .ssid_len = strlen(s_ap_ssid),
            .channel = WIFI_AP_CHANNEL,
            .authmode = WIFI_AUTH_OPEN,     // an instax printer's network has no key
            .max_connection = 4,
        },
    };
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof ap.ap.ssid);

    wifi_config_t sta = { 0 };
    s_sta_wanted = load_station(&sta);

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(s_sta_wanted ? WIFI_MODE_APSTA : WIFI_MODE_AP), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap cfg");
    if (s_sta_wanted) ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta), TAG, "sta cfg");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");

    ESP_LOGI(TAG, "access point \"%s\" (open), http://" WIFI_AP_IP "/", s_ap_ssid);
    if (s_sta_wanted) ESP_LOGI(TAG, "joining \"%s\"", sta.sta.ssid);
    return ESP_OK;
}

esp_err_t wifi_set_station(const char *ssid, const char *pass)
{
    bool forget = !ssid || !ssid[0];
    wifi_config_t probe;
    if (!forget && strlen(ssid) >= sizeof probe.sta.ssid) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open("wifi", NVS_READWRITE, &h), TAG, "nvs open");
    if (forget) {
        nvs_erase_all(h);
    } else {
        nvs_set_str(h, "ssid", ssid);
        nvs_set_str(h, "pass", pass ? pass : "");
    }
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    ESP_RETURN_ON_ERROR(err, TAG, "nvs commit");

    s_sta_wanted = !forget;
    s_sta_connected = false;
    s_sta_ip[0] = 0;
    esp_wifi_disconnect();
    if (forget) return esp_wifi_set_mode(WIFI_MODE_AP);

    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, ssid, sizeof sta.sta.ssid);
    strlcpy((char *)sta.sta.password, pass ? pass : "", sizeof sta.sta.password);
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta), TAG, "sta cfg");
    return esp_wifi_connect();
}

void wifi_describe(char *out, size_t len)
{
    char sta[80] = "home network: not set";
    wifi_config_t cfg;
    if (s_sta_wanted && esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
        if (s_sta_connected) snprintf(sta, sizeof sta, "home network \"%s\": http://%s/", cfg.sta.ssid, s_sta_ip);
        else snprintf(sta, sizeof sta, "home network \"%s\": connecting...", cfg.sta.ssid);
    }
    snprintf(out, len, "AP \"%s\" (open): http://" WIFI_AP_IP "/ ; %s", s_ap_ssid, sta);
}

const char *wifi_ap_ssid(void)
{
    return s_ap_ssid;
}

void wifi_ap_state(char *out, size_t len)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    wifi_config_t cfg = { 0 };
    wifi_sta_list_t clients = { 0 };
    uint8_t primary = 0;
    wifi_second_chan_t second = 0;
    int8_t power = 0;

    esp_err_t err = esp_wifi_get_mode(&mode);
    esp_wifi_get_config(WIFI_IF_AP, &cfg);
    esp_wifi_get_channel(&primary, &second);
    esp_wifi_ap_get_sta_list(&clients);
    esp_wifi_get_max_tx_power(&power);
    esp_netif_dhcp_status_t dhcp = 0;
    esp_err_t derr = esp_netif_dhcps_get_status(s_ap_netif, &dhcp);
    esp_netif_ip_info_t info = { 0 };
    esp_netif_get_ip_info(s_ap_netif, &info);

    snprintf(out, len, "mode %d (%s), ssid \"%s\" len %u, channel %u (radio %u), hidden %u, auth %d, "
             "clients %d, tx power %.1f dBm%s; netif " IPSTR "/" IPSTR ", dhcp server %s (%s)",
             mode, mode == WIFI_MODE_AP ? "AP" : mode == WIFI_MODE_APSTA ? "AP+STA" : "not an AP",
             (char *)cfg.ap.ssid, cfg.ap.ssid_len, cfg.ap.channel, primary, cfg.ap.ssid_hidden,
             cfg.ap.authmode, clients.num, power / 4.0, err == ESP_OK ? "" : " (driver error)",
             IP2STR(&info.ip), IP2STR(&info.netmask),
             dhcp == ESP_NETIF_DHCP_STARTED ? "running" : dhcp == ESP_NETIF_DHCP_STOPPED ? "STOPPED" : "init",
             esp_err_to_name(derr));
}

esp_err_t wifi_set_ap_channel(int channel)
{
    ESP_RETURN_ON_FALSE(channel >= 1 && channel <= 13, ESP_ERR_INVALID_ARG, TAG, "channel 1..13");
    wifi_config_t cfg;
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_AP, &cfg), TAG, "get ap cfg");
    cfg.ap.channel = channel;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &cfg), TAG, "set ap cfg");
    ESP_LOGI(TAG, "access point moved to channel %d", channel);
    return ESP_OK;
}
