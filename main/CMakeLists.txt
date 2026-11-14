idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "page.html"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_server
                                     esp_driver_tsens app_update bootloader_support spi_flash
                                     lwip esp_timer esp_hw_support esp_psram)
