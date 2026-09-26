#ifndef FLOWER_CTL_H
#define FLOWER_CTL_H

#include <stdint.h>

void flower_ctl_init(void);

extern volatile float    g_flower_energy;
extern volatile float    g_flower_rms_dbfs;
extern volatile float    g_flower_floor_dbfs;
extern volatile float    g_flower_energy_raw;
extern volatile float    g_flower_band_low;
extern volatile float    g_flower_band_mid;
extern volatile float    g_flower_band_high;

extern volatile float    g_flower_presence;
extern volatile float    g_flower_scale;
extern volatile float    g_flower_rot_deg;
extern volatile float    g_flower_glow_alpha;
extern volatile uint32_t g_flower_state;

extern volatile float    g_flower_fps;
extern volatile uint32_t g_flower_frame_us;
extern volatile uint32_t g_flower_compose_us;
extern volatile uint32_t g_flower_warp_us;
extern volatile uint32_t g_flower_audio_us;
extern volatile uint32_t g_flower_hop_count;
extern volatile uint32_t g_flower_hop_overrun;
extern volatile uint32_t g_flower_vblanks;
extern volatile uint32_t g_flower_dma2d_err;
extern volatile uint32_t g_flower_dma2d_code;

#endif /* FLOWER_CTL_H */
