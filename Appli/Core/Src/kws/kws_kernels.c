#include "kws/kws_kernels.h"
#include "app/kikkar_config.h"

#if OPT_MVE_KERNELS && defined(__ARM_FEATURE_MVE)
#include <arm_mve.h>
#endif

#define ADD_LEFT_SHIFT  20

/* TFLite-style requantisation: rounding doubling high multiply, then a
   rounding right shift. */
int32_t kws_requantise(int32_t acc, int32_t mult, int32_t shift)
{
    int64_t wide = (int64_t)acc * (int64_t)mult;
    int32_t high;
    int32_t round;

    wide += (int64_t)1 << 30;
    high = (int32_t)(wide >> 31);

    if (shift > 0)
    {
        round = (int32_t)1 << (shift - 1);
        high = (high + round) >> shift;
    }
    else if (shift < 0)
    {
        high <<= -shift;
    }
    return high;
}

int32_t kws_dot_s8(const int8_t *a, const int8_t *b, uint32_t n)
{
    int32_t acc = 0;

#if OPT_MVE_KERNELS && defined(__ARM_FEATURE_MVE)
    while (n > 0u)
    {
        mve_pred16_t p = vctp8q(n);
        int8x16_t va = vldrbq_z_s8(a, p);
        int8x16_t vb = vldrbq_z_s8(b, p);

        acc = vmladavaq_p_s8(acc, va, vb, p);
        a += 16;
        b += 16;
        n = (n > 16u) ? (n - 16u) : 0u;
    }
#else
    uint32_t i;
    for (i = 0u; i < n; i++)
    {
        acc += (int32_t)a[i] * (int32_t)b[i];
    }
#endif
    return acc;
}

/* Scalar for short runs such as the stem's single channel. */
static inline int32_t dot_short(const int8_t *a, const int8_t *b, uint32_t n)
{
    int32_t acc = 0;
    uint32_t i;

    if (n >= 16u)
    {
        return kws_dot_s8(a, b, n);
    }
    for (i = 0u; i < n; i++)
    {
        acc += (int32_t)a[i] * (int32_t)b[i];
    }
    return acc;
}

static int32_t sum_s8(const int8_t *w, uint32_t n)
{
    int32_t acc = 0;
    uint32_t i;

    for (i = 0u; i < n; i++)
    {
        acc += (int32_t)w[i];
    }
    return acc;
}

/* Clamps to int8; ReLU is a floor at the output zero point. */
static int8_t saturate(int32_t v, int32_t out_zero, int relu)
{
    v += out_zero;
    if (relu && v < out_zero) { v = out_zero; }
    if (v >  127) { v =  127; }
    if (v < -128) { v = -128; }
    return (int8_t)v;
}

/* Weights are [out_c][k_t][k_f][in_c]. */
void kws_conv_s8(const int8_t *const *rows, int8_t *out,
                 const int8_t *w, const int32_t *bias,
                 const int32_t *mult, const int8_t *shift,
                 const kws_layer_t *l)
{
    uint32_t ot, of, oc, kt, kf;
    uint32_t in_c = l->in_c;

    for (ot = 0u; ot < l->out_t; ot++)
    {
        for (of = 0u; of < l->out_f; of++)
        {
            int8_t *o = out + (ot * l->out_f + of) * l->out_c;

            for (oc = 0u; oc < l->out_c; oc++)
            {
                int32_t acc = bias[oc];

                for (kt = 0u; kt < l->k_t; kt++)
                {
                    const int8_t *row = rows[ot * l->stride_t + kt * l->dilation];

                    for (kf = 0u; kf < l->k_f; kf++)
                    {
                        int32_t f = (int32_t)(of * l->stride_f + kf) - (int32_t)l->pad_f;
                        const int8_t *wp = w + ((oc * l->k_t + kt) * l->k_f + kf) * in_c;

                        if (f >= 0 && f < (int32_t)l->in_f)
                        {
                            acc += dot_short(row + (uint32_t)f * in_c, wp, in_c);
                        }
                        else
                        {
                            acc += l->in_zero * sum_s8(wp, in_c);
                        }
                    }
                }
                o[oc] = saturate(kws_requantise(acc, mult[oc], shift[oc]),
                                 l->out_zero, l->relu);
            }
        }
    }
}

/* Weights are [k_t][k_f][c]. */
void kws_dwconv_s8(const int8_t *const *rows, int8_t *out,
                   const int8_t *w, const int32_t *bias,
                   const int32_t *mult, const int8_t *shift,
                   const kws_layer_t *l)
{
    uint32_t ot, of, c, kt, kf;
    uint32_t ch = l->in_c;

    for (ot = 0u; ot < l->out_t; ot++)
    {
        for (of = 0u; of < l->out_f; of++)
        {
            int8_t *o = out + (ot * l->out_f + of) * ch;

            for (c = 0u; c < ch; c++)
            {
                int32_t acc = bias[c];

                for (kt = 0u; kt < l->k_t; kt++)
                {
                    const int8_t *row = rows[ot * l->stride_t + kt * l->dilation];

                    for (kf = 0u; kf < l->k_f; kf++)
                    {
                        int32_t f = (int32_t)(of * l->stride_f + kf) - (int32_t)l->pad_f;
                        int32_t wv = w[(kt * l->k_f + kf) * ch + c];

                        if (f >= 0 && f < (int32_t)l->in_f)
                        {
                            acc += wv * (int32_t)row[(uint32_t)f * ch + c];
                        }
                        else
                        {
                            acc += wv * l->in_zero;
                        }
                    }
                }
                o[c] = saturate(kws_requantise(acc, mult[c], shift[c]),
                                l->out_zero, l->relu);
            }
        }
    }
}

/* Same arithmetic as TFLite's reference int8 ADD. */
void kws_add_s8(const int8_t *a, const int8_t *b, int8_t *out, uint32_t n,
                const int32_t *mult, const int8_t *shift,
                const kws_layer_t *l)
{
    uint32_t i;

    for (i = 0u; i < n; i++)
    {
        int32_t x = ((int32_t)a[i] - l->in_zero) * (1 << ADD_LEFT_SHIFT);
        int32_t y = ((int32_t)b[i] - l->aux_zero) * (1 << ADD_LEFT_SHIFT);
        int32_t sum = kws_requantise(x, mult[0], shift[0]) +
                      kws_requantise(y, mult[1], shift[1]);

        out[i] = saturate(kws_requantise(sum, mult[2], shift[2]),
                          l->out_zero, l->relu);
    }
}
