cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(pico_adc_read C CXX ASM)

pico_sdk_init()

add_executable(pico_adc_read main.c)

target_link_libraries(pico_adc_read
    pico_stdlib
    pico_stdio_usb
    hardware_adc
)

# Increase CDC TX buffer so a full frame fits in one write
target_compile_definitions(pico_adc_read PRIVATE
    CFG_TUD_CDC_TX_BUFSIZE=2048
)

pico_add_extra_outputs(pico_adc_read)
