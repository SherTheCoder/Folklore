#ifndef FLOWER_RENDER_H
#define FLOWER_RENDER_H

#include <stdint.h>
#include "flower_assets.h"

#define FLOWER_CENTRE_X   (FLOWER_FRAME_W / 2.0f)
#define FLOWER_CENTRE_Y   (FLOWER_FRAME_H / 2.0f)

#define FLOWER_KMASK_STATIC  ((1u << FLOWER_KIND_GLOW) | (1u << FLOWER_KIND_LEAF))
#define FLOWER_KMASK_FLOWER  ((1u << FLOWER_KIND_CENTER) | \
                              (1u << FLOWER_KIND_PETAL)  | \
                              (1u << FLOWER_KIND_FLORET))
#define FLOWER_KMASK_ALL     (FLOWER_KMASK_STATIC | FLOWER_KMASK_FLOWER)

typedef struct
{
    float pose;
    float alpha;
    float dx;
    float dy;
} flower_draw_t;

int  flower_render_init(void);

void flower_render_frame(const flower_draw_t *draws,
                         float scale, float flower_scale, float rot_deg,
                         uint32_t *compose_us, uint32_t *warp_us);

void flower_render_reset(void);

#endif /* FLOWER_RENDER_H */
