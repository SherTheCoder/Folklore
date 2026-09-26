#ifndef STREAM_PROTO_H
#define STREAM_PROTO_H

#include <stdint.h>

#define STREAM_HDR_BYTES    4u

#define STREAM_MSG_START    1u
#define STREAM_MSG_AUDIO    2u
#define STREAM_MSG_END      3u
#define STREAM_MSG_RESULT   4u
#define STREAM_MSG_ERROR    5u

#define STREAM_FLAG_PREROLL 0x01u
#define STREAM_FLAG_FIRST   0x02u

#define STREAM_END_SILENCE  0u
#define STREAM_END_TIMEOUT  1u
#define STREAM_END_ABORT    2u

typedef struct
{
    uint8_t  version;
    uint8_t  channels;
    uint8_t  sample_bits;
    uint8_t  little_endian;
    uint32_t sample_rate;
    uint32_t utterance;
    uint32_t preroll_samples;
    char     keyword[16];
} stream_start_t;

typedef struct
{
    uint32_t utterance;
    uint32_t samples;
    uint32_t reason;
} stream_end_t;

static inline void stream_write_header(uint8_t *dst, uint8_t type,
                                       uint8_t flags, uint16_t len)
{
    dst[0] = type;
    dst[1] = flags;
    dst[2] = (uint8_t)(len >> 8);
    dst[3] = (uint8_t)len;
}

#endif /* STREAM_PROTO_H */
