#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "tusb.h"
#include <string.h>

//--------------------------------------------------------------------
// Audio configuration
//--------------------------------------------------------------------

#define SAMPLE_RATE         48000
#define SAMPLE_BUFFER_SIZE  (CFG_TUD_AUDIO_EP_SZ_IN / 2)  // 48 samples
#define ADC_PIN             26
#define ADC_CHANNEL         0
#define ADC_BIAS            2048
#define ADC_SCALE           32
#define RAW_BUFFER_COUNT    2

//--------------------------------------------------------------------
// Global state
//--------------------------------------------------------------------

static uint16_t raw_buffer[RAW_BUFFER_COUNT][SAMPLE_BUFFER_SIZE];
static int16_t pcm_buffer[SAMPLE_BUFFER_SIZE];
static volatile bool pcm_ready = false;
static int dma_channel;
static volatile int raw_read_idx = 0;
static volatile int raw_write_idx = 0;

// Audio control state
static bool mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX + 1];
static uint16_t volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX + 1];
static uint32_t samp_freq = SAMPLE_RATE;
static uint8_t clk_valid = 1;
static audio_control_range_4_n_t(1) sample_freq_rng;

//--------------------------------------------------------------------
// DMA interrupt handler
//--------------------------------------------------------------------

static void dma_irq_handler(void) {
    dma_hw->ints0 = (1u << dma_channel);
    raw_read_idx = raw_write_idx;
    raw_write_idx = (raw_write_idx + 1) % RAW_BUFFER_COUNT;
    dma_channel_transfer_to_buffer_now(dma_channel, raw_buffer[raw_write_idx], SAMPLE_BUFFER_SIZE);
    pcm_ready = true;
}

//--------------------------------------------------------------------
// USB Audio callbacks
//--------------------------------------------------------------------

bool tud_audio_tx_done_pre_load_cb(uint8_t rhport, uint8_t itf,
                                    uint8_t ep_in, uint8_t cur_alt_setting) {
    (void)rhport; (void)itf; (void)ep_in; (void)cur_alt_setting;

    // Convert raw ADC to PCM inline in the USB callback (not in IRQ)
    if (pcm_ready) {
        uint16_t *src = raw_buffer[raw_read_idx];
        for (int i = 0; i < SAMPLE_BUFFER_SIZE; i++) {
            int32_t val = ((int32_t)(src[i] & 0xFFF) - ADC_BIAS) * ADC_SCALE;
            if (val > 32767)  val = 32767;
            if (val < -32768) val = -32768;
            pcm_buffer[i] = (int16_t)val;
        }
        pcm_ready = false;
    } else {
        // Underrun — fill with silence rather than dropping the frame
        memset(pcm_buffer, 0, sizeof(pcm_buffer));
    }

    tud_audio_write((uint8_t *)pcm_buffer, sizeof(pcm_buffer));
    return true;
}

bool tud_audio_tx_done_post_load_cb(uint8_t rhport, uint16_t n_bytes_copied,
                                     uint8_t itf, uint8_t ep_in, uint8_t cur_alt_setting) {
    (void)rhport; (void)n_bytes_copied; (void)itf; (void)ep_in; (void)cur_alt_setting;
    return true;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff) {
    (void)rhport;
    if (p_request->bRequest != AUDIO_CS_REQ_CUR) return false;
    uint8_t channel = TU_U16_LOW(p_request->wValue);
    uint8_t ctrl = TU_U16_HIGH(p_request->wValue);
    uint8_t entity = TU_U16_HIGH(p_request->wIndex);
    if (entity == 2) {
        if (ctrl == AUDIO_FU_CTRL_MUTE) { mute[channel] = ((audio_control_cur_1_t *)pBuff)->bCur; return true; }
        if (ctrl == AUDIO_FU_CTRL_VOLUME) { volume[channel] = ((audio_control_cur_2_t *)pBuff)->bCur; return true; }
    }
    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;
    uint8_t channel = TU_U16_LOW(p_request->wValue);
    uint8_t ctrl = TU_U16_HIGH(p_request->wValue);
    uint8_t entity = TU_U16_HIGH(p_request->wIndex);

    if (entity == 1 && ctrl == AUDIO_TE_CTRL_CONNECTOR) {
        audio_desc_channel_cluster_t ret = { .bNrChannels = 1, .bmChannelConfig = 0, .iChannelNames = 0 };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &ret, sizeof(ret));
    }

    if (entity == 2) {
        if (ctrl == AUDIO_FU_CTRL_MUTE) return tud_control_xfer(rhport, p_request, &mute[channel], 1);
        if (ctrl == AUDIO_FU_CTRL_VOLUME) {
            if (p_request->bRequest == AUDIO_CS_REQ_CUR)
                return tud_control_xfer(rhport, p_request, &volume[channel], sizeof(volume[channel]));
            if (p_request->bRequest == AUDIO_CS_REQ_RANGE) {
                audio_control_range_2_n_t(1) r = { .wNumSubRanges = 1, .subrange[0] = { .bMin = -90, .bMax = 90, .bRes = 1 } };
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &r, sizeof(r));
            }
        }
    }

    if (entity == 4) {
        if (ctrl == AUDIO_CS_CTRL_SAM_FREQ) {
            if (p_request->bRequest == AUDIO_CS_REQ_CUR)
                return tud_control_xfer(rhport, p_request, &samp_freq, sizeof(samp_freq));
            if (p_request->bRequest == AUDIO_CS_REQ_RANGE) {
                sample_freq_rng.wNumSubRanges = 1;
                sample_freq_rng.subrange[0].bMin = SAMPLE_RATE;
                sample_freq_rng.subrange[0].bMax = SAMPLE_RATE;
                sample_freq_rng.subrange[0].bRes = 0;
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &sample_freq_rng, sizeof(sample_freq_rng));
            }
        }
        if (ctrl == AUDIO_CS_CTRL_CLK_VALID) return tud_control_xfer(rhport, p_request, &clk_valid, sizeof(clk_valid));
    }

    return false;
}

bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff) {
    (void)rhport; (void)p_request; (void)pBuff; return false;
}

bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport; (void)p_request; return false;
}

bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport; (void)p_request; return true;
}

//--------------------------------------------------------------------
// Init
//--------------------------------------------------------------------

static void adc_dma_init(void) {
    dma_channel = dma_claim_unused_channel(true);
    dma_channel_config cfg = dma_channel_get_default_config(dma_channel);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_dreq(&cfg, DREQ_ADC);
    dma_channel_configure(dma_channel, &cfg, raw_buffer[0], &adc_hw->fifo, SAMPLE_BUFFER_SIZE, false);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    dma_channel_set_irq0_enabled(dma_channel, true);
    irq_set_enabled(DMA_IRQ_0, true);
}

static void adc_init_48khz(void) {
    adc_gpio_init(ADC_PIN);
    adc_init();
    adc_select_input(ADC_CHANNEL);
    adc_fifo_setup(true, true, 1, false, false);
    float clk_div = (float)clock_get_hz(clk_adc) / (float)SAMPLE_RATE - 1.0f;
    adc_set_clkdiv(clk_div);
}

//--------------------------------------------------------------------
// Main
//--------------------------------------------------------------------

int main(void) {
#if defined(PICO_DEFAULT_LED_PIN)
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#endif

    adc_dma_init();
    adc_init_48khz();
    tusb_init();

    raw_write_idx = 0;
    raw_read_idx = 0;
    dma_channel_transfer_to_buffer_now(dma_channel, raw_buffer[0], SAMPLE_BUFFER_SIZE);
    adc_run(true);

    while (true) {
        tud_task();
        // Note: GPIO access in this loop (e.g., LED toggling) can conflict
        // with TinyUSB's USB register access on the same core and cause crashes.
        // See: https://github.com/raspberrypi/pico-sdk/issues/663
    }
}
