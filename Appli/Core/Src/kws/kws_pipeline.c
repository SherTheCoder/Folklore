#include "kws/kws_pipeline.h"
#include "kws/kws_resample.h"
#include "kws/kws_preroll.h"
#include "kws/kws_frontend.h"
#include "kws/kws_fft.h"
#include "kws/kws_model.h"
#include "kws/kws_detector.h"
#include "opt/opt_gate.h"
#include "opt/opt_sleep.h"
#include "app/kikkar_config.h"
#include "flower_display.h"
#include <string.h>

#define STEP_BYTES      (KWS_STEP_FRAMES * KWS_MEL_BINS)
#define STEP_SAMPLES    (KWS_STEP_FRAMES * KWS_FRAME_HOP)
#define LOOKBACK_BYTES  (KWS_GATE_LOOKBACK_STEPS * STEP_BYTES)
#define ALIGN16(n)      (((n) + 15u) & ~15u)

static int8_t (*s_lookback)[STEP_BYTES];
static uint32_t s_lb_next;
static uint32_t s_lb_count;

static volatile kws_event_t s_event;
static uint32_t s_frame_pos;
static uint32_t s_steps;
static uint32_t s_last_us;
static int      s_ready;
static int      s_attached;
static int      s_inference = 1;
static int      s_gate_was_open;

int kws_pipeline_init(void)
{
    int rc;

    kws_preroll_init();
    kws_resample_init();
    kws_frontend_init();

    rc = kws_model_load();
    if (rc != 0)
    {
        s_ready = 0;
        return rc;
    }

    kws_detector_init();
    opt_gate_init();

    s_event         = KWS_EVENT_NONE;
    s_frame_pos     = kws_preroll_head();
    s_steps         = 0u;
    s_last_us       = 0u;
    s_attached      = 0;
    s_inference     = 1;
    s_ready         = 1;
    return 0;
}

/* FFT, front end, look-back and model, each on a 16-byte boundary. */
uint32_t kws_pipeline_workspace_bytes(void)
{
    return ALIGN16(kws_fft_workspace_bytes()) +
           ALIGN16(kws_frontend_workspace_bytes()) +
           ALIGN16(LOOKBACK_BYTES) +
           kws_model_arena_bytes();
}

/* Called at boot and each time the sender gives the block back. */
int kws_pipeline_attach(void *base, uint32_t bytes)
{
    uint8_t *p = (uint8_t *)base;

    s_attached = 0;
    if (!s_ready || base == 0 || bytes < kws_pipeline_workspace_bytes())
    {
        return -1;
    }

    kws_fft_attach(p);
    p += ALIGN16(kws_fft_workspace_bytes());
    kws_frontend_attach(p);
    p += ALIGN16(kws_frontend_workspace_bytes());
    s_lookback = (int8_t (*)[STEP_BYTES])(void *)p;
    p += ALIGN16(LOOKBACK_BYTES);
    kws_model_set_arena(p, kws_model_arena_bytes());

    s_frame_pos     = kws_preroll_head();
    s_lb_next       = 0u;
    s_lb_count      = 0u;
    s_gate_was_open = 0;
    s_attached      = 1;
    return 0;
}

/* A pending keyword is never overwritten by a speech event. */
static void post(kws_event_t e)
{
    if (s_event != KWS_EVENT_KEYWORD)
    {
        s_event = e;
    }
}

static void keep_step(const int8_t *step)
{
    memcpy(s_lookback[s_lb_next], step, STEP_BYTES);
    s_lb_next = (s_lb_next + 1u) % KWS_GATE_LOOKBACK_STEPS;
    if (s_lb_count < KWS_GATE_LOOKBACK_STEPS)
    {
        s_lb_count++;
    }
}

static void run_step(const int8_t *step, uint32_t newest)
{
    float p = kws_model_step(step);

    if (p >= 0.0f && kws_detector_update(p, newest))
    {
        post(KWS_EVENT_KEYWORD);
    }
}

/* When the gate opens, restart the model and replay the kept steps so the
   first syllable is not lost. */
static void catch_up(uint32_t newest)
{
    uint32_t i;
    uint32_t first = (s_lb_next + KWS_GATE_LOOKBACK_STEPS - s_lb_count) %
                     KWS_GATE_LOOKBACK_STEPS;

    kws_model_reset();
    kws_detector_reset();

    for (i = 0u; i < s_lb_count; i++)
    {
        uint32_t age = s_lb_count - 1u - i;
        run_step(s_lookback[(first + i) % KWS_GATE_LOOKBACK_STEPS],
                 newest - age * STEP_SAMPLES);
    }
}

void kws_pipeline_set_inference(int on)
{
    s_inference = on;
}

/* Called from PendSV after the flower's own DSP. */
void kws_pipeline_task(void)
{
    uint32_t t0;

    if (!s_ready)
    {
        return;
    }
    t0 = flower_cycles();

    kws_resample_pump();

    while (s_inference && s_attached &&
           (s_frame_pos + KWS_FRAME_LEN) <= kws_preroll_head())
    {
        int rc = kws_frontend_add_frame(s_frame_pos);
        uint32_t newest = s_frame_pos + KWS_FRAME_LEN;

        if (rc < 0)
        {
            s_frame_pos = kws_preroll_head();
            break;
        }
        s_frame_pos += KWS_FRAME_HOP;
        if (rc == 0)
        {
            continue;
        }

        s_steps++;
        keep_step(kws_frontend_step());

        if (!opt_gate_is_open())
        {
            s_gate_was_open = 0;
            continue;
        }
        if (!s_gate_was_open)
        {
            s_gate_was_open = 1;
            post(KWS_EVENT_SPEECH);
            catch_up(newest);
            continue;
        }
        run_step(kws_frontend_step(), newest);
    }

    s_last_us = flower_us_since(t0);
    opt_sleep_note_busy(s_last_us * flower_cycles_per_us());
}

kws_event_t kws_pipeline_take_event(void)
{
    kws_event_t e = s_event;

    s_event = KWS_EVENT_NONE;
    return e;
}

uint32_t kws_pipeline_steps(void)
{
    return s_steps;
}

uint32_t kws_pipeline_last_us(void)
{
    return s_last_us;
}

int kws_pipeline_is_ready(void)
{
    return s_ready;
}
