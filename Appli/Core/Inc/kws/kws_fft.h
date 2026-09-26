#ifndef KWS_FFT_H
#define KWS_FFT_H

#include <stdint.h>

uint32_t kws_fft_workspace_bytes(void);
void     kws_fft_attach(void *mem);
void     kws_fft_power_spectrum(const float *in, float *power);

#endif /* KWS_FFT_H */
