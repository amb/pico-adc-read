// Schroeder reverberator: 4 parallel comb filters → 2 series allpasses.
// Classic design from "Natural Sounding Artificial Reverberation" (Schroeder, 1962).
//
// Delay lengths are mutually prime at 48 kHz to minimize resonant peaks.
// Total buffer memory: ~27 KB.

#include "reverb.h"
#include <cstring>
#include <algorithm>

//----------------------------------------------------------------------------
// Delay-line lengths (samples at 48 kHz)
//----------------------------------------------------------------------------

// Comb filters (~30-34 ms each, prime numbers)
static const int COMB_LEN[4] = { 1557, 1617, 1491, 1421 };

// Allpass filters (~5-9 ms each)
static const int AP_LEN[2] = { 225, 441 };

//----------------------------------------------------------------------------
// Parameters
//----------------------------------------------------------------------------

static float g_comb_fb[4]     = { 0.82f, 0.80f, 0.84f, 0.78f };  // comb feedback
static float g_ap_coeff[2]     = { 0.60f, 0.55f };                // allpass coefficient
static float g_wet             = 0.30f;                            // wet/dry mix

//----------------------------------------------------------------------------
// Delay-line buffers
//----------------------------------------------------------------------------

static float g_comb_buf[4][1617];   // max comb length = 1617
static int   g_comb_pos[4] = {0};

static float g_ap_buf[2][441];      // max AP length = 441
static int   g_ap_pos[2] = {0};

//----------------------------------------------------------------------------
// Init
//----------------------------------------------------------------------------

extern "C" void schroeder_init(void) {
    for (int i = 0; i < 4; i++) {
        std::memset(g_comb_buf[i], 0, sizeof(g_comb_buf[i]));
        g_comb_pos[i] = 0;
    }
    for (int i = 0; i < 2; i++) {
        std::memset(g_ap_buf[i], 0, sizeof(g_ap_buf[i]));
        g_ap_pos[i] = 0;
    }
}

extern "C" void schroeder_set_mix(float wet) {
    if (wet < 0.0f) wet = 0.0f;
    if (wet > 1.0f) wet = 1.0f;
    g_wet = wet;
}

//----------------------------------------------------------------------------
// Process
//----------------------------------------------------------------------------

static inline float comb(float in, int idx) {
    const int len = COMB_LEN[idx];
    int& pos = g_comb_pos[idx];
    float* buf = g_comb_buf[idx];

    float delayed = buf[pos];
    // Low-pass filter in feedback path (reduces metallic ringing)
    // Simple one-pole: out = delayed + g_comb_fb * (0.5*delayed + 0.5*prev_fb)
    static float lp[4] = {0};
    float fb_in = (delayed + lp[idx]) * 0.5f;
    lp[idx] = fb_in;
    buf[pos] = in + g_comb_fb[idx] * fb_in;

    pos++;
    if (pos >= len) pos = 0;
    return delayed;
}

static inline float allpass(float in, int idx) {
    const int len = AP_LEN[idx];
    int& pos = g_ap_pos[idx];
    float* buf = g_ap_buf[idx];
    const float g = g_ap_coeff[idx];

    float delayed = buf[pos];          // read state[n-K]
    float y = delayed - g * in;        // y = state[n-K] - g*x[n]
    buf[pos] = in + g * y;             // state[n] = x[n] + g*y[n]

    pos++;
    if (pos >= len) pos = 0;
    return y;
}

extern "C" void schroeder_process(float* buf, int n) {
    for (int i = 0; i < n; i++) {
        float dry = buf[i];

        // 4 parallel comb filters
        float wet = 0.0f;
        for (int c = 0; c < 4; c++)
            wet += comb(dry, c);

        // 2 series allpasses
        wet = allpass(wet, 0);
        wet = allpass(wet, 1);

        // Mix
        buf[i] = dry + g_wet * wet;
    }
}
