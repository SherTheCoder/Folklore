#ifndef KWS_FRONTEND_H
#define KWS_FRONTEND_H

#include <stdint.h>

void     kws_frontend_init(void);
uint32_t kws_frontend_workspace_bytes(void);
void     kws_frontend_attach(void *mem);
void     kws_frontend_set_quant(float scale, int32_t zero_point);

int      kws_frontend_add_frame(uint32_t pos);
const int8_t *kws_frontend_step(void);

uint32_t kws_frontend_frame_count(void);
uint32_t kws_frontend_last_us(void);

#endif /* KWS_FRONTEND_H */
