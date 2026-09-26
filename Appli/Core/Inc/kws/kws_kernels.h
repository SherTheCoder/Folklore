#ifndef KWS_KERNELS_H
#define KWS_KERNELS_H

#include <stdint.h>
#include "kws/kws_model.h"

int32_t kws_requantise(int32_t acc, int32_t mult, int32_t shift);
int32_t kws_dot_s8(const int8_t *a, const int8_t *b, uint32_t n);

void kws_conv_s8(const int8_t *const *rows, int8_t *out,
                 const int8_t *w, const int32_t *bias,
                 const int32_t *mult, const int8_t *shift,
                 const kws_layer_t *l);

void kws_dwconv_s8(const int8_t *const *rows, int8_t *out,
                   const int8_t *w, const int32_t *bias,
                   const int32_t *mult, const int8_t *shift,
                   const kws_layer_t *l);

void kws_add_s8(const int8_t *a, const int8_t *b, int8_t *out, uint32_t n,
                const int32_t *mult, const int8_t *shift,
                const kws_layer_t *l);

#endif /* KWS_KERNELS_H */
