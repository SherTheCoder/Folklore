#include "kws/kws_resample.h"
#include "kws/kws_preroll.h"
#include "app/kikkar_config.h"
#include "flower_audio.h"

#define FIR_MASK  (KWS_FIR_TAPS - 1u)

static const int16_t s_fir[KWS_FIR_TAPS] = {
       -72,    -62,     44,    263,    370,    -15,
      -905,  -1500,   -549,   2510,   6653,   9648,
      9646,   6653,   2510,   -549,  -1500,   -905,
       -15,    370,    263,     44,    -62,    -72,
};

static int16_t  s_hist[KWS_FIR_TAPS];
static uint32_t s_head;
static uint32_t s_phase;
static uint32_t s_read;
static uint32_t s_produced;

void kws_resample_init(void)
{
    uint32_t i;

    for (i = 0u; i < KWS_FIR_TAPS; i++)
    {
        s_hist[i] = 0;
    }
    s_head     = 0u;
    s_phase    = 0u;
    s_produced = 0u;
    s_read     = g_flower_audio_w & ~(uint32_t)(FLOWER_AUDIO_HOP - 1u);
}

/* The taps are only evaluated on every third sample, when an output is due. */
static inline int kws_resample_push(int16_t in, int16_t *out)
{
    int32_t  acc = 0;
    uint32_t i;
    uint32_t p;

    s_head = (s_head - 1u) & FIR_MASK;
    s_hist[s_head] = in;

    if (++s_phase < (uint32_t)KWS_DECIM)
    {
        return 0;
    }
    s_phase = 0u;

    p = s_head;
    for (i = 0u; i < KWS_FIR_TAPS; i++)
    {
        acc += (int32_t)s_hist[p] * (int32_t)s_fir[i];
        p = (p + 1u) & FIR_MASK;
    }

    acc >>= 15;
    if (acc >  32767) { acc =  32767; }
    if (acc < -32768) { acc = -32768; }
    *out = (int16_t)acc;
    return 1;
}

uint32_t kws_resample_pump(void)
{
    uint32_t made = 0u;

    while ((g_flower_audio_w - s_read) >= FLOWER_AUDIO_HOP)
    {
        uint32_t base;
        uint32_t i;

        if ((g_flower_audio_w - s_read) > (FLOWER_AUDIO_RING - FLOWER_AUDIO_HOP))
        {
            s_read = (g_flower_audio_w & ~(uint32_t)(FLOWER_AUDIO_HOP - 1u))
                     - FLOWER_AUDIO_HOP;
        }
        base = s_read & FLOWER_AUDIO_RING_MASK;

        for (i = 0u; i < FLOWER_AUDIO_HOP; i++)
        {
            int16_t out;
            if (kws_resample_push(g_flower_audio_ring[base + i], &out))
            {
                kws_preroll_write(out);
                made++;
            }
        }
        s_read += FLOWER_AUDIO_HOP;
    }

    s_produced += made;
    return made;
}

uint32_t kws_resample_output_count(void)
{
    return s_produced;
}
