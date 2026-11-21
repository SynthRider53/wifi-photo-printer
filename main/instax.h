#pragma once
#include "esp_err.h"

// Fujifilm instax SHARE SP-2 emulation, so that "instax printer print" on a
// Fujifilm camera (XQ2 and friends) prints on the thermal printer.
//
// The camera joins the access point (open, named INSTAX-########) and talks to
// a TCP server on port 8080 at the address instax clients expect. It then sends
// the picture as raw planar RGB, 600x800, in 24 segments of 60000 bytes.
// The four-digit password the camera asks for is not a Wi-Fi key: it travels
// inside every packet header (factory default 1111).

esp_err_t instax_start(void);

// Answer to the camera's "function version" query (message 64), which is not
// documented anywhere: tunable from the console while it is being guessed.
void instax_set_function_version(int v);
int  instax_get_function_version(void);

// Size of the parts the camera is asked to send the picture in.
void instax_set_chunk(int bytes);
int  instax_get_chunk(void);
