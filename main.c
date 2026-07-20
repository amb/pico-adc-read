#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/timer.h"
#include "tusb.h"
#include <stdio.h>
#include <string.h>

#define ADC_PIN           26
#define ADC_CHANNEL       0
#define BUF_SIZE          512
#define SAMPLE_PERIOD_US  22   // ~45.45 kHz

// Frame: sync(2) + counter(2) + samples(1024) = 1028 bytes
#define FRAME_SIZE  (2 + 2 + BUF_SIZE * 2)

static uint16_t buffers[2][BUF_SIZE];
static volatile int write_buf = 0;
static volatile int ready_buf = -1;
static volatile int write_idx = 0;
static volatile uint32_t frame_counter = 0;
static volatile uint32_t drops = 0;  // frames dropped due to USB backpressure

bool timer_isr(repeating_timer_t *rt) {
    uint16_t val = adc_read();
    buffers[write_buf][write_idx] = val;
    write_idx++;

    if (write_idx >= BUF_SIZE) {
        if (ready_buf == -1) {
            ready_buf = write_buf;
            write_buf = 1 - write_buf;
            write_idx = 0;
        } else {
            // Overrun — main is busy, overwrite
            write_idx = 0;
        }
    }

    return true;
}

int main() {
    stdio_init_all();  // keep for picotool reboot support

    // LED
#if defined(PICO_DEFAULT_LED_PIN)
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#elif defined(CYW43_WL_GPIO_LED_PIN)
    #include "pico/cyw43_arch.h"
    cyw43_arch_init();
#endif

    // ADC
    adc_init();
    adc_gpio_init(ADC_PIN);
    adc_select_input(ADC_CHANNEL);

    // Start sampling timer
    repeating_timer_t timer;
    add_repeating_timer_us(-SAMPLE_PERIOD_US, timer_isr, NULL, &timer);

    // Wait for USB enumeration
    sleep_ms(1500);

    static uint8_t frame[FRAME_SIZE];
    frame[0] = 0xAA;
    frame[1] = 0x55;

    while (true) {
        // USB device task — must be called regularly for CDC to work
        tud_task();

        int buf = ready_buf;
        if (buf != -1 && tud_cdc_connected()) {
            // Build frame
            uint32_t fc = frame_counter++;
            frame[2] = (fc >> 8) & 0xFF;
            frame[3] = fc & 0xFF;

            uint16_t *src = buffers[buf];
            for (int i = 0; i < BUF_SIZE; i++) {
                uint16_t val = src[i];
                frame[4 + i * 2]     = (val >> 8) & 0xFF;
                frame[4 + i * 2 + 1] = val & 0xFF;
            }

            // Write in chunks — handles any buffer size without silent drops
            uint32_t written = 0;
            while (written < FRAME_SIZE) {
                uint32_t avail = tud_cdc_write_available();
                if (avail > 0) {
                    uint32_t n = FRAME_SIZE - written;
                    if (n > avail) n = avail;
                    written += tud_cdc_write(frame + written, n);
                }
                tud_cdc_write_flush();
                tud_task();
            }

            // Toggle LED on successful send
#if defined(PICO_DEFAULT_LED_PIN)
            gpio_put(PICO_DEFAULT_LED_PIN, !gpio_get(PICO_DEFAULT_LED_PIN));
#elif defined(CYW43_WL_GPIO_LED_PIN)
            static bool led_state;
            led_state = !led_state;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_state);
#endif

            ready_buf = -1;
        }
    }
}
