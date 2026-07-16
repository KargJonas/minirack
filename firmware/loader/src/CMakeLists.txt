idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_wifi esp_eth esp_netif esp_event esp_http_server app_update esp_app_format
)
