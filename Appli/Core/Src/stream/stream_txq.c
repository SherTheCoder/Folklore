#include "stream/stream_txq.h"
#include "app/kikkar_config.h"
#include <string.h>

#define CHUNK_BYTES  (STREAM_CHUNK_SAMPLES * (uint32_t)sizeof(int16_t))

static int16_t *s_base;
static uint32_t s_slots;
static uint32_t s_head;
static uint32_t s_tail;
static uint32_t s_high;
static uint32_t s_drops;

void stream_txq_bind(void *base, uint32_t bytes)
{
    s_base  = (int16_t *)base;
    s_slots = bytes / CHUNK_BYTES;
    if (s_slots > STREAM_TXQ_CHUNKS)
    {
        s_slots = STREAM_TXQ_CHUNKS;
    }
    s_head  = 0u;
    s_tail  = 0u;
    s_high  = 0u;
    s_drops = 0u;
}

void stream_txq_unbind(void)
{
    s_base  = 0;
    s_slots = 0u;
    s_head  = 0u;
    s_tail  = 0u;
}

/* Drops the oldest chunk when the sender falls behind. */
int stream_txq_push(const int16_t *samples, uint32_t count)
{
    uint32_t depth;
    int dropped = 0;

    if (s_base == 0 || s_slots == 0u || count != STREAM_CHUNK_SAMPLES)
    {
        return 1;
    }

    if ((s_head - s_tail) >= s_slots)
    {
        s_tail++;
        s_drops++;
        dropped = 1;
    }

    memcpy(&s_base[(s_head % s_slots) * STREAM_CHUNK_SAMPLES], samples,
           CHUNK_BYTES);
    s_head++;

    depth = s_head - s_tail;
    if (depth > s_high)
    {
        s_high = depth;
    }
    return dropped;
}

uint32_t stream_txq_peek(const int16_t **samples)
{
    if (s_base == 0 || s_head == s_tail)
    {
        return 0u;
    }
    *samples = &s_base[(s_tail % s_slots) * STREAM_CHUNK_SAMPLES];
    return STREAM_CHUNK_SAMPLES;
}

void stream_txq_pop(void)
{
    if (s_head != s_tail)
    {
        s_tail++;
    }
}

uint32_t stream_txq_depth(void)
{
    return s_head - s_tail;
}

uint32_t stream_txq_capacity(void)
{
    return s_slots;
}

uint32_t stream_txq_high_water(void)
{
    return s_high;
}

uint32_t stream_txq_drops(void)
{
    return s_drops;
}
