#pragma once

#include <stddef.h>
#include "esp_err.h"

// Always an access point: open, named INSTAX-<8 digits> so that Fujifilm
// cameras accept it, serving the page at http://192.168.0.251/. If a home
// network is stored in NVS the board also joins it (AP+STA), so a phone or
// camera already on that network can reach the page at the address it got.
esp_err_t wifi_start(void);

// Name of the access point, built from the MAC address at startup.
const char *wifi_ap_ssid(void);

// What the Wi-Fi driver reports right now: mode, channel, clients.
void wifi_ap_state(char *out, size_t len);

// Moves the access point to another channel (1..13) without a restart.
esp_err_t wifi_set_ap_channel(int channel);

// Stores the network and reconnects; ssid == NULL or "" forgets it.
esp_err_t wifi_set_station(const char *ssid, const char *pass);

// One-line description: AP name, STA state and addresses.
void wifi_describe(char *out, size_t len);
