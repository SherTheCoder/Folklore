#include "flower_ctl.h"

#define FLOWER_CTL __attribute__((section(".noncacheable"), used, aligned(32)))

volatile float    g_flower_energy      FLOWER_CTL = 0.0f;
volatile float    g_flower_rms_dbfs    FLOWER_CTL = -120.0f;
volatile float    g_flower_floor_dbfs  FLOWER_CTL = -120.0f;
volatile float    g_flower_energy_raw  FLOWER_CTL = 0.0f;
volatile float    g_flower_band_low    FLOWER_CTL = 0.0f;
volatile float    g_flower_band_mid    FLOWER_CTL = 0.0f;
volatile float    g_flower_band_high   FLOWER_CTL = 0.0f;

volatile float    g_flower_presence    FLOWER_CTL = 0.0f;
volatile float    g_flower_scale       FLOWER_CTL = 0.0f;
volatile float    g_flower_rot_deg     FLOWER_CTL = 0.0f;
volatile float    g_flower_glow_alpha  FLOWER_CTL = 0.0f;
volatile uint32_t g_flower_state       FLOWER_CTL = 0u;

volatile float    g_flower_fps         FLOWER_CTL = 0.0f;
volatile uint32_t g_flower_frame_us    FLOWER_CTL = 0u;
volatile uint32_t g_flower_compose_us  FLOWER_CTL = 0u;
volatile uint32_t g_flower_warp_us     FLOWER_CTL = 0u;
volatile uint32_t g_flower_audio_us    FLOWER_CTL = 0u;
volatile uint32_t g_flower_hop_count   FLOWER_CTL = 0u;
volatile uint32_t g_flower_hop_overrun FLOWER_CTL = 0u;
volatile uint32_t g_flower_vblanks     FLOWER_CTL = 0u;
volatile uint32_t g_flower_dma2d_err   FLOWER_CTL = 0u;
volatile uint32_t g_flower_dma2d_code  FLOWER_CTL = 0u;

/* Cleared by hand: the ELF download does not zero this section. */
void flower_ctl_init(void)
{
    g_flower_energy      = 0.0f;
    g_flower_rms_dbfs    = -120.0f;
    g_flower_floor_dbfs  = -120.0f;
    g_flower_energy_raw  = 0.0f;
    g_flower_band_low    = 0.0f;
    g_flower_band_mid    = 0.0f;
    g_flower_band_high   = 0.0f;

    g_flower_presence    = 0.0f;
    g_flower_scale       = 0.0f;
    g_flower_rot_deg     = 0.0f;
    g_flower_glow_alpha  = 0.0f;
    g_flower_state       = 0u;

    g_flower_fps         = 0.0f;
    g_flower_frame_us    = 0u;
    g_flower_compose_us  = 0u;
    g_flower_warp_us     = 0u;
    g_flower_audio_us    = 0u;
    g_flower_hop_count   = 0u;
    g_flower_hop_overrun = 0u;
    g_flower_vblanks     = 0u;
    g_flower_dma2d_err   = 0u;
    g_flower_dma2d_code  = 0u;
}
