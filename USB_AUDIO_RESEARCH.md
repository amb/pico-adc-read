# USB Audio Interface — Research & Roadmap

## Current Approach vs. USB Audio Class

| | Current (CDC bulk) | USB Audio Class (UAC 1.0) |
|---|---|---|
| **USB transport** | CDC bulk (stdio/tud_cdc_write) | Isochronous (guaranteed bandwidth) |
| **Host sees** | Serial port → custom Python app | Standard USB microphone → any DAW |
| **Latency** | ~15–45 ms | ~2–4 ms |
| **Sample rate** | ~45.45 kHz (timer-limited) | Exactly 48 kHz (ADC clock divider) |
| **ADC mode** | Timer ISR, one read per call | DMA free-running, zero CPU per sample |
| **Buffer** | 512 samples (~11.3 ms) | 48 samples (1 USB frame = 1 ms) |
| **Robustness** | Drops frames on USB backpressure | Isochronous — no retransmission, but guaranteed slots |

## How USB Audio Class Works

The RP2040 presents itself as a **USB Audio Class 1.0 (UAC1) microphone**. The host OS recognizes it
natively (no drivers) and any DAW or audio app can use it as an input device.

```
DMA: ADC free-running → raw_buffer[2] ping-pong → DMA ISR → PCM conversion → USB isochronous IN endpoint
                                                                                    ↓
                                                                              1ms USB frame
                                                                              (48 samples × 16-bit × 1 channel)
```

### Key Components

1. **DMA-driven ADC** — The RP2040 ADC runs in free-running mode with a precisely calculated
   clock divider for exactly 48 kHz. DMA transfers each conversion result into a buffer with
   zero CPU overhead. When one buffer fills, DMA switches to the other (ping-pong) and fires an
   interrupt.

2. **Sample conversion** — Raw 12-bit ADC values (0–4095) are converted to signed 16-bit PCM:
   - Subtract bias (midpoint of the input range) to center around 0
   - Scale to fill the 16-bit signed range
   - Example from the analog microphone library: `((*in++ & 0xFFF) - 2048) * 64`
     This maps 0→-131072 (clamped to -32768), 2048→0, 4095→131008 (clamped to 32767)

3. **USB isochronous transfers** — Every 1 ms (USB Full Speed frame), the Pico sends 48
   samples (96 bytes) on the isochronous IN endpoint. The host USB controller reserves
   bandwidth for this; it cannot be delayed by other traffic.

4. **TinyUSB audio callbacks** — `tud_audio_tx_done_pre_load_cb()` is called when TinyUSB
   needs more audio data. The callback writes the next buffer of PCM samples and returns.

## Latency Analysis

### Current approach
```
Timer ISR (22µs/sample) → 512-sample buffer fill (11.3ms) → tud_cdc_write (~1ms) → Python read + render (~5-30ms)
Total: ~15-45ms
```

### USB Audio Class
```
DMA (20.8µs/sample) → 48-sample buffer fill (1ms) → Isochronous transfer (1ms USB frame) → DAW input buffer
Total: ~2-4ms (mostly the DAW's audio buffer, typically 64-256 samples at 48kHz = 1.3-5.3ms)
```

The USB audio class is **5-10× lower latency** because:
- DMA eliminates per-sample ISR overhead
- 1ms transfer granularity vs. 11ms batch
- No Python layer between ADC and audio system
- DAW sees it as a native audio device with its own low-latency audio pipeline

## Analog Front-End Requirements for Guitar

The RP2040 ADC cannot directly accept a guitar signal. A preamp/buffer circuit is necessary:

| Issue | Why | Fix |
|-------|-----|-----|
| **High impedance** | Guitar pickups are 5–15 kΩ; ADC input impedance is ~10 kΩ at 48 kHz | JFET/op-amp buffer (≥1 MΩ input impedance) |
| **AC coupling** | Guitar signal is AC centered at 0V; ADC expects 0–3.3V | Bias to Vref/2 (~1.65V) via voltage divider + coupling cap |
| **Low signal level** | Guitar is ~100 mV–1V pk-pk; ADC full scale is 3.3V | Gain stage (2–10×) to use more ADC range |
| **Anti-aliasing** | No filter before ADC → frequencies above 24 kHz alias | Simple RC low-pass at ~20 kHz before ADC pin |

**Recommended signal chain:**
```
Guitar → JFET buffer (1MΩ input) → Gain stage (2-10×) → Bias to 1.65V → RC low-pass (20kHz) → GPIO 26 (ADC0)
```

A MAX9814 module (electret mic amp with AGC) or a simple TL072 op-amp circuit would work.
The `microphone-library-for-pico` already supports MAX9814 at 1.25V bias; for guitar you'd
want 1.65V bias and more gain control.

## Implementation Plan

### Phase 1: Standalone USB Microphone (proven path)

Based on the [Arm Microphone Library](https://github.com/ArmDeveloperEcosystem/microphone-library-for-pico)
and the [analog ADC fork](https://github.com/mdhosale/microphone-library-for-pico-analogpassthrough-midiIO):

1. Clone the microphone library
2. Configure for analog microphone, 48 kHz, mono, 16-bit
3. Disable MIDI (simplifies the build)
4. This gives a working USB microphone recognized by macOS/Ableton

**Key `tusb_config.h` settings:**
```c
#define CFG_TUD_CDC     0    // Disable CDC for simplicity (or keep for debug)
#define CFG_TUD_MIDI    0
#define CFG_TUD_AUDIO   1
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX    1    // Mono
#define CFG_TUD_AUDIO_EP_SZ_IN  (48 + 1) * 2 * 1    // 48 samples × 2 bytes × 1 ch
```

### Phase 2: Add CDC for control (optional)

Keep a CDC interface alongside audio for:
- `picotool reboot -f -u` support
- Debug logging
- Runtime parameter control (gain, sample rate)

**Critical gotcha:** Endpoint numbers must not overlap between CDC and AUDIO.
Example from the StackOverflow answer:
```c
#define EPNUM_CDC_NOTIF   0x83   // Must differ from AUDIO IN (0x81)
#define EPNUM_CDC_OUT     0x04
#define EPNUM_CDC_IN      0x84
#define EPNUM_AUDIO_IN    0x81
```

**Bandwidth constraint:** USB Full Speed is 12 Mbps. Audio at 48 kHz mono 16-bit uses
~768 kbps (96 bytes/ms). CDC alongside this should be fine for lightweight use.

### Phase 3: Guitar-optimized front-end

1. Design preamp circuit (buffer + gain + bias)
2. Tune ADC bias to 1.65V (half of 3.3V)
3. Adjust gain to use ~80% of ADC range for typical playing
4. Test with DAW

### Phase 4: Advanced (future)

- **USB Audio Class 2.0 (UAC2)**: Higher sample rates (96/192 kHz), but complex descriptors.
  Phil Schatzmann's [Arduino Audio Tools](https://github.com/pschatzmann/arduino-audio-tools)
  has a working UAC2 implementation for RP2040.
- **Bidirectional audio**: Add DAC output for monitoring/playback using PWM or PIO-based DAC.
- **On-Pico DSP**: Simple EQ, compression, or amp simulation using the second Cortex-M0+ core.

## Key Resources

| Resource | URL |
|----------|-----|
| Arm Microphone Library (PDM) | https://github.com/ArmDeveloperEcosystem/microphone-library-for-pico |
| Analog ADC fork (with MIDI) | https://github.com/mdhosale/microphone-library-for-pico-analogpassthrough-midiIO |
| Pico USB Headset (bidirectional) | https://github.com/denisgav/pico-usb-headset |
| TinyUSB audio_test example | https://github.com/hathach/tinyusb/tree/master/examples/device/audio_test |
| Phil Schatzmann UAC2 for RP2040 | https://www.pschatzmann.ch/home/2024/10/13/tinyusb-audio-on-an-rp2040-in-arduino/ |
| Pico SDK stdio_usb drop bug | https://github.com/raspberrypi/pico-sdk/issues/995 |
| CDC + UAC2 composite on Pico | https://stackoverflow.com/questions/74137877 |
| Hackster.io USB Mic tutorial | https://www.hackster.io/sandeep-mistry/create-a-usb-microphone-with-the-raspberry-pi-pico-cc9bd5 |

## Key Learnings for UAC Implementation

1. **DMA is essential for low-latency audio.** Timer ISR per-sample (our current approach)
   adds jitter and CPU overhead. DMA with ADC free-running mode is jitter-free and uses
   zero CPU cycles per sample — only one interrupt per buffer (every 1ms).

2. **Isochronous transfers are fundamentally different from bulk.** CDC/bulk transfers have
   retransmission and variable latency. Isochronous has guaranteed bandwidth and fixed
   1ms scheduling, but errors are not retransmitted (acceptable for audio — a single
   corrupted sample is inaudible).

3. **Endpoint numbers must be unique across all interfaces.** When combining CDC and AUDIO
   in a composite device, endpoint addresses (0x81, 0x82, etc.) must not overlap.

4. **USB Full Speed bandwidth limits audio quality.** At 12 Mbps, you can do mono 48 kHz
   16-bit (~768 kbps) with room for CDC. Stereo 48 kHz 16-bit (~1.5 Mbps) is possible.
   Stereo 96 kHz 24-bit (~4.6 Mbps) pushes the limit and may not leave room for other
   interfaces.

5. **The existing microphone-library-for-pico is the best starting point.** It handles all
   the complex USB descriptors, TinyUSB configuration, and DMA setup. The analog fork shows
   exactly how to adapt it for ADC input instead of PDM digital mics.

## Quick Start (Phase 1)

```sh
# Clone the analog ADC fork
git clone https://github.com/mdhosale/microphone-library-for-pico-analogpassthrough-midiIO.git
cd microphone-library-for-pico-analogpassthrough-midiIO

# Edit tusb_config.h to disable MIDI: #define CFG_TUD_MIDI 0
# Edit main.c to remove MIDI task code

# Build
mkdir build && cd build
cmake .. -DPICO_BOARD=pico -DPICO_TOOLCHAIN_PATH="$HOME/Code/src/toolchains/arm-gnu-toolchain-15.2.rel1-darwin-arm64-arm-none-eabi"
make -j

# Flash
picotool load examples/usb_microphone/usb_microphone.uf2
```

After flashing, the Pico appears in macOS System Settings → Sound → Input as a USB microphone.
Open Ableton, Audacity, or GarageBand — it shows up as an audio input device.
