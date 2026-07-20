#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

#define CFG_TUSB_MCU             OPT_MCU_RP2040
#define CFG_TUSB_RHPORT0_MODE    OPT_MODE_DEVICE
// CFG_TUSB_OS is set by Pico SDK — do not override
#define CFG_TUSB_DEBUG           0

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN       __attribute__ ((aligned(4)))
#endif

//--------------------------------------------------------------------
// DEVICE CONFIGURATION
//--------------------------------------------------------------------

#define CFG_TUD_ENDPOINT0_SIZE   64

//------------- CLASS -------------//
#define CFG_TUD_CDC              0
#define CFG_TUD_MSC              0
#define CFG_TUD_HID              0
#define CFG_TUD_MIDI             0
#define CFG_TUD_AUDIO            1
#define CFG_TUD_VENDOR           0

//--------------------------------------------------------------------
// AUDIO CLASS DRIVER CONFIGURATION
//--------------------------------------------------------------------

// CDC FIFO size
#define CFG_TUD_CDC_RX_BUFSIZE   64
#define CFG_TUD_CDC_TX_BUFSIZE   64

// Mono microphone, 48 kHz, 16-bit
#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN                     TUD_AUDIO_MIC_ONE_CH_DESC_LEN
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT                     1
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ                  64

#define CFG_TUD_AUDIO_ENABLE_EP_IN                        1
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX        2
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX                1

// 48 samples per 1ms USB frame × 2 bytes × 1 channel = 96 bytes
#define CFG_TUD_AUDIO_EP_SZ_IN                            (48 * 2 * 1)
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX                 CFG_TUD_AUDIO_EP_SZ_IN
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ              CFG_TUD_AUDIO_EP_SZ_IN

#ifdef __cplusplus
}
#endif

#endif
