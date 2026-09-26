#ifndef KWS_RESAMPLE_H
#define KWS_RESAMPLE_H

#include <stdint.h>

void     kws_resample_init(void);

uint32_t kws_resample_pump(void);

uint32_t kws_resample_output_count(void);

#endif /* KWS_RESAMPLE_H */
