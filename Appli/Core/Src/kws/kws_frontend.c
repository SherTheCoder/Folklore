#include "kws/kws_frontend.h"
#include "kws/kws_fft.h"
#include "kws/kws_preroll.h"
#include "app/kikkar_config.h"
#include "flower_display.h"
#include <math.h>

#define SPEC_BINS   (KWS_FFT_LEN / 2u + 1u)
#define MEL_WEIGHTS 640u
#define STEP_BYTES  (KWS_STEP_FRAMES * KWS_MEL_BINS)

typedef struct
{
    float   window[KWS_FRAME_LEN];
    float   frame[KWS_FFT_LEN];
    float   power[SPEC_BINS];
    float   melw[MEL_WEIGHTS];
    int16_t pcm[KWS_FRAME_LEN];
    int8_t  build[STEP_BYTES];
    int8_t  step[STEP_BYTES];
} frontend_work_t;

static frontend_work_t *s_w;
static uint16_t s_mel_start[KWS_MEL_BINS];
static uint16_t s_mel_len[KWS_MEL_BINS];

static float   s_mean[KWS_MEL_BINS];
static float   s_cms_a;
static uint32_t s_in_step;
static uint32_t s_frames;
static uint32_t s_last_us;
static float   s_qscale = 1.0f;
static int32_t s_qzero;

/* HTK mel scale, the same as the model card's reference front end. */
static float hz_to_mel(float hz)
{
    return 2595.0f * log10f(1.0f + hz / 700.0f);
}

static float mel_to_hz(float mel)
{
    return 700.0f * (powf(10.0f, mel / 2595.0f) - 1.0f);
}

/* Triangular filters, with only the non-zero weights of each band stored. */
static void build_mel(void)
{
    float edge[KWS_MEL_BINS + 2];
    float lo = hz_to_mel(KWS_MEL_LO_HZ);
    float hi = hz_to_mel(KWS_MEL_HI_HZ);
    float bin_hz = (float)KWS_FS_HZ / (float)KWS_FFT_LEN;
    uint32_t i, b, w = 0u;

    for (i = 0u; i < KWS_MEL_BINS + 2u; i++)
    {
        edge[i] = mel_to_hz(lo + (hi - lo) * (float)i / (float)(KWS_MEL_BINS + 1u));
    }

    for (b = 0u; b < KWS_MEL_BINS; b++)
    {
        float f0 = edge[b], f1 = edge[b + 1u], f2 = edge[b + 2u];
        uint32_t k0 = (uint32_t)(f0 / bin_hz);
        uint32_t k2 = (uint32_t)(f2 / bin_hz) + 1u;
        uint32_t k;

        if (k2 > SPEC_BINS - 1u) { k2 = SPEC_BINS - 1u; }
        s_mel_start[b] = (uint16_t)k0;
        s_mel_len[b]   = 0u;

        for (k = k0; k <= k2 && w < MEL_WEIGHTS; k++)
        {
            float f = (float)k * bin_hz;
            float g = 0.0f;

            if (f > f0 && f < f1)       { g = (f - f0) / (f1 - f0); }
            else if (f >= f1 && f < f2) { g = (f2 - f) / (f2 - f1); }

            s_w->melw[w++] = g;
            s_mel_len[b]++;
        }
    }
}

void kws_frontend_init(void)
{
    uint32_t i;

    for (i = 0u; i < KWS_MEL_BINS; i++)
    {
        s_mean[i] = 0.0f;
    }
    s_cms_a = 1.0f - expf(-((float)KWS_FRAME_HOP / (float)KWS_FS_HZ) /
                          ((float)KWS_CMS_TAU_MS / 1000.0f));
    s_w       = 0;
    s_in_step = 0u;
    s_frames  = 0u;
    s_last_us = 0u;
}

uint32_t kws_frontend_workspace_bytes(void)
{
    return (uint32_t)sizeof(frontend_work_t);
}

/* Rebuilds the tables in a new workspace; the running mean is kept. */
void kws_frontend_attach(void *mem)
{
    uint32_t i;

    s_w = (frontend_work_t *)mem;
    for (i = 0u; i < KWS_FRAME_LEN; i++)
    {
        s_w->window[i] = 0.5f - 0.5f * cosf(2.0f * 3.14159265358979f * (float)i /
                                            (float)(KWS_FRAME_LEN - 1u));
    }
    build_mel();
    s_in_step = 0u;
}

void kws_frontend_set_quant(float scale, int32_t zero_point)
{
    s_qscale = (scale > 0.0f) ? scale : 1.0f;
    s_qzero  = zero_point;
}

static void stage_frame(void)
{
    uint32_t i;

    for (i = 0u; i < KWS_FRAME_LEN; i++)
    {
        s_w->frame[i] = (float)s_w->pcm[i] * (1.0f / 32768.0f) * s_w->window[i];
    }
    for (i = KWS_FRAME_LEN; i < KWS_FFT_LEN; i++)
    {
        s_w->frame[i] = 0.0f;
    }
}

/* Log mel energies, floored at -80 dB. */
static void mel_log(float *out)
{
    uint32_t b, k, w = 0u;

    for (b = 0u; b < KWS_MEL_BINS; b++)
    {
        float acc = 0.0f;
        uint32_t k0 = s_mel_start[b];

        for (k = 0u; k < s_mel_len[b]; k++)
        {
            acc += s_w->power[k0 + k] * s_w->melw[w++];
        }
        out[b] = logf((acc > KWS_LOG_FLOOR) ? acc : KWS_LOG_FLOOR);
    }
}

/* Plain average for the first 0.5 s, then a running mean with a 3 s time
   constant.  Returns 0 while warming up. */
static int normalise(float *mel)
{
    uint32_t b;

    if (s_frames < KWS_CMS_WARMUP_FRAMES)
    {
        for (b = 0u; b < KWS_MEL_BINS; b++)
        {
            s_mean[b] += (mel[b] - s_mean[b]) / (float)(s_frames + 1u);
        }
        return 0;
    }
    for (b = 0u; b < KWS_MEL_BINS; b++)
    {
        s_mean[b] += s_cms_a * (mel[b] - s_mean[b]);
        mel[b] -= s_mean[b];
    }
    return 1;
}

/* Returns 1 when this frame completes a step, 0 if not, -1 if the audio is
   no longer in the pre-roll. */
int kws_frontend_add_frame(uint32_t pos)
{
    float mel[KWS_MEL_BINS];
    uint32_t t0 = flower_cycles();
    int8_t *row;
    uint32_t b, i;
    int ready;

    if (kws_preroll_read(pos, s_w->pcm, KWS_FRAME_LEN) != KWS_FRAME_LEN)
    {
        return -1;
    }

    stage_frame();
    kws_fft_power_spectrum(s_w->frame, s_w->power);
    mel_log(mel);
    ready = normalise(mel);
    s_frames++;

    if (!ready)
    {
        s_last_us = flower_us_since(t0);
        return 0;
    }

    row = &s_w->build[s_in_step * KWS_MEL_BINS];
    for (b = 0u; b < KWS_MEL_BINS; b++)
    {
        int32_t q = (int32_t)lrintf(mel[b] / s_qscale) + s_qzero;
        if (q >  127) { q =  127; }
        if (q < -128) { q = -128; }
        row[b] = (int8_t)q;
    }

    s_last_us = flower_us_since(t0);
    if (++s_in_step < KWS_STEP_FRAMES)
    {
        return 0;
    }
    for (i = 0u; i < STEP_BYTES; i++)
    {
        s_w->step[i] = s_w->build[i];
    }
    s_in_step = 0u;
    return 1;
}

const int8_t *kws_frontend_step(void)
{
    return s_w->step;
}

uint32_t kws_frontend_frame_count(void)
{
    return s_frames;
}

uint32_t kws_frontend_last_us(void)
{
    return s_last_us;
}
