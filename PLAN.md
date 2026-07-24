# NAM-on-PWM — Plan

> **Note**: This is the original design document. The as-built implementation
> differs in some details (ISR-based I/O instead of DMA, no USB audio).
> See [AGENTS.md](AGENTS.md) for the current architecture and
> [README.md](README.md) for a quick overview.

Replace the simple ADC→PWM passthrough with a **Neural Amp Modeler (NAM) A2-Lite**
running on the Pico 2 (RP2350), based on
[oyama/pico-neural-amp-modeler-demo](https://github.com/oyama/pico-neural-amp-modeler-demo).

ADC on GP26 → dual-core NAM → PWM on GP19 at 200 kHz carrier.  No USB audio.

## What is NAM v2 (A2)?

**Neural Amp Modeler Architecture 2 (A2)** is the next generation of open-source amp
modeling, built by [TONE3000](https://tone3000.com) in partnership with NAM creator
Steve Atkinson.  It's the most accurate amp modeling technology ever built, and its
smaller A2-Lite variant is efficient enough to run on a **$3 ARM Cortex-M7 chip**.

A2 is a WaveNet-derived feedforward neural network.  A single ".nam" download contains
two "slim points" of the same model:

| Variant | Channels | Use case | Runs on RP2350? |
|---------|----------|----------|-----------------|
| **A2-Full** | 8 | Studio/DAW — maximum accuracy | ❌ No |
| **A2-Lite** | 3 | Embedded/pedals — still beats all commercial modelers | ✅ Yes |

A2-Lite is what makes this project possible.  On a Daisy Seed (Cortex-M7 @ 480 MHz)
it clears real time easily; on the RP2350's dual Cortex-M33 @ 300 MHz, oyama's
optimized dual-core pipeline runs it at 73% CPU (4,533 cycles/sample at 48 kHz).

A2 is **fully open-source** (MIT license).  NAM v1 ("A1") still works but is
deprecated — all new captures on TONE3000 are A2.

## Where the amp simulations come from: TONE3000.com

[TONE3000](https://tone3000.com) (formerly ToneHunt) is the community platform for
NAM captures and impulse responses.  Thousands of creators upload hyper-realistic
neural models of amplifiers, pedals, outboard gear, and full signal chains.

How it works:
1. A creator captures their real amp/pedal/rig using the NAM training pipeline
   (sweep signals → neural network training → .nam model file)
2. The .nam file is uploaded to TONE3000 with metadata (amp model, settings, cab,
   mics, description, tags)
3. Anyone can browse and download these captures for free
4. The .nam file can be loaded into the NAM plugin (DAW), compatible hardware
   pedals, or — in our case — compiled into firmware for a standalone pedal

**Licensing**: Most captures use the T3K license — free for personal use, but the
.nam file itself cannot be redistributed.  This means our firmware repository can
include the weights (as a compiled-in array) but users should download their own
.nam files from TONE3000 for different tones.  See [TONE3000's policy](https://www.tone3000.com/legal/tone-sharing-policy).

## Default model: 1964 Marshall JTM-45 Block Logo Radiospares OT

**URL**: https://www.tone3000.com/tones/1964-marshall-jtm-45-block-logo-radiospares-ot-crunch-a2-76417

**Creator**: [amalgamaudio](https://www.amalgamcaptures.com) (professional capture artist)

| Property | Value |
|----------|-------|
| Amp | 1964 Marshall JTM-45 Block Logo with Radiospares Output Transformer |
| Cab | 1966 Marshall 4×12 Straight Pinstripe, Celestion G12 Alnico (vintage) |
| Mics | Royer R121, Beyerdynamic M160, Neumann U87 |
| Style | Old-school blues, early rock (Clapton, Hendrix, Townshend) |
| Settings | Presence 4, Bass 0, Middle 6, Treble 7, Vol I 7, Vol II 0 |
| A2-Full ESR | 0.0042 (excellent) |
| A2-Lite ESR | 0.0106 (still very good) |
| Downloads | 5,852 |
| License | T3K |

> "The holy grail Marshall JTM-45 amp bar none — an early 1964 Block Logo with the
> Radiospares Output Transformer.  It has a singing sustain, musical compression and
> a bold smoothness that none of the later Drake OT equipped amps can match."
> — amalgamaudio

This is an ideal default: a celebrated vintage amp with a crunch character that
covers blues, classic rock, and edge-of-breakup tones.  At A2-Lite size it's ~1,871
float weights (~7.5 KB), easily fitting the RP2350's flash.

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

## Where to get amp simulation files

**Download any A2 capture from [tone3000.com/search](https://tone3000.com/search).**
Filter by "A2" under Technical to see only A2-compatible captures.

### What's inside a .nam file

A `.nam` file is a JSON container (a "SlimmableContainer") that packs two
sub-models of the same amp at different channel widths:

| Sub-model | Channels | Weights | Fits RP2350? |
|-----------|----------|---------|--------------|
| **A2-Lite** | 3 | ~1,871 (~7.5 KB) | ✅ Yes — real time at 300 MHz |
| **A2-Full** | 8 | ~5,000 (~20 KB) | ❌ No — too large for real time |

Only A2-Lite runs in real time on the RP2350.  A2-Full and the older WaveNet v1
("standard NAM") models are rejected at build time by `is_a2_shape()` with a clear
error — no silent fallback to a wrong or slow model.

**Naming note**: The NAM engine source calls A2-Lite "nano" and A2-Full "standard"
(this predates TONE3000's naming).  In the code you'll see `is_a2_shape` and
`a2_fast` referring to A2-Lite (the 3-channel nano shape).

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
# Download any A2 capture from tone3000.com (e.g., browse tone3000.com/search)
# The default model (already in the repo) is:
#   1964 Marshall JTM-45 Block Logo Radiospares OT - CRUNCH

# Place your .nam file in the project root:
cp ~/Downloads/"My Sick 5150.nam" ./tone.nam

# Point the build at it — either by editing CMakeLists.txt:
#   nam_set_model(tone.nam)
# or on the command line:
cmake -S . -B build -DNAM_MODEL=tone.nam -DPICO_BOARD=pico2 -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
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
