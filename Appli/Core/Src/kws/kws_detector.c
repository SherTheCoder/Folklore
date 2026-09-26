#include "kws/kws_detector.h"
#include "app/kikkar_config.h"
#include "flower_display.h"
#include "stm32n6xx_hal.h"

static float    s_hist[KWS_SMOOTH_STEPS];
static uint32_t s_n;
static float    s_score;
static uint32_t s_trigger_pos;
static uint32_t s_trigger_cyc;
static uint32_t s_fired_tick;
static uint32_t s_fire_count;
static int      s_locked;

void kws_detector_init(void)
{
    kws_detector_reset();
    s_trigger_pos = 0u;
    s_trigger_cyc = 0u;
    s_fired_tick  = 0u;
    s_fire_count  = 0u;
    s_locked      = 0;
}

void kws_detector_reset(void)
{
    uint32_t i;

    for (i = 0u; i < KWS_SMOOTH_STEPS; i++)
    {
        s_hist[i] = 0.0f;
    }
    s_n     = 0u;
    s_score = 0.0f;
}

static float smooth(float p)
{
    float acc = 0.0f;
    uint32_t i;

    s_hist[s_n % KWS_SMOOTH_STEPS] = p;
    s_n++;

    for (i = 0u; i < KWS_SMOOTH_STEPS; i++)
    {
        acc += s_hist[i];
    }
    return acc / (float)KWS_SMOOTH_STEPS;
}

/* Returns 1 when the smoothed score reaches the threshold outside the
   refractory period. */
int kws_detector_update(float p, uint32_t audio_pos)
{
    uint32_t now = HAL_GetTick();

    s_score = smooth(p);

    if (s_locked)
    {
        if ((now - s_fired_tick) < KWS_REFRACTORY_MS)
        {
            return 0;
        }
        s_locked = 0;
    }

    if (s_score < KWS_THRESHOLD)
    {
        return 0;
    }

    s_trigger_pos = audio_pos;
    s_trigger_cyc = flower_cycles();
    s_fired_tick  = now;
    s_locked      = 1;
    s_fire_count++;
    return 1;
}

void kws_detector_rearm(void)
{
    s_locked = 0;
}

float kws_detector_score(void)
{
    return s_score;
}

uint32_t kws_detector_trigger_pos(void)
{
    return s_trigger_pos;
}

uint32_t kws_detector_trigger_cycles(void)
{
    return s_trigger_cyc;
}

uint32_t kws_detector_fire_count(void)
{
    return s_fire_count;
}
