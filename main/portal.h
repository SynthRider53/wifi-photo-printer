#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

// Keeps a phone on the access point. A phone checks for internet access right
// after joining and drops an open network that stays silent -- measured: gone
// two seconds after connecting. Answering that check marks the network as one
// that needs a sign-in, which a phone keeps connected (and keeps using mobile
// data for everything else).

esp_err_t portal_start(void);
esp_err_t portal_http_register(httpd_handle_t srv);
