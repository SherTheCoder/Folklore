#ifndef KWS_DETECTOR_H
#define KWS_DETECTOR_H

#include <stdint.h>

void kws_detector_init(void);
void kws_detector_reset(void);
int  kws_detector_update(float p, uint32_t audio_pos);
void kws_detector_rearm(void);

float    kws_detector_score(void);
uint32_t kws_detector_trigger_pos(void);
uint32_t kws_detector_trigger_cycles(void);
uint32_t kws_detector_fire_count(void);

#endif /* KWS_DETECTOR_H */
