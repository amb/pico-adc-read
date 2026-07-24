// NAM-on-PWM: Neural Amp Modeler A2-Lite on RP2350 with ADC input + PWM output.
//
// GP26 (pin 31) — ADC audio input (guitar signal, AC-coupled, biased to ~1.65V)
// GP19 (pin 25) — PWM audio output @ 200 kHz carrier → RC low-pass filter
//
// BOOTSEL cycles: NAM active → passthrough → 440 Hz test tone.
// LED: solid = NAM, slow blink = passthrough, fast blink = tone.
//
// Single 48 kHz timer ISR handles all audio I/O:
//   Tone:       generate square wave → PWM (direct)
//   Passthrough: read ADC FIFO → PWM (direct)
//   NAM:         read ADC FIFO → collect 48-sample blocks → ring buffer
//                for main-loop NAM processing. Output from ring → PWM.

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/sync.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/vreg.h"
#include <cstdio>
#include <cstring>

#include "nam_fx.h"

//----------------------------------------------------------------------------
// Hardware pins
//----------------------------------------------------------------------------

#define AUDIO_PIN        19
#define ADC_PIN          26
#define ADC_CHANNEL      0

//----------------------------------------------------------------------------
// Audio parameters
//----------------------------------------------------------------------------

#define SAMPLE_RATE      48000
#define NFRAMES          48       // NAM block size (1 ms at 48 kHz)
#define PWM_FREQ_HZ      200000
#define PWM_WRAP         624
#define PWM_MID          (PWM_WRAP / 2)

#define ADC_BIAS          2048
#define TONE_FREQ_HZ      440
#define TONE_AMPLITUDE    250     // raw PWM units

// Output ring buffer (NAM mode only)
#define RING_SIZE         192

// Modes
typedef enum { MODE_NAM, MODE_PASSTHROUGH, MODE_TONE, MODE_COUNT } audio_mode_t;
static volatile audio_mode_t mode = MODE_NAM;

//----------------------------------------------------------------------------
// Ring buffer (NAM output → ISR)
//----------------------------------------------------------------------------

static float    out_ring[RING_SIZE];
static volatile uint32_t ring_w = 0;
static volatile uint32_t ring_r = 0;
static float    ring_last = 0.0f;

static inline void ring_push(const float* src) {
    uint32_t idx = ring_w % RING_SIZE;
    std::memcpy(&out_ring[idx], src, (size_t)NFRAMES * sizeof(float));
    __dmb();
    ring_w += NFRAMES;
}

static inline float ring_pop(void) {
    if (ring_r == ring_w) return ring_last;
    float s = out_ring[ring_r % RING_SIZE];
    __dmb();
    ring_r++;
    ring_last = s;
    return s;
}

//----------------------------------------------------------------------------
// ADC input double-buffer (ISR writes, main loop reads in NAM mode)
//----------------------------------------------------------------------------

static float    adc_in_buf[2][NFRAMES];
static volatile int  adc_in_ready = -1;   // buffer index ready for main loop (-1 = none)
static          int  adc_in_idx = 0;      // which buffer the ISR is filling
static          int  adc_in_pos = 0;      // position within that buffer

//----------------------------------------------------------------------------
// Conversion helpers
//----------------------------------------------------------------------------

static inline float adc_to_float(uint16_t raw) {
    return ((float)((int32_t)raw - ADC_BIAS)) * (1.0f / 2048.0f);
}

static inline uint16_t float_to_pwm(float x) {
    if (x > 1.0f)  x = 1.0f;
    if (x < -1.0f) x = -1.0f;
    return (uint16_t)(PWM_MID + (int32_t)(x * (float)PWM_MID));
}

//----------------------------------------------------------------------------
// 48 kHz audio timer ISR — handles everything
//----------------------------------------------------------------------------

static repeating_timer_t audio_timer;
static uint32_t tone_phase = 0;
static const uint32_t tone_inc =
    (uint32_t)((float)TONE_FREQ_HZ * 16777216.0f / (float)SAMPLE_RATE + 0.5f);

static bool audio_timer_callback(repeating_timer_t* /*rt*/) {
    uint16_t level = PWM_MID;

    if (mode == MODE_TONE) {
        tone_phase += tone_inc;
        level = (tone_phase & 0x00800000)
            ? (uint16_t)(PWM_MID - TONE_AMPLITUDE)
            : (uint16_t)(PWM_MID + TONE_AMPLITUDE);
        // still drain ADC FIFO to prevent stall
        if (!adc_fifo_is_empty()) adc_fifo_get();

    } else if (mode == MODE_PASSTHROUGH) {
        if (!adc_fifo_is_empty()) {
            uint16_t raw = adc_fifo_get();
            level = float_to_pwm(adc_to_float(raw));
        }

    } else { // MODE_NAM
        // Read ADC sample into double-buffer
        if (!adc_fifo_is_empty()) {
            uint16_t raw = adc_fifo_get();
            adc_in_buf[adc_in_idx][adc_in_pos] = adc_to_float(raw);
            adc_in_pos++;
            if (adc_in_pos >= NFRAMES) {
                adc_in_pos = 0;
                adc_in_ready = adc_in_idx;
                adc_in_idx = 1 - adc_in_idx;
            }
        }

        // Output from ring buffer
        float s = ring_pop();
        level = float_to_pwm(s);
    }

    pwm_set_gpio_level(AUDIO_PIN, level);
    return true;
}

//----------------------------------------------------------------------------
// PWM init
//----------------------------------------------------------------------------

static void pwm_audio_init(void) {
    gpio_set_function(AUDIO_PIN, GPIO_FUNC_PWM);
    uint slice_num = pwm_gpio_to_slice_num(AUDIO_PIN);
    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, 1.0f);
    pwm_config_set_wrap(&config, PWM_WRAP);
    pwm_init(slice_num, &config, true);
    pwm_set_gpio_level(AUDIO_PIN, PWM_MID);
}

//----------------------------------------------------------------------------
// ADC init — free-running, FIFO drained by ISR
//----------------------------------------------------------------------------

static void adc_init_all(void) {
    adc_gpio_init(ADC_PIN);
    adc_init();
    adc_select_input(ADC_CHANNEL);
    adc_set_clkdiv(64);   // ~48 kHz at 300 MHz
    adc_fifo_setup(true, false, 1, false, false);  // FIFO on, no DMA DREQ
    adc_run(true);
}

//----------------------------------------------------------------------------
// BOOTSEL button — SRAM-resident
//----------------------------------------------------------------------------

bool __no_inline_not_in_flash_func(get_bootsel_button)(void) {
    const uint CS_PIN_INDEX = 1;
    uint32_t flags = save_and_disable_interrupts();
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    for (volatile int i = 0; i < 1000; ++i);
#if PICO_RP2040
#define CS_BIT (1u << 1)
#else
#define CS_BIT SIO_GPIO_HI_IN_QSPI_CSN_BITS
#endif
    bool pressed = !(sio_hw->gpio_hi_in & CS_BIT);
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    restore_interrupts(flags);
    return pressed;
}

//----------------------------------------------------------------------------
// Main
//----------------------------------------------------------------------------

int main(void) {
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(300000, true);

#if defined(PICO_DEFAULT_LED_PIN)
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, false);
#endif

    stdio_init_all();
    printf("\nNAM-on-PWM — RP2350 @ 300 MHz\n");
    printf("BOOTSEL cycles: NAM / passthrough / 440 Hz tone\n");

    pwm_audio_init();
    adc_init_all();
    sleep_us(200);

    nam_fx_init();
    printf("NAM engine: %s\n", nam_fx_name());

    sleep_ms(1500);
    add_repeating_timer_us(-21, audio_timer_callback, nullptr, &audio_timer);

    // Main loop
    bool last_button = false;
    uint32_t next_button_poll = time_us_32();
    uint32_t next_blink = 0;
    bool led_state = false;

    while (true) {
        // --- BOOTSEL poll (10 ms) ---
        if (time_us_32() - next_button_poll > 10000) {
            next_button_poll += 10000;
            multicore_lockout_start_blocking();
            bool pressed = get_bootsel_button();
            multicore_lockout_end_blocking();

            if (last_button && !pressed) {
                mode = (audio_mode_t)((mode + 1) % MODE_COUNT);
                if (mode == MODE_NAM) {
                    nam_fx_reset();
                    // Reset input double-buffer state
                    adc_in_ready = -1;
                    adc_in_pos = 0;
                }
                printf("Mode: %s\n",
                       mode == MODE_NAM ? "NAM active" :
                       mode == MODE_PASSTHROUGH ? "Passthrough" : "440 Hz tone");
            }
            last_button = pressed;
        }

        // --- LED ---
#if defined(PICO_DEFAULT_LED_PIN)
        uint32_t period = (mode == MODE_NAM) ? 0 :
                          (mode == MODE_PASSTHROUGH) ? 500000 : 125000;
        if (period == 0) {
            gpio_put(PICO_DEFAULT_LED_PIN, true);
        } else if (time_us_32() - next_blink > period) {
            next_blink += period;
            led_state = !led_state;
            gpio_put(PICO_DEFAULT_LED_PIN, led_state);
        }
#endif

        // --- NAM block processing ---
        if (mode == MODE_NAM && adc_in_ready >= 0) {
            int buf_idx = adc_in_ready;
            adc_in_ready = -1;

            if ((RING_SIZE - (ring_w - ring_r)) >= NFRAMES) {
                float out_float[NFRAMES];
                nam_fx_process(out_float, adc_in_buf[buf_idx], NFRAMES);
                ring_push(out_float);
            }
        }
    }

    return 0;
}
