#include "shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dither.h"
#include "instax.h"
#include "printer.h"
#include "testpattern.h"
#include "wifi.h"

static int cmd_test(int argc, char **argv)
{
    printf("printing test pattern...\n");
    esp_err_t err = testpattern_print();
    printf("%s\n", err == ESP_OK ? "done" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_feed(int argc, char **argv)
{
    int dots = argc > 1 ? atoi(argv[1]) : 48;
    return printer_feed(dots) == ESP_OK ? 0 : 1;
}

static int cmd_init(int argc, char **argv)
{
    return printer_reset() == ESP_OK ? 0 : 1;
}

static int cmd_baud(int argc, char **argv)
{
    if (argc > 1) {
        int baud = atoi(argv[1]);
        if (baud < 1200 || baud > 921600) {
            printf("baud 1200..921600\n");
            return 1;
        }
        if (printer_set_baud(baud) != ESP_OK) return 1;
    }
    printf("baud = %d\n", printer_get_baud());
    return 0;
}

static int cmd_speed(int argc, char **argv)
{
    if (argc > 1) printer_set_speed(atoi(argv[1]));
    printf("speed = %d lines/s\n", printer_get_speed());
    return 0;
}

// `raw 1f 1b 1f df 05` -- for trying undocumented printer settings by hand.
static int cmd_raw(int argc, char **argv)
{
    uint8_t buf[64];
    int n = 0;
    for (int i = 1; i < argc && n < (int)sizeof buf; i++) buf[n++] = (uint8_t)strtol(argv[i], NULL, 16);
    if (!n) {
        printf("raw <hex byte> ...\n");
        return 1;
    }
    printf("sending %d bytes\n", n);
    return printer_raw(buf, n) == ESP_OK ? 0 : 1;
}

static int cmd_band(int argc, char **argv)
{
    if (argc > 1) printer_set_band(atoi(argv[1]));
    printf("band = %d rows (%.1f mm)\n", printer_get_band(), printer_get_band() / 8.0);
    return 0;
}

static int cmd_fnver(int argc, char **argv)
{
    if (argc > 1) instax_set_function_version(atoi(argv[1]));
    printf("instax function version = %d\n", instax_get_function_version());
    return 0;
}

static int cmd_ixchunk(int argc, char **argv)
{
    if (argc > 1) instax_set_chunk(atoi(argv[1]));
    printf("instax chunk = %d bytes\n", instax_get_chunk());
    return 0;
}

static int cmd_tail(int argc, char **argv)
{
    if (argc > 1) printer_set_tail(atoi(argv[1]));
    printf("tail = %d dots (%.1f mm)\n", printer_get_tail(), printer_get_tail() / 8.0);
    return 0;
}

// Measures which feed method really moves the paper. Five marks, bar i is
// 8*i bytes wide; between neighbours each method should feed 80 dots = 10 mm:
//   1-2: ESC 3 8 + 10 x LF   2-3: ESC J 80   3-4: ESC d 10   4-5: 80 blank raster rows
static void feedtest_mark(int i)
{
    uint8_t rows[2 * PRINTER_ROW_BYTES] = { 0 };
    for (int r = 0; r < 2; r++) memset(rows + r * PRINTER_ROW_BYTES, 0xFF, 8 * i);
    printer_raster(rows, 2);
    vTaskDelay(pdMS_TO_TICKS(1500));
}

static int cmd_feedtest(int argc, char **argv)
{
    static const uint8_t blank[80 * PRINTER_ROW_BYTES];
    static const uint8_t sp[] = { 0x1B, '3', 8 }, esc_j[] = { 0x1B, 'J', 80 }, esc_d[] = { 0x1B, 'd', 10 };
    printer_reset();
    feedtest_mark(1);
    printer_raw(sp, sizeof sp);
    for (int i = 0; i < 10; i++) printer_raw("\n", 1);
    vTaskDelay(pdMS_TO_TICKS(1500));
    feedtest_mark(2);
    printer_raw(esc_j, sizeof esc_j);
    vTaskDelay(pdMS_TO_TICKS(1500));
    feedtest_mark(3);
    printer_raw(esc_d, sizeof esc_d);
    vTaskDelay(pdMS_TO_TICKS(1500));
    feedtest_mark(4);
    printer_raster(blank, 80);
    vTaskDelay(pdMS_TO_TICKS(1500));
    feedtest_mark(5);
    printer_raw("\n\n\n\n\n\n", 6);
    printf("done\n");
    return 0;
}

// wifi            -> show addresses
// wifi <ssid> [pass] -> join a home network too (AP stays on)
// wifi clear      -> forget the home network
static int cmd_wifi(int argc, char **argv)
{
    if (argc > 1) {
        esp_err_t err = strcmp(argv[1], "clear") == 0 ? wifi_set_station(NULL, NULL)
                                                      : wifi_set_station(argv[1], argc > 2 ? argv[2] : "");
        if (err != ESP_OK) {
            printf("failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        vTaskDelay(pdMS_TO_TICKS(6000));
    }
    char line[220];
    wifi_describe(line, sizeof line);
    printf("%s\n", line);
    return 0;
}

static int cmd_heap(int argc, char **argv)
{
    printf("heap free %u, largest block %u, minimum ever %u\n",
           (unsigned)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
           (unsigned)esp_get_minimum_free_heap_size());
    return 0;
}

static int cmd_settle(int argc, char **argv)
{
    if (argc > 1) printer_set_settle(atoi(argv[1]));
    printf("settle = %d ms\n", printer_get_settle());
    return 0;
}

static int cmd_dither(int argc, char **argv)
{
    if (argc > 1) dither_set_mode(argv[1][0] == 'b' ? DITHER_BAYER : DITHER_FS);
    printf("dither = %s\n", dither_get_mode() == DITHER_BAYER ? "bayer" : "fs");
    return 0;
}

// heat <n1> <n2> <n3> / density <0..31>: tune print quality, then run `test`.
static int cmd_heat(int argc, char **argv)
{
    int d, t, g, n;
    if (argc == 4 && printer_set_heat(atoi(argv[1]), atoi(argv[2]), atoi(argv[3])) != ESP_OK) {
        printf("n1 0..255, n2 3..255, n3 0..255\n");
        return 1;
    }
    printer_get_tuning(&d, &t, &g, &n);
    printf("heat: max dots %d, time %d us, gap %d us\n", 8 * (d + 1), t * 10, g * 10);
    return 0;
}

static int cmd_density(int argc, char **argv)
{
    int d, t, g, n;
    if (argc > 1 && printer_set_density(atoi(argv[1])) != ESP_OK) {
        printf("density 0..31\n");
        return 1;
    }
    printer_get_tuning(&d, &t, &g, &n);
    printf("density = %d\n", n);
    return 0;
}

// Prints one line per UART speed. Only the speeds the printer really runs at
// come out readable; the rest is garbage. The configured speed is restored.
static int cmd_baudtest(int argc, char **argv)
{
    static const int rates[] = { 9600, 19200, 38400, 57600, 115200 };
    int keep = printer_get_baud();
    for (size_t i = 0; i < sizeof rates / sizeof rates[0]; i++) {
        char line[24];
        snprintf(line, sizeof line, "BAUD %d OK", rates[i]);
        printer_set_baud(rates[i]);
        printer_text(line);
        vTaskDelay(pdMS_TO_TICKS(600));
    }
    printer_set_baud(keep);
    printf("done: note which lines are readable\n");
    return 0;
}

// DLE EOT 1..4: printer / offline / error / paper sensor status bytes.
static int cmd_status(int argc, char **argv)
{
    bool any = false;
    for (int n = 1; n <= 4; n++) {
        uint8_t b;
        if (printer_status(n, &b, 300) == ESP_OK) {
            printf("DLE EOT %d -> 0x%02X\n", n, b);
            any = true;
        }
    }
    if (!any) printf("no answer (printer TXD not wired, or wrong baud)\n");
    return 0;
}

esp_err_t shell_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "printer> ";
    esp_console_dev_usb_serial_jtag_config_t dev = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_usb_serial_jtag(&dev, &repl_cfg, &repl), "shell", "repl");

    const esp_console_cmd_t cmds[] = {
        { .command = "test",   .help = "print the calibration strip",        .func = cmd_test },
        { .command = "feed",   .help = "feed paper [dots, default 48]",      .func = cmd_feed },
        { .command = "init",   .help = "ESC @ printer reset",                .func = cmd_init },
        { .command = "baud",   .help = "show/set printer UART baud rate",    .func = cmd_baud },
        { .command = "speed",  .help = "show/set max lines per second",      .func = cmd_speed },
        { .command = "tail",   .help = "show/set paper feed after a picture, dots", .func = cmd_tail },
        { .command = "band",   .help = "show/set rows per raster command",   .func = cmd_band },
        { .command = "raw",    .help = "send raw bytes to the printer, e.g. `raw 1b 40`", .func = cmd_raw },
        { .command = "fnver",  .help = "show/set the instax \"function version\" answer", .func = cmd_fnver },
        { .command = "ixchunk", .help = "show/set the instax transfer part size",  .func = cmd_ixchunk },
        { .command = "feedtest", .help = "print marks to measure feed methods", .func = cmd_feedtest },
        { .command = "wifi",   .help = "show / `wifi <ssid> [pass]` / `wifi clear`", .func = cmd_wifi },
        { .command = "heap",   .help = "show free memory",                   .func = cmd_heap },
        { .command = "settle", .help = "show/set pause before feed, ms",     .func = cmd_settle },
        { .command = "dither", .help = "dither fs|bayer",                    .func = cmd_dither },
        { .command = "heat",   .help = "ESC 7 <n1 0..255> <n2 3..255> <n3 0..255>", .func = cmd_heat },
        { .command = "density", .help = "DC2 # density 0..31",               .func = cmd_density },
        { .command = "baudtest", .help = "print a line at 9600..115200",     .func = cmd_baudtest },
        { .command = "status", .help = "ask the printer for status bytes",   .func = cmd_status },
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmds[i]), "shell", "register");
    }
    return esp_console_start_repl(repl);
}
