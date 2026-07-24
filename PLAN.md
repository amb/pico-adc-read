# NAM-on-PWM — Plan

Replace the simple ADC→PWM passthrough with a **Neural Amp Modeler (NAM) A2-Lite**
running on the Pico 2 (RP2350), based on
[oyama/pico-neural-amp-modeler-demo](https://github.com/oyama/pico-neural-amp-modeler-demo).

ADC on GP26 → dual-core NAM → PWM on GP19 at 200 kHz carrier.  No USB audio.

## Source of truth

oyama's repo runs a production NAM A2-Lite tone (3-channel WaveNet, 23 dilated-conv
layers, ~1,871 parameters, ~7.5 KB) split across both M33 cores at 300 MHz, hitting
4,533 cycles/sample (73% CPU at 48 kHz).  It pipes audio over USB Audio Class 2.0
(UAC2).  We keep the engine and dual-core pipeline; we replace USB audio with ADC+PWM.

## Architecture diff

| Layer | oyama (loopback) | Us (standalone) |
|-------|------------------|-----------------|
| Audio in | USB isochronous IN (host → device) | ADC free-running on GP26, DMA to 48-sample blocks |
| Audio out | USB isochronous OUT (device → host) | Timer ISR at ~48 kHz pushes samples to PWM duty on GP19 |
| Block clock | USB SOF (1 ms = 48 frames) | Timer at 1 ms (48 samples) |
| NAM engine | a2_fast dual-core layer pipeline (KSPLIT=14) | **Same** |
| Model | Embedded at compile time via nam2c | **Same** |
| Clock | 300 MHz @ 1.20 V | **Same** |
| BOOTSEL | Toggle NAM vs passthrough | **Same** |
| LED | On = NAM, Off = passthrough | **Same** |
| USB | UAC2 audio device | CDC only (serial + picotool reboot) |

## Build-system changes

1. **Switch to C++** — the NAM engine is C++/Eigen.  `main.c` → `main.cpp`.
2. **Add submodules** — NeuralAmpModelerCore fork, Eigen, nlohmann/json.
3. **nam2c host tool** — compiles the .nam model into a C array for embedding.
4. **CMakeLists.txt** rewritten to match oyama's build (Release, exceptions, whole-archive).
5. **PICO_BOARD=pico2** and PICO_SDK_PATH required.

## Audio data flow

```
                    ┌─────────────────────────────┐
GP26 ──→ ADC ──→ DMA ──→ in_buf[48] ──→  NAM    │
                    │    (double-buffered)   engine │
                    │    @ 1 ms blocks       (both  │
                    │                        cores) │
                    │                             │
GP19 ←── PWM ←── timer ←── out_buf[48] ←─── NAM    │
         200 kHz  48 kHz   (ring buffer)    output  │
         carrier  ISR                             │
                    └─────────────────────────────┘
```

### Input path
- ADC on GP26, free-running at 48 kHz (ADC clock divider for exact rate).
- DMA collects 48 samples into a double-buffered input block.
- DMA-completion ISR swaps buffers and signals the NAM pipeline.

### Output path
- NAM produces a 48-sample output block per 1 ms tick.
- Output samples are placed in a ring buffer.
- A repeating-timer ISR at ~48 kHz (21 µs) pops one sample per tick
  and updates PWM duty cycle on GP19.
- PWM carrier: 200 kHz (wrap=624), same as current branch.

### Block processing
- One 48-sample block arrives every 1 ms (USB SOF equivalent).
- Core1 runs front layers [0, KSPLIT); Core0 runs back layers [KSPLIT, 23) + head.
- Double-buffered handoff between cores via SIO FIFO.
- Bit-exact vs single-core.

## Files to create / modify

| File | Action | Notes |
|------|--------|-------|
| `main.cpp` | Rewrite | Replaces `main.c`. Init ADC, PWM, NAM, dual-core, 1 ms block timer |
| `nam_fx.cpp` | Adapt from oyama | Remove USB audio hooks; add ADC-in / PWM-out buffer interface |
| `nam_fx.h` | New | Interface for NAM init + per-block process |
| `CMakeLists.txt` | Rewrite | C++ build, submodules, nam2c embedding, Release, 300 MHz |
| `tusb_config.h` | Minimal | CDC only (no UAC2), for picotool reboot |
| `AGENTS.md` | Update | Flash/build workflow for new branch |

## Dependencies (submodules)

| Repo | Role |
|------|------|
| oyama/NeuralAmpModelerCore (fork) | NAM engine with a2_fast layer-partition API |
| libeigen/eigen | Linear algebra (header-only) |
| nlohmann/json | .nam parsing (host-side nam2c only) |

## Steps (implementation order)

1. **Add submodules** + rewrite CMakeLists.txt to build oyama's loopback first
   as a smoke test, then add our ADC/PWM changes.
2. **Port nam_fx.cpp** — remove USB audio layer, add buffer-based I/O interface.
3. **Add ADC+DMA input** — 48-sample double-buffer at 48 kHz.
4. **Add PWM+timer output** — ring buffer consumed at 48 kHz.
5. **Wire dual-core** — core1 launch, SIO FIFO handoff, 1 ms block tick.
6. **BOOTSEL toggle** + LED — NAM vs clean passthrough.
7. **Embed model** — nam2c toolchain, example tone.
8. **Benchmark** — verify cycle budget at 300 MHz.

## Open questions

- **ADC quality**: The RP2350 ADC is 12-bit SAR. Guitar signal needs preamp/bias.
  Is the onboard ADC good enough, or do we need an external codec?
- **PWM resolution**: At 200 kHz carrier, PWM wrap=624 gives ~9.3 bits.
  Acceptable for guitar monitoring, but higher quality would need a codec.
- **Latency**: 1 ms block + NAM pipeline (~1 ms) + PWM output buffering = ~2-3 ms.
- **Model storage**: Today the model is compiled in. Flash has room for one model
  (~7.5 KB weights + overhead). Runtime model loading is future work.
