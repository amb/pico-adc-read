# pico-adc-read — PWM Audio Branch (`pwm-audio-no-usb`)

No USB Audio Class. USB CDC kept only for serial + picotool reboot.

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
mkdir -p build && cd build && cmake .. && make -j$(sysctl -n hw.ncpu)
```

## Hardware

- **GP19 (pin 25)** — PWM audio output, 200 kHz carrier
- **GP26 (pin 31)** — ADC audio input
- RC low-pass filter on GP19: e.g. 1 kΩ series + 10 nF to GND

## USB CDC notes

- `pico_enable_stdio_usb(target 1)` in CMakeLists.txt — that's all that's needed
- **Do not add a custom `tusb_config.h`** — the SDK provides its own with the reset interface
- **Do not add `usb_descriptors.c`** — the SDK's `stdio_usb_descriptors.c` handles it
