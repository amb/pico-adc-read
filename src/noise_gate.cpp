// DC-blocking first-order high-pass filter.
// y[n] = a * (y[n-1] + x[n] - x[n-1])
// a = e^(-2π * fc / fs) ≈ 0.9987 for fc=10Hz, fs=48kHz

#include "noise_gate.h"

static const float COEFF = 0.9987f;   // ~10 Hz cutoff at 48 kHz
static float g_prev_x = 0.0f;
static float g_prev_y = 0.0f;

extern "C" void dc_blocker_init(void) {
    g_prev_x = 0.0f;
    g_prev_y = 0.0f;
}

extern "C" void dc_blocker_process(float* buf, int n) {
    for (int i = 0; i < n; i++) {
        float x = buf[i];
        float y = COEFF * (g_prev_y + x - g_prev_x);
        g_prev_x = x;
        g_prev_y = y;
        buf[i] = y;
    }
}
