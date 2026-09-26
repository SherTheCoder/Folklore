#include "main.h"
#include "flower_audio.h"
#include "flower_ctl.h"
#include "flower_display.h"
#include "arm_math.h"
#include <math.h>

int16_t           g_flower_audio_ring[FLOWER_AUDIO_RING];
volatile uint32_t g_flower_audio_w;
volatile uint8_t  g_flower_audio_ready;

static uint32_t   s_read;
static float32_t  s_hop_f32[FLOWER_AUDIO_HOP];
static float32_t  s_band_f32[FLOWER_AUDIO_HOP];

static float s_env;
static float s_floor_dbfs;
static int   s_floor_seeded;
static float s_hop_dt;
static float s_a_attack, s_a_release;
static float s_a_floor_fall, s_a_floor_rise;
static float s_knee_norm;

#if FLOWER_AUDIO_BANDS
#define BAND_STAGES  1u
static arm_biquad_cascade_df2T_instance_f32 s_bq[3];
static float32_t s_bq_coeff[3][5 * BAND_STAGES];
static float32_t s_bq_state[3][2 * BAND_STAGES];
static float     s_band_env[3];
#endif

void flower_audio_isr_notify(void)
{
    if (g_flower_audio_ready && ((g_flower_audio_w - s_read) >= FLOWER_AUDIO_HOP))
    {
        SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    }
}

static float one_pole(float tau_s, float dt_s)
{
    if (tau_s <= 0.0f)
    {
        return 1.0f;
    }
    return 1.0f - expf(-dt_s / tau_s);
}

#if FLOWER_AUDIO_BANDS
/* RBJ biquad in CMSIS's layout, with a1 and a2 already negated. */
static void biquad_design(float32_t *c, float f0_hz, float q, int kind)
{
    float w0    = 2.0f * 3.14159265358979f * f0_hz / FLOWER_AUDIO_FS_HZ;
    float cw    = cosf(w0);
    float sw    = sinf(w0);
    float alpha = sw / (2.0f * q);
    float a0    = 1.0f + alpha;
    float b0, b1, b2;

    if (kind < 0)
    {
        b0 = (1.0f - cw) * 0.5f;
        b1 = (1.0f - cw);
        b2 = b0;
    }
    else if (kind > 0)
    {
        b0 = (1.0f + cw) * 0.5f;
        b1 = -(1.0f + cw);
        b2 = b0;
    }
    else
    {
        b0 = alpha;
        b1 = 0.0f;
        b2 = -alpha;
    }

    c[0] = b0 / a0;
    c[1] = b1 / a0;
    c[2] = b2 / a0;
    c[3] = -(-2.0f * cw) / a0;
    c[4] = -(1.0f - alpha) / a0;
}
#endif

void flower_audio_init(void)
{
    s_read       = g_flower_audio_w & ~(uint32_t)(FLOWER_AUDIO_HOP - 1u);
    s_env        = 0.0f;
    s_floor_dbfs   = FLOWER_FLOOR_INIT_DBFS;
    s_floor_seeded = 0;

    s_hop_dt       = (float)FLOWER_AUDIO_HOP / FLOWER_AUDIO_FS_HZ;
    s_a_attack     = one_pole(FLOWER_ENV_ATTACK_S,  s_hop_dt);
    s_a_release    = one_pole(FLOWER_ENV_RELEASE_S, s_hop_dt);
    s_a_floor_fall = one_pole(FLOWER_FLOOR_FALL_S,  s_hop_dt);
    s_a_floor_rise = one_pole(FLOWER_FLOOR_RISE_S,  s_hop_dt);

    s_knee_norm = 1.0f / (1.0f - 0.5f * FLOWER_GATE_KNEE);

#if FLOWER_AUDIO_BANDS
    {
        float mid_f0 = sqrtf(FLOWER_BAND_LOW_HZ * FLOWER_BAND_HIGH_HZ);
        float mid_q  = mid_f0 / (FLOWER_BAND_HIGH_HZ - FLOWER_BAND_LOW_HZ);
        int i;

        biquad_design(s_bq_coeff[0], FLOWER_BAND_LOW_HZ,  0.707f, -1);
        biquad_design(s_bq_coeff[1], mid_f0,              mid_q,   0);
        biquad_design(s_bq_coeff[2], FLOWER_BAND_HIGH_HZ, 0.707f,  1);

        for (i = 0; i < 3; i++)
        {
            arm_biquad_cascade_df2T_init_f32(&s_bq[i], BAND_STAGES,
                                             s_bq_coeff[i], s_bq_state[i]);
            s_band_env[i] = 0.0f;
        }
    }
#endif

    HAL_NVIC_SetPriority(PendSV_IRQn, 5, 0);

    g_flower_floor_dbfs = s_floor_dbfs;
    g_flower_audio_ready = 1u;
}

static float to_dbfs(float rms)
{
    if (rms < 1e-7f)
    {
        rms = 1e-7f;
    }
    return 20.0f * log10f(rms);
}

/* Quadratic knee near the floor, so room tone fades instead of flickering. */
static float soft_knee(float x)
{
    float y;

    if (x <= 0.0f)
    {
        return 0.0f;
    }
    if (x < FLOWER_GATE_KNEE)
    {
        y = 0.5f * x * x / FLOWER_GATE_KNEE;
    }
    else
    {
        y = x - 0.5f * FLOWER_GATE_KNEE;
    }
    y *= s_knee_norm;
    return (y > 1.0f) ? 1.0f : y;
}

static float envelope(float prev, float target)
{
    float a = (target > prev) ? s_a_attack : s_a_release;
    return prev + (target - prev) * a;
}

void flower_audio_task(void)
{
    uint32_t t0;

    if (!g_flower_audio_ready)
    {
        return;
    }
    t0 = flower_cycles();

    while ((g_flower_audio_w - s_read) >= FLOWER_AUDIO_HOP)
    {
        uint32_t base = s_read & FLOWER_AUDIO_RING_MASK;
        float32_t rms;
        float dbfs, above, raw;

        if ((g_flower_audio_w - s_read) > (FLOWER_AUDIO_RING - FLOWER_AUDIO_HOP))
        {
            s_read = (g_flower_audio_w & ~(uint32_t)(FLOWER_AUDIO_HOP - 1u))
                     - FLOWER_AUDIO_HOP;
            base   = s_read & FLOWER_AUDIO_RING_MASK;
            g_flower_hop_overrun++;
        }

        arm_q15_to_float(&g_flower_audio_ring[base], s_hop_f32, FLOWER_AUDIO_HOP);
        s_read += FLOWER_AUDIO_HOP;

        arm_rms_f32(s_hop_f32, FLOWER_AUDIO_HOP, &rms);
        dbfs = to_dbfs(rms);

        if (!s_floor_seeded)
        {
            s_floor_dbfs   = dbfs;
            s_floor_seeded = 1;
        }
        else if (dbfs < s_floor_dbfs)
        {
            s_floor_dbfs += (dbfs - s_floor_dbfs) * s_a_floor_fall;
        }
        else
        {
            s_floor_dbfs += (dbfs - s_floor_dbfs) * s_a_floor_rise;
        }
        if (s_floor_dbfs < FLOWER_FLOOR_MIN_DBFS) { s_floor_dbfs = FLOWER_FLOOR_MIN_DBFS; }
        if (s_floor_dbfs > FLOWER_FLOOR_MAX_DBFS) { s_floor_dbfs = FLOWER_FLOOR_MAX_DBFS; }

        above = (dbfs - s_floor_dbfs - FLOWER_GATE_MARGIN_DB) / FLOWER_GATE_SPAN_DB;
        raw   = soft_knee(above);
        s_env = envelope(s_env, raw);

#if FLOWER_AUDIO_BANDS
        {
            int i;
            for (i = 0; i < 3; i++)
            {
                float32_t brms;
                float bdb, babove;

                arm_biquad_cascade_df2T_f32(&s_bq[i], s_hop_f32, s_band_f32,
                                            FLOWER_AUDIO_HOP);
                arm_rms_f32(s_band_f32, FLOWER_AUDIO_HOP, &brms);
                bdb    = to_dbfs(brms);
                babove = (bdb - s_floor_dbfs - FLOWER_GATE_MARGIN_DB) /
                         FLOWER_GATE_SPAN_DB;
                s_band_env[i] = envelope(s_band_env[i], soft_knee(babove));
            }
            g_flower_band_low  = s_band_env[0];
            g_flower_band_mid  = s_band_env[1];
            g_flower_band_high = s_band_env[2];
        }
#endif

        g_flower_rms_dbfs   = dbfs;
        g_flower_floor_dbfs = s_floor_dbfs;
        g_flower_energy_raw = raw;
        g_flower_energy     = s_env;
        g_flower_hop_count++;
    }

    g_flower_audio_us = flower_us_since(t0);
}

float flower_audio_energy(void)
{
    return g_flower_energy;
}
