idf_component_register(SRCS "nvs_config.c" "ota.c" "log_buf.c"
                    INCLUDE_DIRS "include"
                    REQUIRES nvs_flash app_update)
