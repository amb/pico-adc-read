# Pico ADC Reader — Project Report

## Overview

Real-time audio ADC sampling on a Raspberry Pi Pico (RP2040) with a Python desktop waveform viewer. The Pico samples ADC0 (GPIO 26, pin 31) at ~45.45 kHz, double-buffers 512 samples, and streams them to a macOS host over USB. The Python app (`adc_viewer.py`) renders the waveform in real time.

## Architecture

```
Guitar → ADC0 (GP26) → RP2040 ADC → Timer ISR → Double Buffer → tud_cdc_write → USB CDC → Python/tkinter
```

### Pico Firmware (`main.c`)

| Component | Detail |
|-----------|--------|
| Sampling rate | ~45.45 kHz (22 µs timer period — closest to 44.1 kHz with 1 µs hardware timer resolution) |
| Buffer size | 512 samples × 2 buffers = double-buffered |
| Frame format | `0xAA 0x55` sync + 2-byte counter (big-endian uint16) + 1024 bytes samples (512 × uint16 BE) |
| Frame size | 1028 bytes |
| Frame rate | ~88.8 Hz (45.45 kHz / 512) |
| Data rate | ~91.3 KB/s |
| USB transport | TinyUSB CDC directly (`tud_cdc_write`), not stdio |
| TX buffer | 2048 bytes (`CFG_TUD_CDC_TX_BUFSIZE`) |

**Key design decisions:**

- **Timer ISR** (`add_repeating_timer_us`) reads ADC and fills one buffer. When full, atomically hands it off to the main loop and switches to the other buffer. Overruns silently overwrite.
- **Main loop** builds the entire frame in a static buffer, writes it via `tud_cdc_write` in chunks (handles any buffer size), calls `tud_task()` between chunks to keep the USB stack alive.
- **`stdio_init_all()`** is kept only for `picotool reboot -f -u` support (remote BOOTSEL).

### Python Viewer (`adc_viewer.py`)

| Component | Detail |
|-----------|--------|
| GUI | tkinter (Canvas, 1024×400 default) |
| Serial | pyserial (115200 baud, but USB CDC ignores baud) |
| Frame parsing | Finds `0xAA 0x55` sync, extracts counter + 512 samples |
| Validation | Rejects frames with any sample > 4095 (corruption detection) |
| Display | Green waveform on dark background, dashed center line, stats bar |
| Update rate | 30 ms (~33 fps) |

**Stats bar shows:** average/min/max voltage, dropped frame count, bad frame count, buffer count.

## Challenges & Solutions

### 1. Silent data corruption in `stdio_usb` (MAJOR)

**Symptom:** 179/201 frames had corrupted data. Valid ADC values at frame start, garbage mid-frame at random offsets. High peaks showed "odd spikes."

**Diagnosis:** The corruption pattern showed valid samples then bytes with upper nibble set to `0xF`, indicating missing/dropped bytes. Hex analysis confirmed data was being silently truncated.

**Root cause:** [Pico SDK issue #995](https://github.com/raspberrypi/pico-sdk/issues/995) — `stdio_usb_out_chars()` has a timeout (`PICO_STDIO_USB_STDOUT_TIMEOUT_US`, default 500ms). When the internal USB TX buffer fills and the host hasn't drained it within the timeout, the function **breaks out of its write loop, silently dropping the remaining bytes.** The `fwrite` return value incorrectly reports success.

**Fix:** Bypassed stdio entirely. Used TinyUSB's `tud_cdc_write()` directly with a chunked write loop, plus `tud_task()` calls between chunks. Increased `CFG_TUD_CDC_TX_BUFSIZE` from 256 to 2048 to reduce chunking. This gives us proper blocking behavior — data is never silently dropped.

### 2. `tud_cdc_write_available()` buffer size mismatch

**Symptom:** After switching to direct `tud_cdc_write`, no data was being sent at all — the LED never blinked and read returned 0 bytes.

**Root cause:** The initial code checked `tud_cdc_write_available() >= FRAME_SIZE` before writing. The default TinyUSB CDC TX buffer is 256 bytes, but our frame is 1028 bytes, so the check always failed and every frame was silently discarded.

**Fix:** Changed to a chunked write loop that writes as much as fits per iteration, plus increased the buffer to 2048 bytes so the typical case is a single write.

### 3. Homebrew Python lacks tkinter

**Symptom:** `import tkinter` failed with `ModuleNotFoundError: No module named '_tkinter'`.

**Fix:** `brew install python-tk` adds tkinter support to Homebrew Python. Alternatively, used `uv` for dependency management with the system Python.

### 4. Short circuit incident

**Symptom:** Pico stopped responding on USB after a short circuit on the breadboard.

**Resolution:** The Pico survived. After reconnecting the USB cable, `picotool reboot -f -u` successfully rebooted it to BOOTSEL mode. The RP2040 appears to have survived whatever short occurred.

## Project structure

```
~/Code/src/pico-adc-read/
├── main.c              # Pico firmware
├── CMakeLists.txt      # CMake build (includes CFG_TUD_CDC_TX_BUFSIZE=2048)
├── pico_sdk_import.cmake
├── build/              # Build outputs (pico_adc_read.elf, .uf2, .bin)
└── python/
    ├── pyproject.toml  # uv project config
    ├── adc_viewer.py   # Python waveform viewer
    ├── .venv/          # uv virtual environment
    └── .python-version
```

## Key Learnings

1. **Never trust `fwrite` to stdout on Pico for binary data.** The Pico SDK's `stdio_usb` has a known timeout-based truncation bug. Use `tud_cdc_write()` directly for any non-trivial binary transfer.

2. **Validate your protocol.** The frame counter + 12-bit range check caught corruption immediately that would otherwise appear as mysterious "odd spikes" in the waveform. Binary protocols need validation.

3. **USB CDC baud rate is meaningless.** The 115200 setting doesn't constrain throughput — USB full-speed can handle several Mbps. The actual bottleneck was the SDK's buffer management.

4. **Call `tud_task()` regularly.** When using TinyUSB directly (not through stdio), the USB stack needs periodic `tud_task()` calls to process host requests and maintain the connection.

5. **Chunked writes are robust.** Instead of requiring the entire frame to fit in one USB buffer write, a loop that writes whatever fits is resilient to buffer size changes.

## Flash commands

```sh
# Full cycle (requires USB stdio in current firmware)
picotool reboot -f -u && ninja -C build && picotool load build/pico_adc_read.uf2 && picotool reboot
```

## Run the viewer

```sh
cd ~/Code/src/pico-adc-read/python && uv run adc_viewer.py
```
