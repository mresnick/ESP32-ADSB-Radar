cmake_minimum_required(VERSION 3.16)
project(esp32_adsb_radar_native_tests C)

# Standalone host ("native," in PlatformIO/Zephyr terms - runs on the dev
# machine, not the ESP32 target) build of this repo's host-testable modules
# (plain C, no ESP-IDF dependency) - entirely separate from the ESP-IDF
# project tree (main/CMakeLists.txt's SRC_DIRS glob never sees this
# directory). Build/run with:
#
#   cmake -S test/native -B test/native/build
#   cmake --build test/native/build
#   ./test/native/build/test_aircraft_model
#   ./test/native/build/test_geo_math
#
# or, without CMake at all:
#
#   gcc -std=c11 -I main -I test/native/fakes test/native/test_aircraft_model.c main/aircraft_model.c -o test_aircraft_model
#   gcc -std=c11 -I main test/native/test_geo_math.c main/geo_math.c -lm -o test_geo_math

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(test_aircraft_model
    test_aircraft_model.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../main/aircraft_model.c
)
target_include_directories(test_aircraft_model PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../main
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
)

add_executable(test_geo_math
    test_geo_math.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../main/geo_math.c
)
target_include_directories(test_geo_math PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../main
)
target_link_libraries(test_geo_math PRIVATE m)
