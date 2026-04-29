idf_component_register(SRCS "main.c" "trigger_input.c" "audio_in.c" "cloud_client.c" "audio_out.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_http_client json driver esp_timer esp_event esp_netif esp_wifi nvs_flash
                                esp_vocat spiffs)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../spiffs")
spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)
