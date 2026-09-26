#include "opt/opt_arena.h"

static uint8_t  s_block[OPT_ARENA_BYTES] __attribute__((aligned(32),
                                                        section(".kikkar_ram")));
static opt_arena_owner_t s_owner;
static uint32_t s_peak;
static uint32_t s_peak_kws;
static uint32_t s_peak_stream;
static uint32_t s_conflicts;

void opt_arena_init(void)
{
    s_owner       = OPT_ARENA_FREE;
    s_peak        = 0u;
    s_peak_kws    = 0u;
    s_peak_stream = 0u;
    s_conflicts   = 0u;
}

/* Fails, and counts a conflict, while the other side still holds the block. */
void *opt_arena_claim(opt_arena_owner_t owner, uint32_t bytes)
{
    if (owner == OPT_ARENA_FREE || bytes > OPT_ARENA_BYTES)
    {
        s_conflicts++;
        return 0;
    }
    if (s_owner != OPT_ARENA_FREE && s_owner != owner)
    {
        s_conflicts++;
        return 0;
    }

    s_owner = owner;
    if (bytes > s_peak)
    {
        s_peak = bytes;
    }
    if (owner == OPT_ARENA_KWS && bytes > s_peak_kws)
    {
        s_peak_kws = bytes;
    }
    if (owner == OPT_ARENA_STREAM && bytes > s_peak_stream)
    {
        s_peak_stream = bytes;
    }
    return s_block;
}

/* A release by the wrong owner only bumps the conflict counter. */
void opt_arena_release(opt_arena_owner_t owner)
{
    if (s_owner != owner)
    {
        s_conflicts++;
        return;
    }
    s_owner = OPT_ARENA_FREE;
}

opt_arena_owner_t opt_arena_owner(void)
{
    return s_owner;
}

uint32_t opt_arena_peak(void)
{
    return s_peak;
}

uint32_t opt_arena_conflicts(void)
{
    return s_conflicts;
}

/* Separate buffers would cost both peaks; the shared block costs the larger. */
uint32_t opt_arena_saved_bytes(void)
{
    return (s_peak_kws < s_peak_stream) ? s_peak_kws : s_peak_stream;
}
