cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(pico_adc_read C CXX ASM)

pico_sdk_init()

add_executable(pico_adc_read
    main.c
)

target_link_libraries(pico_adc_read
    pico_stdlib
    hardware_pwm
)

# USB CDC for serial + picotool reboot, no audio
pico_enable_stdio_usb(pico_adc_read 1)
pico_enable_stdio_uart(pico_adc_read 0)

pico_add_extra_outputs(pico_adc_read)
