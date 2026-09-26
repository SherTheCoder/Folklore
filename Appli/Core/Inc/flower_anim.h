#ifndef FLOWER_ANIM_H
#define FLOWER_ANIM_H

#include <stdint.h>
#include "flower_render.h"

typedef enum
{
    FLOWER_STATE_DORMANT  = 0,
    FLOWER_STATE_BLOOMING = 1,
    FLOWER_STATE_ALIVE    = 2,
    FLOWER_STATE_WILTING  = 3
} flower_state_t;

void flower_anim_init(void);

void flower_anim_tick(float dt_s, uint32_t bloom, float energy);

flower_state_t       flower_anim_state(void);
const flower_draw_t *flower_anim_draws(void);
float                flower_anim_scale(void);
float                flower_anim_flower_scale(void);
float                flower_anim_rot_deg(void);

int                  flower_anim_is_dormant(void);

#endif /* FLOWER_ANIM_H */
