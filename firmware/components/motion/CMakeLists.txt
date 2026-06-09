idf_component_register(SRCS "motor.c" "servo.c" "obstacle.c"
                    INCLUDE_DIRS "include"
                    REQUIRES driver bcp localization)
