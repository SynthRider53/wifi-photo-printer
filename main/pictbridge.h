#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

// PictBridge over IP (CIPA DC-001 + DC-005 + DCG-006): the board shows up as a
// "DPS printer" on the Wi-Fi, so the camera's "Print from Wi-Fi printer" finds
// it and sends the picture straight to the thermal printer.
//
//  1. SSDP (UDP 1900) announces the DPSPrinter device.
//  2. The camera fetches /desc.xml and POSTs ConnectionRequest to /root/soap_folder.
//  3. The board connects back to the camera (PTP-IP, TCP 15740) as the PTP
//     initiator and plays the DPS "host": discovery, configurePrintService,
//     getCapability, startJob; then it pulls the JPEG from the camera.

// SSDP responder; call once the Wi-Fi is up.
esp_err_t pictbridge_start(void);

// UPnP pages and the SOAP control URL, served by the main web server (port 80).
esp_err_t pictbridge_http_register(httpd_handle_t srv);
