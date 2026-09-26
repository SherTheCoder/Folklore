#include "kws/kws_fft.h"
#include "app/kikkar_config.h"
#include <math.h>

#define FFT_N      (KWS_FFT_LEN)
#define FFT_M      (FFT_N / 2u)

typedef struct
{
    float work[2u * FFT_M];
    float twid[FFT_M];
    float split[FFT_M + 2u];
} fft_work_t;

static fft_work_t *s_w;

uint32_t kws_fft_workspace_bytes(void)
{
    return (uint32_t)sizeof(fft_work_t);
}

/* Builds the twiddles for the half-length pass and the real split. */
void kws_fft_attach(void *mem)
{
    float *twid, *split;
    uint32_t k;

    s_w   = (fft_work_t *)mem;
    twid  = s_w->twid;
    split = s_w->split;

    for (k = 0u; k < FFT_M / 2u; k++)
    {
        float a = -2.0f * 3.14159265358979f * (float)k / (float)FFT_M;
        twid[2u * k]      = cosf(a);
        twid[2u * k + 1u] = sinf(a);
    }
    for (k = 0u; k <= FFT_M / 2u; k++)
    {
        float a = -2.0f * 3.14159265358979f * (float)k / (float)FFT_N;
        split[2u * k]      = cosf(a);
        split[2u * k + 1u] = sinf(a);
    }
}

/* Gold-Rader bit reversal, in place. */
static void bit_reverse(float *z, uint32_t n)
{
    uint32_t i;
    uint32_t j = 0u;

    for (i = 0u; i < n - 1u; i++)
    {
        uint32_t m = n >> 1;

        if (i < j)
        {
            float tr = z[2u * i];
            float ti = z[2u * i + 1u];
            z[2u * i]      = z[2u * j];
            z[2u * i + 1u] = z[2u * j + 1u];
            z[2u * j]      = tr;
            z[2u * j + 1u] = ti;
        }
        while (j >= m && m > 0u)
        {
            j -= m;
            m >>= 1;
        }
        j += m;
    }
}

static void fft_complex(float *z, uint32_t n)
{
    uint32_t step;

    bit_reverse(z, n);

    for (step = 1u; step < n; step <<= 1)
    {
        uint32_t span = step << 1;
        uint32_t skip = n / span;
        uint32_t k;

        for (k = 0u; k < step; k++)
        {
            float wr = s_w->twid[2u * k * skip];
            float wi = s_w->twid[2u * k * skip + 1u];
            uint32_t i;

            for (i = k; i < n; i += span)
            {
                uint32_t p = 2u * i;
                uint32_t q = 2u * (i + step);
                float tr = z[q] * wr - z[q + 1u] * wi;
                float ti = z[q] * wi + z[q + 1u] * wr;

                z[q]      = z[p] - tr;
                z[q + 1u] = z[p + 1u] - ti;
                z[p]      += tr;
                z[p + 1u] += ti;
            }
        }
    }
}

/* Real transform done as a half-length complex one plus a split step. */
void kws_fft_power_spectrum(const float *in, float *power)
{
    float *work = s_w->work;
    const float *split = s_w->split;
    uint32_t k;
    float dc, nyq;

    for (k = 0u; k < FFT_M; k++)
    {
        work[2u * k]      = in[2u * k];
        work[2u * k + 1u] = in[2u * k + 1u];
    }

    fft_complex(work, FFT_M);

    dc  = work[0] + work[1];
    nyq = work[0] - work[1];
    power[0]     = dc * dc;
    power[FFT_M] = nyq * nyq;

    for (k = 1u; k < FFT_M / 2u + 1u; k++)
    {
        uint32_t m = FFT_M - k;
        float ar = work[2u * k],  ai = work[2u * k + 1u];
        float br = work[2u * m],  bi = work[2u * m + 1u];

        float er = 0.5f * (ar + br);
        float ei = 0.5f * (ai - bi);
        float orr = 0.5f * (ai + bi);
        float oi = -0.5f * (ar - br);

        float wr = split[2u * k];
        float wi = split[2u * k + 1u];

        float xr = er + (wr * orr - wi * oi);
        float xi = ei + (wr * oi + wi * orr);

        power[k] = xr * xr + xi * xi;

        if (m != k)
        {
            float yr = er - (wr * orr - wi * oi);
            float yi = -(ei - (wr * oi + wi * orr));
            power[m] = yr * yr + yi * yi;
        }
    }
}
