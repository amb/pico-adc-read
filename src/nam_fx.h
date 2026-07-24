#pragma once
// NAM A2-Lite effect interface (C-linkage).
#ifdef __cplusplus
extern "C" {
#endif

// One-time init: create models, launch core1 front-half worker.
void nam_fx_init(void);

// Process one block of mono float samples in-place (or distinct in/out).
// frames must equal NFRAMES (48 at 48 kHz = 1 ms block).
void nam_fx_process(float* out, const float* in, int frames);

// Re-prime the pipeline (call on each bypass→active transition).
void nam_fx_reset(void);

// Human-readable effect name.
const char* nam_fx_name(void);

#ifdef __cplusplus
}
#endif
