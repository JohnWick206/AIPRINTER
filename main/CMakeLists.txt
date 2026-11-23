idf_component_register(
    SRCS "main.c" "display.c" "ui.c" "refine.c" "anim.c" "grokbot.c" "http.c" "image.cpp" "font_cn_24.c" "font_cn_16.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif esp_http_client esp_lcd driver nvs_flash json esp_timer mbedtls lvgl esp_rom
    PRIV_REQUIRES pngdec
)
