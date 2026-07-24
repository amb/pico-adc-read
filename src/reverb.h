#pragma once
// Simple mono Schroeder reverberator: 4 parallel comb filters → 2 series allpasses.
// Processes blocks of samples in-place (mixes wet into dry).

#ifdef __cplusplus
extern "C" {
#endif

void schroeder_init(void);
void schroeder_set_mix(float wet);  // 0.0 = dry, 1.0 = full wet (default ~0.3)
void schroeder_process(float* buf, int n);  // in-place, dry + wet mix

#ifdef __cplusplus
}
#endif
