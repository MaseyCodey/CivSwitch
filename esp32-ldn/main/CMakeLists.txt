idf_component_register(
  SRCS "main.c" "civ7.c" "installer.c"
  INCLUDE_DIRS "."
  REQUIRES esp_wifi esp_netif esp_event esp_timer nvs_flash esp_driver_gpio wpa_supplicant mbedtls lwip esp_partition
)
