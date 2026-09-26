#ifndef KWS_PIPELINE_H
#define KWS_PIPELINE_H

#include <stdint.h>

typedef enum
{
    KWS_EVENT_NONE = 0,
    KWS_EVENT_SPEECH,
    KWS_EVENT_KEYWORD
} kws_event_t;

int      kws_pipeline_init(void);
uint32_t kws_pipeline_workspace_bytes(void);
int      kws_pipeline_attach(void *base, uint32_t bytes);

void     kws_pipeline_task(void);
void     kws_pipeline_set_inference(int on);
kws_event_t kws_pipeline_take_event(void);

uint32_t kws_pipeline_steps(void);
uint32_t kws_pipeline_last_us(void);
int      kws_pipeline_is_ready(void);

#endif /* KWS_PIPELINE_H */
