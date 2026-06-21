cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
project(music_player)
include(${ZEPHYR_BASE}/cmake/cfb.cmake NO_POLICY_SCOPE)
target_include_directories(app PRIVATE ${CMAKE_BINARY_DIR}/include)

target_sources(app PRIVATE src/main.cpp src/sd_card.cpp src/gpio.cpp src/mp_lvgl.cpp)
