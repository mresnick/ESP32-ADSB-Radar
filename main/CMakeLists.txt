idf_component_register(
    SRC_DIRS "." "Config" "LCD" "GUI" "Fonts"
    INCLUDE_DIRS "." "Config" "LCD" "GUI" "Fonts"
    REQUIRES driver esp_wifi esp_event esp_netif lwip
             esp_http_server esp_http_client nvs_flash esp_timer mbedtls
)
