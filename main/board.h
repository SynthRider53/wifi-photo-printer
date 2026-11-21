#pragma once

// ESP32-C3 SuperMini wiring. GPIO8 (LED), GPIO9 (BOOT button) and GPIO2 are
// strapping pins -- the button is only read after boot.
#define PRINTER_UART        UART_NUM_1
#define PRINTER_TX_GPIO     21      // -> printer RXD
#define PRINTER_RX_GPIO     20      // <- printer TXD (optional, 5 V! use a divider)
#define PRINTER_BAUD        9600    // QR204 default; see the self-test page

#define BUTTON_GPIO         9       // BOOT button, active low

// Access point the phone and the cameras join. A Fujifilm camera only talks to
// an instax SHARE printer, whose network is open and named INSTAX-<8 digits>,
// at a fixed address -- so the access point imitates one. The four-digit
// password the camera asks for is not a Wi-Fi key: it rides inside the instax
// protocol (factory default 1111) and we accept any value.
#define WIFI_AP_SSID_PREFIX "INSTAX-"
#define WIFI_AP_CHANNEL     1
#define WIFI_AP_IP          "192.168.0.251"     // where instax clients look for the printer
// The pool may hold at most CONFIG_LWIP_DHCPS_MAX_STATION_NUM addresses (8),
// or the driver quietly throws it away and picks its own.
#define WIFI_AP_DHCP_FIRST  "192.168.0.100"
#define WIFI_AP_DHCP_LAST   "192.168.0.107"

// Tallest picture accepted over HTTP (rows of 48 bytes): 2000 rows = 96 KB, 250 mm.
#define JOB_MAX_ROWS        2000

// Paper pushed out after a picture: clears the case and leaves a margin below
// the picture after tear-off. Measured: ESC J 80 = 9 mm, so ~8.9 dots/mm.
#define PRINTER_TAIL_DOTS   75
