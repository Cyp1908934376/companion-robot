cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(companion-robot-test)

# Test component: compiles localization.c directly with test harness
idf_component_register(SRCS "test_localization.c"
                    INCLUDE_DIRS "." "../components/localization/include"
                    REQUIRES unity bcp)
