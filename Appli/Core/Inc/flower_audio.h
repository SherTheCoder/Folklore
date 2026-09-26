#ifndef FLOWER_AUDIO_H
#define FLOWER_AUDIO_H

#include <stdint.h>
#include "flower_config.h"

#define FLOWER_AUDIO_RING       (FLOWER_AUDIO_HOP * 8u)
#define FLOWER_AUDIO_RING_MASK  (FLOWER_AUDIO_RING - 1u)

extern int16_t           g_flower_audio_ring[FLOWER_AUDIO_RING];
extern volatile uint32_t g_flower_audio_w;

extern volatile uint8_t  g_flower_audio_ready;

void  flower_audio_init(void);

float flower_audio_energy(void);

void  flower_audio_task(void);

static inline void flower_audio_isr_push(int16_t sample)
{
    uint32_t w = g_flower_audio_w;
    g_flower_audio_ring[w & FLOWER_AUDIO_RING_MASK] = sample;
    g_flower_audio_w = w + 1u;
}

void flower_audio_isr_notify(void);

#endif /* FLOWER_AUDIO_H */
