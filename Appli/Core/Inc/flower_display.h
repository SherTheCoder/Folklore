#ifndef FLOWER_DISPLAY_H
#define FLOWER_DISPLAY_H

#include <stdint.h>
#include "flower_assets.h"

#define FLOWER_PANEL_W         800u
#define FLOWER_PANEL_H         480u

#define FLOWER_FB_W            800u
#define FLOWER_FB_H            480u
#define FLOWER_FB_BPP          2u
#define FLOWER_FB_STRIDE       (FLOWER_FB_W * FLOWER_FB_BPP)
#define FLOWER_FB_BYTES        (FLOWER_FB_STRIDE * FLOWER_FB_H)

#define FLOWER_LAYER_X         ((FLOWER_PANEL_W - FLOWER_FB_W) / 2u)
#define FLOWER_LAYER_Y         ((FLOWER_PANEL_H - FLOWER_FB_H) / 2u)

#define FLOWER_FB_ORIGIN_X     ((FLOWER_FRAME_W - (int)FLOWER_FB_W) / 2)
#define FLOWER_FB_ORIGIN_Y     ((FLOWER_FRAME_H - (int)FLOWER_FB_H) / 2)

#define FLOWER_COMPOSE_STRIDE  (FLOWER_COMPOSE_W * FLOWER_FB_BPP)
#define FLOWER_COMPOSE_BYTES   (FLOWER_COMPOSE_STRIDE * FLOWER_COMPOSE_H)

int  flower_display_init(void);

uint32_t flower_display_memcheck(void);

void flower_display_dump(void);

void flower_display_scan_test(void);

void flower_display_clock_dump(void);

void flower_display_pin_dump(void);

void      flower_display_mpu_dump(void);

void      flower_display_membench(void);

uint16_t *flower_display_back_buffer(void);
uint32_t  flower_display_back_index(void);

uint16_t *flower_display_compose_buffer(void);

void flower_display_present(void);

void flower_display_set_visible(int visible);
int  flower_display_is_visible(void);

void flower_display_blank(void);

void flower_display_irq(void);

void     flower_clock_init(void);
uint32_t flower_cycles(void);
uint32_t flower_us_since(uint32_t start_cycles);
float    flower_s_since(uint32_t start_cycles);
uint32_t flower_cycles_per_us(void);

#endif /* FLOWER_DISPLAY_H */
