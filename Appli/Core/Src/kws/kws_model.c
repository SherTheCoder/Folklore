#include "kws/kws_model.h"
#include "kws/kws_kernels.h"
#include "kws/kws_frontend.h"
#include "app/kikkar_config.h"
#include "flower_display.h"
#include <math.h>
#include <string.h>

#define KWS_MAX_LAYERS  32u
#define KWS_MAX_ROWS    16u

extern const uint8_t kws_model_blob[];
extern const uint8_t kws_model_blob_end[];

_Static_assert(sizeof(kws_model_header_t) == 60u,
               "header layout must match tools/export_kws_model.py HEADER_FMT");
_Static_assert(sizeof(kws_layer_t) == 56u,
               "layer layout must match tools/export_kws_model.py LAYER_FMT");

static const kws_model_header_t *s_hdr;
static const kws_layer_t        *s_layers;
static const uint8_t            *s_payload;
static int8_t                   *s_arena;
static int8_t                   *s_state;
static uint32_t                  s_slot_bytes;
static uint32_t                  s_weights;
static uint8_t                   s_head[KWS_MAX_LAYERS];
static uint32_t                  s_last_us;
static uint32_t                  s_steps;

static uint32_t row_bytes(const kws_layer_t *l)
{
    return (uint32_t)l->in_f * l->in_c;
}

static uint32_t layer_weights(const kws_layer_t *l)
{
    uint32_t taps = (uint32_t)l->k_t * l->k_f;

    if (l->op == KWS_OP_CONV)   { return taps * l->in_c * l->out_c; }
    if (l->op == KWS_OP_DWCONV) { return taps * l->in_c; }
    return 0u;
}

/* Rejects a layer that would run past its slot or its state. */
static int layer_fits(const kws_layer_t *l)
{
    uint32_t out_bytes = (uint32_t)l->out_t * l->out_f * l->out_c;
    uint32_t in_bytes  = (uint32_t)l->in_t * row_bytes(l);

    if (l->op < KWS_OP_CONV || l->op > KWS_OP_ADD)          { return 0; }
    if (l->dst >= KWS_SLOTS)                                { return 0; }
    if (l->src >= KWS_SLOTS && l->src != KWS_SLOT_INPUT)    { return 0; }
    if (l->src == l->dst)                                   { return 0; }
    if (out_bytes > s_slot_bytes || in_bytes > s_slot_bytes) { return 0; }
    if ((uint32_t)l->hist_t + l->in_t > KWS_MAX_ROWS)       { return 0; }
    if (l->state_off + (uint32_t)l->hist_t * row_bytes(l) > s_hdr->state_bytes)
    {
        return 0;
    }
    if (l->op == KWS_OP_ADD && l->aux >= KWS_SLOTS)         { return 0; }
    return 1;
}

/* Checks the linked blob and passes its input quantisation to the front end. */
int kws_model_load(void)
{
    const kws_model_header_t *h = (const kws_model_header_t *)kws_model_blob;
    uint16_t i;

    s_hdr = 0;
    if (h->magic != KWS_MODEL_MAGIC)          { return -1; }
    if (h->version != KWS_MODEL_VERSION)      { return -2; }
    if (h->step_frames != KWS_STEP_FRAMES)    { return -3; }
    if (h->mel_bins != KWS_MEL_BINS)          { return -4; }
    if (h->layer_count == 0u || h->layer_count > KWS_MAX_LAYERS) { return -5; }
    if (h->arena_bytes <= h->state_bytes)     { return -6; }

    s_hdr        = h;
    s_layers     = (const kws_layer_t *)(kws_model_blob + sizeof(kws_model_header_t));
    s_payload    = kws_model_blob + h->payload_off;
    s_slot_bytes = (h->arena_bytes - h->state_bytes) / KWS_SLOTS;
    s_weights    = 0u;

    for (i = 0u; i < h->layer_count; i++)
    {
        if (!layer_fits(&s_layers[i]))
        {
            s_hdr = 0;
            return -7;
        }
        s_weights += layer_weights(&s_layers[i]);
    }

    kws_frontend_set_quant(h->in_scale, h->in_zero);
    return 0;
}

uint32_t kws_model_arena_bytes(void)
{
    return (s_hdr != 0) ? s_hdr->arena_bytes : 0u;
}

/* Takes the arena and starts the model from silence. */
void kws_model_set_arena(void *base, uint32_t bytes)
{
    if (s_hdr == 0 || base == 0 || bytes < s_hdr->arena_bytes)
    {
        s_arena = 0;
        s_state = 0;
        return;
    }
    s_arena = (int8_t *)base;
    s_state = s_arena + KWS_SLOTS * s_slot_bytes;
    kws_model_reset();
}

/* Fills every ring with its layer's input zero point, i.e. silence. */
void kws_model_reset(void)
{
    uint16_t i;

    if (s_hdr == 0 || s_state == 0)
    {
        return;
    }
    for (i = 0u; i < s_hdr->layer_count; i++)
    {
        const kws_layer_t *l = &s_layers[i];

        if (l->hist_t != 0u)
        {
            memset(s_state + l->state_off, (int)(int8_t)l->in_zero,
                   (uint32_t)l->hist_t * row_bytes(l));
        }
        s_head[i] = 0u;
    }
}

static int8_t *slot(uint8_t n)
{
    return s_arena + (uint32_t)n * s_slot_bytes;
}

/* Input rows in time order: the ring, oldest first, then this step's rows. */
static void gather_rows(uint16_t i, const kws_layer_t *l, const int8_t *src,
                        const int8_t **rows)
{
    uint32_t rb = row_bytes(l);
    uint32_t r;

    for (r = 0u; r < l->hist_t; r++)
    {
        rows[r] = s_state + l->state_off + ((s_head[i] + r) % l->hist_t) * rb;
    }
    for (r = 0u; r < l->in_t; r++)
    {
        rows[l->hist_t + r] = src + r * rb;
    }
}

/* Overwrites the oldest rows in the ring with this step's input. */
static void remember_rows(uint16_t i, const kws_layer_t *l, const int8_t *src)
{
    uint32_t rb = row_bytes(l);
    uint32_t r;

    if (l->hist_t == 0u)
    {
        return;
    }
    for (r = 0u; r < l->in_t; r++)
    {
        memcpy(s_state + l->state_off + (uint32_t)s_head[i] * rb, src + r * rb, rb);
        s_head[i] = (uint8_t)((s_head[i] + 1u) % l->hist_t);
    }
}

/* Runs one 30 ms step and returns the keyword probability. */
float kws_model_step(const int8_t *frames)
{
    const int8_t *rows[KWS_MAX_ROWS];
    uint32_t t0 = flower_cycles();
    const int8_t *last = 0;
    float logit;
    uint16_t i;

    if (s_hdr == 0 || s_arena == 0)
    {
        return -1.0f;
    }

    for (i = 0u; i < s_hdr->layer_count; i++)
    {
        const kws_layer_t *l = &s_layers[i];
        const int8_t  *src = (l->src == KWS_SLOT_INPUT) ? frames : slot(l->src);
        int8_t        *dst = slot(l->dst);
        const int8_t  *w = (const int8_t *)(s_payload + l->w_off);
        const int32_t *b = (const int32_t *)(const void *)(s_payload + l->b_off);
        const int32_t *mult = (const int32_t *)(const void *)(s_payload + l->q_off);
        const int8_t  *shift;

        switch (l->op)
        {
        case KWS_OP_CONV:
            shift = (const int8_t *)(mult + l->out_c);
            gather_rows(i, l, src, rows);
            kws_conv_s8(rows, dst, w, b, mult, shift, l);
            remember_rows(i, l, src);
            break;

        case KWS_OP_DWCONV:
            shift = (const int8_t *)(mult + l->out_c);
            gather_rows(i, l, src, rows);
            kws_dwconv_s8(rows, dst, w, b, mult, shift, l);
            remember_rows(i, l, src);
            break;

        case KWS_OP_ADD:
            shift = (const int8_t *)(mult + 3);
            kws_add_s8(src, slot(l->aux), dst,
                       (uint32_t)l->out_t * l->out_f * l->out_c, mult, shift, l);
            break;

        default:
            return -1.0f;
        }
        last = dst;
    }

    logit = ((float)last[0] - (float)s_hdr->out_zero) * s_hdr->out_scale;

    s_last_us = flower_us_since(t0);
    s_steps++;
    return 1.0f / (1.0f + expf(-logit));
}

const kws_model_header_t *kws_model_info(void)
{
    return s_hdr;
}

uint32_t kws_model_flash_bytes(void)
{
    return (uint32_t)(kws_model_blob_end - kws_model_blob);
}

uint32_t kws_model_weights(void)
{
    return s_weights;
}

uint32_t kws_model_last_us(void)
{
    return s_last_us;
}

uint32_t kws_model_step_count(void)
{
    return s_steps;
}
