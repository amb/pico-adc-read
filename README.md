# NAM-on-PWM — Neural Amp Modeler on a $5 Pico 2

Real-time guitar amp modeling on a Raspberry Pi Pico 2 (RP2350).
ADC input → NAM A2-Lite → effects → PWM output. No USB audio, no external DSP.

## Quick start

```bash
# Clone with submodules
git clone --recurse-submodules https://github.com/amb/pico-adc-read.git
cd pico-adc-read
git checkout nam-pwm

# Build (requires Pico SDK + ARM GNU toolchain — see AGENTS.md)
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(sysctl -n hw.ncpu)

# Flash
picotool load build/pico_adc_read.uf2 && picotool reboot
```

## Hardware

- **Pico 2 (RP2350)** — required (dual Cortex-M33 at 300 MHz)
- **GP26** (pin 31) — guitar input via LM358 preamp, AC-coupled, ~1.65V bias
- **GP19** (pin 25) — PWM audio out @ 200 kHz → RC low-pass (1kΩ + 10nF)
- External RC filter on GP19 converts PWM to audio

## What it does

Press BOOTSEL to cycle three modes:

| Mode | LED | Signal path |
|------|-----|-------------|
| NAM | Solid | ADC → DC blocker → NAM A2-Lite → reverb → PWM |
| Passthrough | Slow blink | ADC → PWM (clean) |
| 440 Hz | Fast blink | Test tone → PWM |

## Signal chain

```
GP26 → LM358 → ADC (12-bit, 48 kHz)
  → DC blocker (10 Hz HPF)
  → NAM A2-Lite (dual-core a2_fast, 300 MHz, 73% CPU)
  → Schroeder reverb (4 combs + 2 allpasses)
  → PWM @ 200 kHz carrier
```

## Swapping amp models

Download any A2 capture from [tone3000.com](https://tone3000.com), then:

```bash
cmake .. -DNAM_MODEL=my_amp.nam -DCMAKE_BUILD_TYPE=Release
make -j
picotool load build/pico_adc_read.uf2 && picotool reboot
```

Only A2-Lite (3-channel) models work — 1,871 weights, ~7.5 KB each.
`nam2c` validates at build time and rejects incompatible models.

Current model: **EVH 5150 III 50W Stealth Red + Merciless Drive**

## Docs

- **[AGENTS.md](AGENTS.md)** — full dev guide: build, flash, source map, adding effects, CPU budget
- **[PLAN.md](PLAN.md)** — original design document and architecture rationale

## Credits

- NAM engine: [oyama/NeuralAmpModelerCore](https://github.com/oyama/NeuralAmpModelerCore) (MIT)
- NAM architecture: Steve Atkinson / [TONE3000](https://tone3000.com)
- Amp captures: community creators on TONE3000 (T3K license)
