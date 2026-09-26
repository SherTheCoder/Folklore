#ifndef OPT_ARENA_H
#define OPT_ARENA_H

#include <stdint.h>

#define OPT_ARENA_BYTES  19456u

typedef enum
{
    OPT_ARENA_FREE = 0,
    OPT_ARENA_KWS,
    OPT_ARENA_STREAM
} opt_arena_owner_t;

void  opt_arena_init(void);
void *opt_arena_claim(opt_arena_owner_t owner, uint32_t bytes);
void  opt_arena_release(opt_arena_owner_t owner);

opt_arena_owner_t opt_arena_owner(void);
uint32_t opt_arena_peak(void);
uint32_t opt_arena_conflicts(void);
uint32_t opt_arena_saved_bytes(void);

#endif /* OPT_ARENA_H */
