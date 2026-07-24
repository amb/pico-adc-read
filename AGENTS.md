# pico-adc-read — PWM Audio Branch (`pwm-audio-no-usb`)

No USB Audio Class. USB CDC kept only for serial + picotool reboot.
Audio output via PWM at 200 kHz carrier on GP19, through an external RC low-pass filter.

## Flash (no unplugging)

```bash
picotool reboot -f -u && sleep 1 && picotool load build/pico_adc_read.uf2 && picotool reboot
```

If that fails, hold BOOTSEL while plugging in, then:
```bash
picotool load build/pico_adc_read.uf2 && picotool reboot
```

## Build

```bash
# Pico 1 (RP2040) — default
mkdir -p build && cd build && cmake .. && make -j$(sysctl -n hw.ncpu)

# Pico 2 (RP2350)
mkdir -p build && cd build && cmake .. -DPICO_BOARD=pico2 && make -j$(sysctl -n hw.ncpu)
```

## Hardware

- **GP19 (pin 25)** — PWM audio output, 200 kHz carrier
- **GP26 (pin 31)** — ADC audio input
- RC low-pass filter on GP19: e.g. 1 kΩ series + 10 nF to GND

## Runtime modes — press & release BOOTSEL to toggle

| Mode | LED | What it does |
|------|-----|--------------|
| ADC passthrough | Slow blink (1 Hz) | Guitar signal GP26 → ADC → PWM out GP19 |
| 440 Hz test tone | Fast blink (4 Hz) | 440 Hz square wave on GP19 |

## USB CDC notes

- `pico_enable_stdio_usb(target 1)` in CMakeLists.txt — that's all that's needed
- **Do not add a custom `tusb_config.h`** — the SDK provides its own with the reset interface
- **Do not add `usb_descriptors.c`** — the SDK's `stdio_usb_descriptors.c` handles it
