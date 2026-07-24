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
7. **Embed model** — nam2c host tool builds from `tone.nam`, see [model embedding](#how-the-model-gets-embedded-nam2c-pipeline) above.
8. **Benchmark** — verify cycle budget at 300 MHz.

## Where to get the amp simulation file

**Download from [tone3000.com](https://tone3000.com/).**  TONE3000 is Steve Atkinson's
site for NAM v2 (A2 architecture) captures.  Pick any A2 capture and download the
`.nam` file.

### What's inside a .nam file

A `.nam` file is a JSON container (technically a "SlimmableContainer") that packs
two sub-models of the same amp at different sizes:

| Sub-model | Channels | Weights | Fits RP2350? |
|-----------|----------|---------|--------------|
| **A2-Lite** | 3 | ~1,871 (~7.5 KB) | ✅ Yes — real time at 300 MHz |
| **A2-Full** | 8 | ~5,000 (~20 KB) | ❌ No — too large for real time |

Only A2-Lite runs in real time.  A2-Full and the older WaveNet v1 ("standard NAM")
are rejected at build time with a clear error.

**Naming note**: The NAM engine source calls A2-Lite "nano" and A2-Full "standard"
(this predates TONE3000's A2-Lite / A2-Full naming).  In the code you'll see
`is_a2_shape` and `a2_fast` referring to A2-Lite (the 3-channel nano shape).

## How the model gets embedded (nam2c pipeline)

oyama's `tools/nam2c.cpp` is a **host-side C++ tool** (compiled with the system
compiler, not arm-none-eabi-gcc) that runs at firmware-build time.  It does the
full pipeline in one validated step:

```
┌───────────────────────────────────────────────────┐
│ 1. Load .nam file (nlohmann/json)                  │
│ 2. If SlimmableContainer, pull out A2-Lite (3-ch)  │
│    sub-model; otherwise accept a bare WaveNet      │
│ 3. Validate with the ENGINE'S OWN is_a2_shape() —  │
│    same detector that runs on-device — so build    │
│    acceptance can never drift from the runtime     │
│ 4. Emit nam_model.c: weight array + metadata       │
│    constants (channels, layers, receptive field,   │
│    model name, architecture)                       │
└───────────────────────────────────────────────────┘
```

The generated `nam_model.c` is a single C file containing:
- `const float nam_model_weights[]` — the weight array (~1,871 floats)
- `const unsigned nam_model_weights_len` — array length
- `const char nam_model_name[]` — tone name from metadata
- `const int nam_model_channels / layers / kernel_min / kernel_max`
- `const int nam_model_receptive_field` — in samples (~132 ms)

This file is compiled into the firmware.  **No filesystem, no SD card, no runtime
parsing** — the model is a static const array in flash.

### Swapping the model

To use a different amp capture:

```bash
# Download any A2 capture from tone3000.com
cp "My Sick 5150.nam" tone.nam

# Edit CMakeLists.txt: change nam_set_model(example.nam) to nam_set_model(tone.nam)
# Or pass it on the cmake command line:
cmake -S . -B build -DNAM_MODEL=tone.nam -DPICO_BOARD=pico2 -DCMAKE_BUILD_TYPE=Release
cmake --build build --target pico_nam_pwm -j
```

`nam2c` validates the model at build time.  If you accidentally use an A2-Full
or WaveNet v1 model, `is_a2_shape()` rejects it and the build fails with a clear
message — no silent fallback to a wrong tone.

### Output gain

`NAM_OUTPUT_GAIN` in `src/nam_fx.cpp` scales the output level.  The output clamps,
giving amp-like clipping when pushed.  Default is unity; adjust to match your
pickup/ADC levels.

## Open questions

- **ADC quality**: The RP2350 ADC is 12-bit SAR. Guitar signal needs preamp/bias.
  Is the onboard ADC good enough, or do we need an external codec?
- **PWM resolution**: At 200 kHz carrier, PWM wrap=624 gives ~9.3 bits.
  Acceptable for guitar monitoring, but higher quality would need a codec.
- **Latency**: 1 ms block + NAM pipeline (~1 ms) + PWM output buffering = ~2-3 ms.
- **Model storage**: Flash has room for one model (~7.5 KB weights + overhead).
  Runtime model loading (USB mass-storage drag-and-drop) is planned upstream
  but out of scope for our initial integration.
