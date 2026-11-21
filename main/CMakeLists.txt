idf_component_register(
    SRCS "main.c" "printer.c" "printjob.c" "dither.c" "testpattern.c" "shell.c" "wifi.c" "webui.c"
         "jpegprint.c" "pictbridge.c" "instax.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver console esp_timer esp_wifi esp_netif esp_event nvs_flash esp_http_server esp_rom lwip
    EMBED_TXTFILES "web/index.html"
    EMBED_FILES "web/pdf.mjs.gz" "web/pdf.worker.mjs.gz"
)
