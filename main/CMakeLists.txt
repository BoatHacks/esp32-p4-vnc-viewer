idf_component_register(
    SRCS
        "main.c"
        "wifi_connect.c"
        "rfb_client.c"
        "vnc_display.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_netif
        nvs_flash
        esp_lcd
        esp_lcd_touch
        mbedtls
        lwip
        driver
)
