idf_component_register(SRCS "camera.c" "microphone.c" "imu.c" "env_sensor.c" "tof.c" "touch.c"
                    INCLUDE_DIRS "include"
                    REQUIRES driver esp_camera)
