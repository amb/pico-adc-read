cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(pico_adc_read C CXX ASM)

pico_sdk_init()

add_executable(pico_adc_read
    main.c
    usb_descriptors.c
)

# Project root for tusb_config.h
target_include_directories(pico_adc_read PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(pico_adc_read
    pico_stdlib
    hardware_adc
    hardware_dma
    hardware_clocks
    hardware_irq
    tinyusb_device
    tinyusb_board
)

pico_add_extra_outputs(pico_adc_read)
