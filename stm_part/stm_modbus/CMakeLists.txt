cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME stm_modbus)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# ---------------------------------------------------------------------------
# 用户代码：Components（LCD / RS485 / Modbus / UI）+ LVGL + LVGL 端口
# ---------------------------------------------------------------------------

# LVGL 的根目录就是 Middlewares/lvgl（lvgl.h、src/、lv_conf.h 都在里面）。
# 注意：Middlewares/lvgl/src/lv_conf.h 已经被改成一行的转发文件，指向真正的配置
# Middlewares/lvgl/lv_conf.h —— 不然 lv_conf_internal.h 的 __has_include 会先命中
# src/ 里那份模板，导致这里的配置根本不生效。
set(LVGL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/lvgl)

file(GLOB_RECURSE LVGL_SRC CONFIGURE_DEPENDS ${LVGL_ROOT}/src/*.c)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Components/LCD/lcd_st7789.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Components/LED/led.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Components/KEY/key.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Components/RS485/rs485.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Components/Modbus/modbus_master.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Components/Link/esp_link.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Components/UI/ui_env_monitor.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Components/UI/fonts/ui_font_bold_15.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Components/UI/fonts/ui_font_reg_12.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Components/UI/fonts/ui_font_num_26.c
    ${LVGL_ROOT}/porting/lv_port_disp.c
    ${LVGL_SRC}
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Components/LCD
    ${CMAKE_CURRENT_SOURCE_DIR}/Components/LED
    ${CMAKE_CURRENT_SOURCE_DIR}/Components/KEY
    ${CMAKE_CURRENT_SOURCE_DIR}/Components/RS485
    ${CMAKE_CURRENT_SOURCE_DIR}/Components/Modbus
    ${CMAKE_CURRENT_SOURCE_DIR}/Components/Link
    ${CMAKE_CURRENT_SOURCE_DIR}/Components/UI
    ${LVGL_ROOT}
    ${LVGL_ROOT}/porting
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
