idf_component_register(SRCS "conn_manager.c" "ws_client.c" "ble_gatt.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_wifi esp_bt nvs_flash)
