#include "flower_anim.h"
#include "flower_config.h"
#include "flower_assets.h"
#include "flower_ctl.h"
#include <math.h>

typedef struct
{
    float x;
    float v;
    float omega;
    float zeta;
} flower_spring_t;

static void spring_init(flower_spring_t *s, float freq_hz, float zeta, float x0)
{
    s->x     = x0;
    s->v     = 0.0f;
    s->omega = 2.0f * 3.14159265358979f * freq_hz;
    s->zeta  = zeta;
}

static void spring_step(flower_spring_t *s, float target, float dt)
{
    float a = s->omega * s->omega * (target - s->x) - 2.0f * s->zeta * s->omega * s->v;
    s->v += a * dt;
    s->x += s->v * dt;
}

static flower_spring_t s_open;
static flower_spring_t s_scale;
static flower_spring_t s_glow;
static flower_spring_t s_rot;

static float           s_presence;
static float           s_time;
static float           s_scale_out;
static float           s_flower_scale_out;
static flower_state_t  s_state;
static flower_draw_t   s_draws[FLOWER_ELEMENT_COUNT];

static uint32_t s_glow_index;
static float s_dir_x[FLOWER_ELEMENT_COUNT];
static float s_dir_y[FLOWER_ELEMENT_COUNT];
static float s_phase[FLOWER_ELEMENT_COUNT];
static float s_stagger[FLOWER_ELEMENT_COUNT];

static float clamp01(float v)
{
    if (v < 0.0f) { return 0.0f; }
    if (v > 1.0f) { return 1.0f; }
    return v;
}

static float ease_out(float t)
{
    float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

static float frac(float v)
{
    return v - floorf(v);
}

void flower_anim_init(void)
{
    uint32_t i;
    uint32_t petal_seen  = 0u;
    uint32_t floret_seen = 0u;

    spring_init(&s_open,  FLOWER_SPRING_OPEN_FREQ_HZ,  FLOWER_SPRING_OPEN_DAMPING,  0.0f);
    spring_init(&s_scale, FLOWER_SPRING_SCALE_FREQ_HZ, FLOWER_SPRING_SCALE_DAMPING, 1.0f);
    spring_init(&s_glow,  FLOWER_SPRING_GLOW_FREQ_HZ,  FLOWER_SPRING_GLOW_DAMPING,  0.0f);

    spring_init(&s_rot, (4.744f / FLOWER_WILT_ROT_S) / (2.0f * 3.14159265358979f),
                1.0f, 0.0f);

    s_presence   = 0.0f;
    s_time       = 0.0f;
    s_state      = FLOWER_STATE_DORMANT;
    s_glow_index = 0u;

    for (i = 0u; i < (uint32_t)FLOWER_ELEMENT_COUNT; i++)
    {
        const flower_element_t *el = &flower_elements[i];
        float dx  = (float)el->pivot_fx_q4 / 16.0f - FLOWER_CENTRE_X;
        float dy  = (float)el->pivot_fy_q4 / 16.0f - FLOWER_CENTRE_Y;
        float len = sqrtf(dx * dx + dy * dy);

        if (len > 1.0f)
        {
            s_dir_x[i] = dx / len;
            s_dir_y[i] = dy / len;
        }
        else
        {
            s_dir_x[i] = 0.0f;
            s_dir_y[i] = 0.0f;
        }

        if (el->kind == FLOWER_KIND_PETAL)
        {
            s_phase[i] = frac((float)el->index * FLOWER_PETAL_PHASE_GOLDEN) *
                         FLOWER_PETAL_PHASE_SPREAD;
            s_stagger[i] = (FLOWER_PETAL_COUNT > 1)
                           ? ((float)petal_seen / (float)(FLOWER_PETAL_COUNT - 1u))
                           : 0.0f;
            petal_seen++;
        }
        else if (el->kind == FLOWER_KIND_FLORET)
        {
            s_phase[i] = frac((float)el->index * FLOWER_FLORET_PHASE_GOLDEN) *
                         FLOWER_PETAL_PHASE_SPREAD;
            s_stagger[i] = (FLOWER_FLORET_COUNT > 1)
                           ? ((float)floret_seen / (float)(FLOWER_FLORET_COUNT - 1u))
                           : 0.0f;
            floret_seen++;
        }
        else
        {
            s_phase[i]   = 0.0f;
            s_stagger[i] = 0.0f;
        }

        if (el->kind == FLOWER_KIND_GLOW)
        {
            s_glow_index = i;
        }

        s_draws[i].pose  = 0.0f;
        s_draws[i].alpha = 0.0f;
        s_draws[i].dx    = 0.0f;
        s_draws[i].dy    = 0.0f;
    }
}

/* Integrates presence and the springs, then builds the frame's draw list. */
void flower_anim_tick(float dt_s, uint32_t bloom, float energy)
{
    const int   on = (bloom != 0u);
    float dt = dt_s;
    float remaining;
    float open_target, scale_target, glow_target, rot_target;
    float presence_eased, global_alpha, global_scale;
    float breathe_base;
    uint32_t i;

    if (dt < 0.0f)               { dt = 0.0f; }
    if (dt > FLOWER_DT_MAX_S)    { dt = FLOWER_DT_MAX_S; }

    s_time += dt;

    if (on)
    {
        s_presence += dt / FLOWER_BLOOM_S;
        if (s_presence > 1.0f) { s_presence = 1.0f; }
    }
    else
    {
        s_presence -= dt / FLOWER_WILT_TOTAL_S;
        if (s_presence < 0.0f) { s_presence = 0.0f; }
    }

    if (on)
    {
        s_state = (s_presence >= 1.0f) ? FLOWER_STATE_ALIVE : FLOWER_STATE_BLOOMING;
    }
    else
    {
        s_state = (s_presence > FLOWER_PRESENCE_EPSILON) ? FLOWER_STATE_WILTING
                                                         : FLOWER_STATE_DORMANT;
    }

    if (on)
    {
        float low_accent;

#if FLOWER_AUDIO_BANDS
        low_accent = g_flower_band_low * FLOWER_BAND_LOW_TO_SCALE;
#else
        low_accent = 0.0f;
#endif
        open_target  = energy * FLOWER_ENERGY_TO_OPEN;
        scale_target = 1.0f + energy * FLOWER_ENERGY_TO_SCALE +
                       low_accent * FLOWER_ENERGY_TO_SCALE;
        glow_target  = FLOWER_GLOW_IDLE_ALPHA + energy * FLOWER_ENERGY_TO_GLOW;
        rot_target   = 0.0f;
    }
    else
    {
        open_target  = 0.0f;
        scale_target = FLOWER_WILT_SCALE_TO;
        glow_target  = 0.0f;
        rot_target   = FLOWER_WILT_ROT_DEG;
    }

    remaining = dt;
    while (remaining > 0.0f)
    {
        float h = (remaining > FLOWER_DT_SUBSTEP_S) ? FLOWER_DT_SUBSTEP_S : remaining;
        spring_step(&s_open,  open_target,  h);
        spring_step(&s_scale, scale_target, h);
        spring_step(&s_glow,  glow_target,  h);
        spring_step(&s_rot,   rot_target,   h);
        remaining -= h;
    }

    if (s_state == FLOWER_STATE_DORMANT)
    {
        s_rot.x = 0.0f;
        s_rot.v = 0.0f;
    }

    presence_eased = ease_out(clamp01(s_presence));
    global_alpha   = presence_eased;

    breathe_base = 2.0f * 3.14159265358979f * FLOWER_BREATHE_HZ * s_time;
    {
        const float zoom = FLOWER_BLOOM_SCALE_FROM +
                            (FLOWER_BLOOM_SCALE_TO - FLOWER_BLOOM_SCALE_FROM) *
                            presence_eased;
        const float breathe = 1.0f + FLOWER_BREATHE_SCALE_AMP * sinf(breathe_base);

        global_scale = zoom * breathe *
                       (1.0f + (s_scale.x - 1.0f) * FLOWER_TWIG_SCALE_FOLLOW);
        s_flower_scale_out = zoom * breathe * s_scale.x;
    }

    for (i = 0u; i < (uint32_t)FLOWER_ELEMENT_COUNT; i++)
    {
        const flower_element_t *el = &flower_elements[i];
        flower_draw_t *d = &s_draws[i];
        float open, push, unfurl;

        d->alpha = global_alpha;
        d->pose  = 0.0f;
        d->dx    = 0.0f;
        d->dy    = 0.0f;

        switch (el->kind)
        {
        case FLOWER_KIND_GLOW:
            d->alpha = global_alpha *
                       clamp01(s_glow.x *
                               (1.0f + FLOWER_BREATHE_GLOW_AMP * sinf(breathe_base * 0.7f)));
            break;

        case FLOWER_KIND_PETAL:
        case FLOWER_KIND_FLORET:
        {
            float rate  = (el->kind == FLOWER_KIND_FLORET)
                          ? FLOWER_FLORET_BREATHE_SCALE : 1.0f;
            float wobble = FLOWER_BREATHE_OPEN_AMP *
                           sinf(breathe_base * rate +
                                2.0f * 3.14159265358979f * s_phase[i]);

            {
                const float window = (FLOWER_BLOOM_STAGGER_S < 0.9f * FLOWER_BLOOM_S)
                                     ? (FLOWER_BLOOM_STAGGER_S / FLOWER_BLOOM_S)
                                     : 0.9f;
                float start = window * s_stagger[i];
                unfurl = (s_presence - start) / (1.0f - window);
                unfurl = ease_out(clamp01(unfurl));
            }

            open = unfurl * (FLOWER_REST_OPEN + s_open.x + wobble);

#if FLOWER_AUDIO_BANDS
            if (el->kind == FLOWER_KIND_FLORET)
            {
                open += unfurl * g_flower_band_high * FLOWER_BAND_HIGH_TO_FLORET *
                        FLOWER_BREATHE_OPEN_AMP;
            }
#endif
            open = clamp01(open);

            push = open * ((el->kind == FLOWER_KIND_PETAL)
                           ? FLOWER_PETAL_PUSH_PX : FLOWER_FLORET_PUSH_PX);

            d->pose = open * (float)(FLOWER_POSE_COUNT - 1);
            d->dx   = s_dir_x[i] * push;
            d->dy   = s_dir_y[i] * push;
            break;
        }

        case FLOWER_KIND_LEAF:
            {
                float sway = s_open.x * FLOWER_ENERGY_TO_TWIG_PX * presence_eased;
                d->dx = s_dir_x[i] * sway;
                d->dy = s_dir_y[i] * sway;
            }
            break;

        case FLOWER_KIND_CENTER:
        default:
            break;
        }
    }

    g_flower_presence   = s_presence;
    g_flower_scale      = s_flower_scale_out;
    g_flower_rot_deg    = s_rot.x;
    g_flower_glow_alpha = s_draws[s_glow_index].alpha;
    g_flower_state      = (uint32_t)s_state;

    s_scale_out = global_scale;
}

flower_state_t flower_anim_state(void)
{
    return s_state;
}

const flower_draw_t *flower_anim_draws(void)
{
    return s_draws;
}

float flower_anim_flower_scale(void)
{
    return s_flower_scale_out;
}

float flower_anim_scale(void)
{
    return s_scale_out;
}

float flower_anim_rot_deg(void)
{
    return s_rot.x;
}

int flower_anim_is_dormant(void)
{
    return (s_state == FLOWER_STATE_DORMANT);
}
