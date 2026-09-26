#ifndef KWS_PREROLL_H
#define KWS_PREROLL_H

#include <stdint.h>

void     kws_preroll_init(void);

void     kws_preroll_write(int16_t sample);

uint32_t kws_preroll_head(void);

uint32_t kws_preroll_tail(void);

uint32_t kws_preroll_read(uint32_t from, int16_t *dst, uint32_t n);

uint32_t kws_preroll_rewind(uint32_t from, uint32_t samples);

#endif /* KWS_PREROLL_H */
