#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "hardware/sync.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include <stdio.h>

//--------------------------------------------------------------------
// Audio configuration
//--------------------------------------------------------------------

#define AUDIO_PIN           19       // GP19 (pin 25) — PWM output → RC filter → audio
#define ADC_PIN             26       // GP26 (pin 31) — ADC0 input
#define ADC_CHANNEL         0
#define PWM_FREQ_HZ         200000   // PWM carrier frequency

//--------------------------------------------------------------------
// ADC / PWM parameters
//--------------------------------------------------------------------

#define ADC_BIAS            2048     // 12-bit ADC midpoint (1.65 V with 3.3 V ref)
#define PWM_DIVIDER         1.0f
#define PWM_WRAP            624
#define PWM_MID             (PWM_WRAP / 2)

//--------------------------------------------------------------------
// Test tone
//--------------------------------------------------------------------

#define TONE_FREQ_HZ        440
#define TONE_AMPLITUDE       250

//--------------------------------------------------------------------
// Modes
//--------------------------------------------------------------------

typedef enum { MODE_ADC, MODE_TONE, MODE_COUNT } audio_mode_t;
static volatile audio_mode_t current_mode = MODE_ADC;

//--------------------------------------------------------------------
// BOOTSEL button — must run from SRAM (disables flash access temporarily)
//--------------------------------------------------------------------

bool __no_inline_not_in_flash_func(get_bootsel_button)(void) {
    const uint CS_PIN_INDEX = 1;

    uint32_t flags = save_and_disable_interrupts();

    // Float flash CS pin
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    // Wait for pin to settle — no flash access allowed here
    for (volatile int i = 0; i < 1000; ++i);

#if PICO_RP2040
#define CS_BIT (1u << 1)
#else
#define CS_BIT SIO_GPIO_HI_IN_QSPI_CSN_BITS
#endif
    bool pressed = !(sio_hw->gpio_hi_in & CS_BIT);

    // Restore flash CS control
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    restore_interrupts(flags);
    return pressed;
}

//--------------------------------------------------------------------
// Audio timer — fires at ~47.6 kHz
//--------------------------------------------------------------------

static repeating_timer_t audio_timer;

static uint16_t adc_to_pwm(uint16_t raw) {
    int32_t val = (int32_t)raw - ADC_BIAS;
    val = val * (int32_t)(PWM_WRAP / 2) / ADC_BIAS;
    return PWM_MID + (uint16_t)val;
}

bool audio_timer_callback(repeating_timer_t *rt) {
    uint16_t level;

    if (current_mode == MODE_ADC) {
        if (adc_fifo_is_empty()) {
            pwm_set_gpio_level(AUDIO_PIN, PWM_MID);
            return true;
        }
        level = adc_to_pwm(adc_fifo_get());
    } else {
        // 440 Hz square wave via phase accumulator
        static uint32_t phase = 0;
        const uint32_t inc = (uint32_t)((float)TONE_FREQ_HZ * 16777216.0f / 47619.0f + 0.5f);
        phase += inc;
        level = (phase & 0x00800000) ? (PWM_MID - TONE_AMPLITUDE) : (PWM_MID + TONE_AMPLITUDE);
    }

    pwm_set_gpio_level(AUDIO_PIN, level);
    return true;
}

//--------------------------------------------------------------------
// Init
//--------------------------------------------------------------------

static void pwm_audio_init(void) {
    gpio_set_function(AUDIO_PIN, GPIO_FUNC_PWM);
    uint slice_num = pwm_gpio_to_slice_num(AUDIO_PIN);

    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, PWM_DIVIDER);
    pwm_config_set_wrap(&config, PWM_WRAP);
    pwm_init(slice_num, &config, true);
    pwm_set_gpio_level(AUDIO_PIN, PWM_MID);
}

static void adc_init_free_running(void) {
    adc_gpio_init(ADC_PIN);
    adc_init();
    adc_select_input(ADC_CHANNEL);
    adc_fifo_setup(true, false, 1, false, false);
    adc_set_clkdiv(0);
    adc_run(true);
}

static void audio_timer_init(void) {
    add_repeating_timer_us(-21, audio_timer_callback, NULL, &audio_timer);
}

//--------------------------------------------------------------------
// Main
//--------------------------------------------------------------------

int main(void) {
#if defined(PICO_DEFAULT_LED_PIN)
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#endif

    stdio_init_all();

    printf("ADC/PWM guitar passthrough — BOOTSEL toggles 440 Hz tone\n");

    pwm_audio_init();
    adc_init_free_running();
    sleep_us(200);

    printf("Mode: ADC passthrough  GP%d → GP%d @ %d Hz carrier\n",
           ADC_PIN, AUDIO_PIN, PWM_FREQ_HZ);

    sleep_ms(1500);  // USB enumeration
    audio_timer_init();

    // Main loop — poll BOOTSEL button with debounce, toggle mode on press
    bool last_button = false;
    uint32_t next_poll = time_us_32();

    while (true) {
        // Poll button every 10 ms
        if (time_us_32() - next_poll > 10000) {
            next_poll += 10000;

            bool pressed = get_bootsel_button();

            // Toggle mode on falling edge (release)
            if (last_button && !pressed) {
                current_mode = (current_mode + 1) % MODE_COUNT;
                printf("Mode: %s\n",
                       current_mode == MODE_ADC ? "ADC passthrough"
                                               : "440 Hz square wave");
            }
            last_button = pressed;
        }

        // LED heartbeat — pattern indicates mode
        // MODE_ADC: slow blink (1 Hz), MODE_TONE: fast blink (4 Hz)
        uint32_t period = (current_mode == MODE_ADC) ? 500000 : 125000;
#if defined(PICO_DEFAULT_LED_PIN)
        static uint32_t next_blink = 0;
        if (time_us_32() - next_blink > period) {
            next_blink += period;
            static bool led_on = true;
            gpio_put(PICO_DEFAULT_LED_PIN, led_on);
            led_on = !led_on;
        }
#endif
    }
}
