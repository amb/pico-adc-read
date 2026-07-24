#pragma once
// Simple first-order DC-blocking high-pass filter (~10 Hz at 48 kHz).
// y[n] = coeff * (y[n-1] + x[n] - x[n-1])
// Removes DC bias and subsonic noise in one multiply-add per sample.

#ifdef __cplusplus
extern "C" {
#endif

void dc_blocker_init(void);
void dc_blocker_process(float* buf, int n);   // in-place

#ifdef __cplusplus
}
#endif
