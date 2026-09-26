#include "kws/kws_preroll.h"
#include "app/kikkar_config.h"

#define PREROLL_MASK  (KWS_PREROLL_SAMPLES - 1u)

static int16_t  s_ring[KWS_PREROLL_SAMPLES] __attribute__((section(".kikkar_ram")));
static uint32_t s_head;

void kws_preroll_init(void)
{
    s_head = 0u;
}

void kws_preroll_write(int16_t sample)
{
    s_ring[s_head & PREROLL_MASK] = sample;
    s_head++;
}

uint32_t kws_preroll_head(void)
{
    return s_head;
}

uint32_t kws_preroll_tail(void)
{
    return (s_head > KWS_PREROLL_SAMPLES) ? (s_head - KWS_PREROLL_SAMPLES) : 0u;
}

uint32_t kws_preroll_read(uint32_t from, int16_t *dst, uint32_t n)
{
    uint32_t tail = kws_preroll_tail();
    uint32_t i;

    if ((int32_t)(from - tail) < 0)
    {
        from = tail;
    }
    if ((from + n) > s_head)
    {
        n = (s_head > from) ? (s_head - from) : 0u;
    }
    for (i = 0u; i < n; i++)
    {
        dst[i] = s_ring[(from + i) & PREROLL_MASK];
    }
    return n;
}

uint32_t kws_preroll_rewind(uint32_t from, uint32_t samples)
{
    uint32_t tail = kws_preroll_tail();

    if (from < samples)
    {
        return tail;
    }
    from -= samples;
    return ((int32_t)(from - tail) < 0) ? tail : from;
}
