# Pico USB Audio Interface — Final Report

## What We Built

A Raspberry Pi Pico (RP2040) that appears as a **native USB microphone** on macOS.
Plug it in and it shows up in System Settings → Sound → Input as "Pico USB Audio."
Works with any DAW (GarageBand, Ableton, Audacity), amp sims (Neural Amp Modeler
Gateway, Tonocracy), or any app that takes a microphone input.

**Performance:**
- Mono, 16-bit, 48 kHz
- Minimum buffer: 64 samples (~1.3 ms latency)
- Reliable and stable (no crashes with extended use)

## Architecture

```
Guitar → ADC0 (GP26) → DMA (ping-pong, 48 kHz) → PCM conversion → tud_audio_write → USB isochronous IN endpoint
                                                                                          ↓
                                                                                    Host: standard USB microphone
```

| Layer | Detail |
|-------|--------|
| ADC sampling | DMA-driven free-running mode, clock divider = 999 for exactly 48 kHz |
| Buffer | Ping-pong: 2 × 48 samples (1 USB frame = 1 ms each) |
| PCM conversion | 12-bit raw → signed 16-bit: `((raw & 0xFFF) - 2048) * 32`, clamped to ±32767 |
| USB class | Audio Class 1.0 (UAC1), mono microphone descriptor |
| USB transport | Isochronous IN endpoint, 96 bytes/ms (48 samples × 2 bytes) |
| TinyUSB config | `CFG_TUD_AUDIO=1`, CDC disabled, `CFG_TUSB_OS=OPT_OS_PICO` |
| Build | CMake: `tinyusb_device` + `tinyusb_board` (both required for RP2040 USB) |

## Development Journey

### Phase 1: CDC Bulk Transport (MVP)
Started with timer-ISR ADC sampling at ~45 kHz, 512-sample buffers sent over CDC
serial. Built a Python/tkinter waveform viewer. Worked for visualization but hit
the Pico SDK's `stdio_usb` data corruption bug (issue #995).

### Phase 2: Direct TinyUSB CDC
Bypassed stdio with `tud_cdc_write()` — fixed data corruption. Good for the
waveform viewer but not usable as an audio device.

### Phase 3: USB Audio Class
Rewrote as a USB Audio Class 1.0 microphone. This was the hard part.

## What Went Wrong (and Why)

### 1. USB didn't enumerate at all (4 attempts, ~90 minutes)

**Symptom:** Firmware compiled and ran (LED heartbeat visible), but macOS never
saw the USB device.

**Attempts:**
- `tinyusb_device` only → no enumeration
- `CFG_TUSB_OS=OPT_OS_NONE` → no enumeration (needs `OPT_OS_PICO` for RP2040)
- `pico_stdio_usb` + custom descriptors → linker errors (duplicate descriptor callbacks)
- `CFG_TUSB_OS=OPT_OS_PICO` + `tinyusb_device` only → no enumeration

**Root cause:** Missing `tinyusb_board` in CMake link libraries. The Arm
Microphone Library example links both `tinyusb_device` AND `tinyusb_board`.
Without `tinyusb_board`, the RP2040 USB PHY/PLL is never initialized.

**Fix:** Add `tinyusb_board` to `target_link_libraries`.

### 2. "Pipe stalled" — device detected but no audio (2 attempts)

**Symptom:** Device appeared as "Pico USB Audio" in IORegistry but not in audio
devices. Kernel log showed repeated `pipe stalled` on endpoint 0.

**Root cause:** macOS queries the audio device for clock source, mute state,
volume range, and connector info. Our control callbacks returned `false`
(= stall) for everything. The host couldn't initialize the audio interface.

**Fix:** Implemented proper `tud_audio_get_req_entity_cb` responding to:
entity 1 (input terminal connector), entity 2 (feature unit: mute/volume),
entity 4 (clock source: sample rate + clock valid).

### 3. Gateway.app freezes, Pico needs physical reconnect (3 attempts)

**Symptom:** After seconds to minutes of use, Gateway/Named would freeze.
The Pico had to be physically unplugged and reconnected.

**Attempted fixes:**
- Silence fill on DMA underrun (helped but didn't solve)
- Move PCM conversion from DMA ISR to USB callback (no change)

**Root cause:** GPIO writes (`gpio_put` for LED) in the main loop alongside
`tud_task()`. Pico SDK issue #663 documents that GPIO register access
conflicts with TinyUSB's USB register access on the same core, causing USB
stack crashes.

**Fix:** Removed all GPIO/LED access from the main loop.

### 4. 32-sample buffer crashes Gateway

**Root cause:** USB Full Speed sends audio in 1 ms frames = 48 samples at
48 kHz. A 32-sample request tries to deliver partial frames, which the
hardware can't do. This is a **physics limitation of USB 1.1**, not a Pico
bug. Even the RME Babyface Pro FS (professional interface) has a 48-sample
minimum.

**Status:** Won't fix. 64 samples (1.3 ms) is the practical minimum.

## What Went Well

- **DMA-driven ADC** was straightforward. The RP2040's ADC + DMA integration
  is excellent for audio — zero CPU overhead per sample.
- **TinyUSB audio descriptors** worked first try once we had the right CMake
  setup. The `TUD_AUDIO_MIC_ONE_CH_DESCRIPTOR` macro generates correct
  descriptors.
- **The Arm Microphone Library** was an invaluable reference. Reading its
  CMakeLists.txt revealed `tinyusb_board` which solved our biggest problem.
- **Kernel log debugging** (`log show --predicate`) was essential for
  diagnosing the pipe stall issue.

## Workflow Improvements

| Problem | Improvement |
|---------|-------------|
| 4+ failed flash attempts with manual BOOTSEL each time | Add `pico_stdio_usb` as a composite CDC+Audio device so `picotool reboot -f -u` works |
| Long feedback loop (code → build → BOOTSEL → flash → test) | Keep a working CDC firmware as a "recovery" image that can be flashed quickly |
| Debugging without serial output | Add UART printf for debug logs (needs USB-UART adapter) or keep CDC interface alongside audio |
| "Does it crash?" test takes minutes | Automate stress test: script that opens audio device and monitors for disconnect |
| Re-discovering the same build issues repeatedly | Document exact CMake incantation: `tinyusb_device tinyusb_board` with `CFG_TUSB_OS=OPT_OS_PICO` |

## Key Lessons

1. **`tinyusb_device` alone is not enough on RP2040.** You need `tinyusb_board`
   for hardware initialization. This is documented nowhere obvious — we found
   it by reading the Microphone Library's CMakeLists.txt.

2. **`CFG_TUSB_OS=OPT_OS_PICO` is required**, not `OPT_OS_NONE`. The Pico SDK's
   TinyUSB integration depends on Pico-specific OS primitives.

3. **USB Audio Class descriptors are fragile.** One wrong endpoint number or
   missing control callback implementation and the host silently rejects the
   device. The kernel log is your only friend.

4. **GPIO and USB don't mix on the same core.** Pico SDK #663: any GPIO
   access in the same loop as `tud_task()` can crash the USB stack. Keep
   your main loop lean — just `tud_task()` and data processing.

5. **Isochronous frames are sacred.** Every 1ms USB frame must have data.
   Missing a frame (not calling `tud_audio_write`) can crash host drivers.
   Always send *something*, even if it's silence.

6. **USB Full Speed has a hard 1ms floor.** You cannot get sub-1ms audio
   latency from a USB 1.1 device. 48 samples at 48 kHz is the indivisible
   quantum. Anyone claiming 32-sample buffer on Full Speed is wrong.

7. **Read working examples' build files.** The Arm Microphone Library's
   CMakeLists.txt answered more questions than any documentation.

## File Map

```
main.c              — DMA ADC init, PCM conversion, TinyUSB audio callbacks
tusb_config.h       — CFG_TUD_AUDIO=1, endpoint sizing, OPT_OS_PICO
usb_descriptors.c   — Device/configuration/string descriptors for UAC1 mono mic
CMakeLists.txt      — tinyusb_device + tinyusb_board, CFG_TUD_CDC_TX_BUFSIZE
REPORT.md           — Phase 1 (CDC bulk) report
USB_AUDIO_RESEARCH.md — Research on USB Audio Class feasibility
python/             — Phase 1 waveform viewer (obsolete, kept for reference)
```

## Flash Command

```sh
# Build
cd ~/Code/src/pico-adc-read
PICO_SDK_PATH=~/Code/src/pico-sdk \
PICO_TOOLCHAIN_PATH=~/Code/src/toolchains/arm-gnu-toolchain-15.2.rel1-darwin-arm64-arm-none-eabi \
cmake -G Ninja -DPICO_BOARD=pico -B build && ninja -C build

# Flash (hold BOOTSEL, plug in USB)
picotool load build/pico_adc_read.uf2 && picotool reboot
```

## Test It

1. Flash the firmware
2. Open System Settings → Sound → Input → select "Pico USB Audio"
3. Open any DAW or amp sim (GarageBand, Audacity, Neural Amp Modeler Gateway, Tonocracy)
4. Set buffer to 64 samples
5. Play guitar
