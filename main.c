#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include <stdio.h>

//--------------------------------------------------------------------
// Audio configuration
//--------------------------------------------------------------------

#define AUDIO_PIN           19       // GP19 (pin 25) — PWM output → RC filter → audio
#define PWM_FREQ_HZ         200000   // PWM carrier frequency

//--------------------------------------------------------------------
// Test tone
//--------------------------------------------------------------------

#define TONE_FREQ_HZ        440      // A4
#define AMPLITUDE           250      // PWM level swing (0..PWM_WRAP)

//--------------------------------------------------------------------
// PWM parameters
//--------------------------------------------------------------------

#define PWM_DIVIDER         1.0f
#define PWM_WRAP            624
#define PWM_MID             (PWM_WRAP / 2)

//--------------------------------------------------------------------
// Audio timer
//--------------------------------------------------------------------

static repeating_timer_t audio_timer;

bool audio_timer_callback(repeating_timer_t *rt) {
    static uint32_t phase = 0;
    const uint32_t phase_inc = (uint32_t)((float)TONE_FREQ_HZ * 16777216.0f / 47619.0f + 0.5f);

    phase += phase_inc;

    uint16_t level = (phase & 0x00800000) ? (PWM_MID - AMPLITUDE) : (PWM_MID + AMPLITUDE);
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

static void audio_timer_init(void) {
    add_repeating_timer_us(-21, audio_timer_callback, NULL, &audio_timer);
}

//--------------------------------------------------------------------
// Main
//--------------------------------------------------------------------

int main(void) {
    // LED heartbeat on Pico's built-in LED (GP25)
#if defined(PICO_DEFAULT_LED_PIN)
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#endif

    // USB CDC — takes a moment to enumerate
    stdio_init_all();

    printf("PWM tone test starting...\n");

    pwm_audio_init();
    printf("PWM init done on GP%d\n", AUDIO_PIN);

    sleep_ms(1500);  // Give USB time to enumerate
    printf("Starting 440 Hz tone\n");

    audio_timer_init();

    // Blink LED at 2 Hz as heartbeat — confirms code is alive
    uint32_t next_blink = time_us_32();
    while (true) {
        if (time_us_32() - next_blink > 250000) {
            next_blink += 250000;
#if defined(PICO_DEFAULT_LED_PIN)
            static bool led_on = true;
            gpio_put(PICO_DEFAULT_LED_PIN, led_on);
            led_on = !led_on;
#endif
        }
    }
}
