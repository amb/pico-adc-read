cmake_minimum_required(VERSION 3.13)

# NAM A2-Lite real-time amp modeling on RP2350 with ADC input + PWM output.
# Pico 2 (RP2350) only — dual Cortex-M33 at 300 MHz required for the NAM pipeline.

set(PICO_BOARD pico2)
set(PICO_PLATFORM rp2350)
set(PICO_CXX_ENABLE_EXCEPTIONS 1)

include(pico_sdk_import.cmake)
project(pico_adc_read C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

# ---- Model embedding via nam2c host tool ----
# Place your .nam file in the project root and point nam_set_model() at it,
# or override with -DNAM_MODEL=path/to/tone.nam on the cmake command line.
include(cmake/nam_model.cmake)
if(DEFINED NAM_MODEL)
    nam_set_model(${NAM_MODEL})
else()
    nam_set_model(example.nam)
endif()

# ---- NAM inference engine (NeuralAmpModelerCore, a2_fast path) ----
set(NAMCORE ${CMAKE_CURRENT_LIST_DIR}/lib/NeuralAmpModelerCore)
file(GLOB NAMCORE_SRC ${NAMCORE}/NAM/*.cpp ${NAMCORE}/NAM/wavenet/*.cpp)
add_library(nam_engine STATIC ${NAMCORE_SRC})
target_include_directories(nam_engine BEFORE PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include/compat)
target_include_directories(nam_engine SYSTEM PUBLIC
    ${NAMCORE} ${NAMCORE}/Dependencies/eigen ${NAMCORE}/Dependencies/nlohmann)
target_compile_definitions(nam_engine PUBLIC
    NAM_SAMPLE_FLOAT=1 NAM_ENABLE_A2_FAST=1 NAM_USE_INLINE_GEMM=1
    NAM_SHARED_PTR_ATOMIC_FREE_FUNCS=1 _LIBCPP_VERSION=1
    thread_local=)
target_compile_options(nam_engine PRIVATE -O2 -ffast-math -funroll-loops -Wno-unused-parameter)

# whole-archive so the engine's static-initializer JSON parsers survive linking
set(NAM_ENGINE_OPT -Wl,--whole-archive nam_engine -Wl,--no-whole-archive)

# ---- Firmware target ----
add_executable(pico_adc_read
    src/main.cpp
    src/nam_fx.cpp
    ${NAM_MODEL_SOURCE}
)
target_include_directories(pico_adc_read PRIVATE include src)
target_link_libraries(pico_adc_read
    ${NAM_ENGINE_OPT}
    pico_stdlib
    pico_multicore
    hardware_adc
    hardware_pwm
    hardware_clocks
    hardware_vreg
    hardware_irq
)

# USB CDC for serial + picotool reboot, no UART
pico_enable_stdio_usb(pico_adc_read 1)
pico_enable_stdio_uart(pico_adc_read 0)

pico_add_extra_outputs(pico_adc_read)
