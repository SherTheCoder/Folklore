#include "stream/stream_session.h"
#include "stream/stream_txq.h"
#include "stream/stream_proto.h"
#include "kws/kws_preroll.h"
#include "net/net_tcp.h"
#include "opt/opt_arena.h"
#include "app/kikkar_config.h"
#include "flower_display.h"
#include <string.h>

#define LOOKBACK_SAMPLES  ((uint32_t)KWS_LOOKBACK_MS * KWS_FS_HZ / 1000u)
#define AUDIO_MSG_BYTES   (STREAM_HDR_BYTES + STREAM_CHUNK_SAMPLES * 2u)

static uint8_t  s_msg[AUDIO_MSG_BYTES];
static int16_t  s_chunk[STREAM_CHUNK_SAMPLES];
static uint32_t s_send_pos;
static uint32_t s_start_pos;
static uint32_t s_trigger_pos;
static uint32_t s_trigger_cyc;
static uint32_t s_utterance;
static uint32_t s_payload;
static uint32_t s_overhead;
static uint32_t s_samples;
static uint32_t s_first_us;
static uint32_t s_failed;
static int      s_active;
static int      s_first_sent;

void stream_session_init(void)
{
    s_active     = 0;
    s_first_sent = 0;
    s_utterance  = 0u;
    s_payload    = 0u;
    s_overhead   = 0u;
    s_samples    = 0u;
    s_first_us   = 0u;
    s_failed     = 0u;
}

static int send_start(uint32_t preroll_samples)
{
    stream_start_t start;
    uint8_t buf[STREAM_HDR_BYTES + sizeof(stream_start_t)];

    start.version         = STREAM_PROTO_VERSION;
    start.channels        = 1u;
    start.sample_bits     = 16u;
    start.little_endian   = 1u;
    start.sample_rate     = KWS_FS_HZ;
    start.utterance       = s_utterance;
    start.preroll_samples = preroll_samples;
    memset(start.keyword, 0, sizeof(start.keyword));
    memcpy(start.keyword, KIKKAR_KEYWORD, sizeof(KIKKAR_KEYWORD) - 1u);

    stream_write_header(buf, STREAM_MSG_START, 0u, (uint16_t)sizeof(start));
    memcpy(&buf[STREAM_HDR_BYTES], &start, sizeof(start));

    if (net_tcp_send(buf, sizeof(buf)) != sizeof(buf))
    {
        return -1;
    }
    s_overhead += sizeof(buf);
    return 0;
}

/* Starts sending KWS_LOOKBACK_MS before the trigger. */
int stream_session_begin(uint32_t trigger_pos, uint32_t trigger_cycles)
{
    void *arena;
    uint32_t start_pos;

    if (s_active)
    {
        return -1;
    }
    if (net_tcp_state() != NET_TCP_ESTABLISHED)
    {
        s_failed++;
        return -2;
    }

    arena = opt_arena_claim(OPT_ARENA_STREAM, STREAM_TXQ_CHUNKS *
                            STREAM_CHUNK_SAMPLES * (uint32_t)sizeof(int16_t));
    if (arena == 0)
    {
        s_failed++;
        return -3;
    }
    stream_txq_bind(arena, STREAM_TXQ_CHUNKS * STREAM_CHUNK_SAMPLES *
                    (uint32_t)sizeof(int16_t));

    start_pos     = kws_preroll_rewind(trigger_pos, LOOKBACK_SAMPLES);
    s_trigger_pos = trigger_pos;
    s_trigger_cyc = trigger_cycles;
    s_start_pos   = start_pos;
    s_send_pos    = start_pos;
    s_utterance++;
    s_first_sent  = 0;
    s_first_us    = 0u;
    s_samples     = 0u;

    if (send_start(trigger_pos - start_pos) != 0)
    {
        stream_txq_unbind();
        opt_arena_release(OPT_ARENA_STREAM);
        s_failed++;
        return -4;
    }

    s_active = 1;
    return 0;
}

static void fill_queue(void)
{
    while ((s_send_pos + STREAM_CHUNK_SAMPLES) <= kws_preroll_head())
    {
        if (stream_txq_depth() >= stream_txq_capacity())
        {
            break;
        }
        if (kws_preroll_read(s_send_pos, s_chunk, STREAM_CHUNK_SAMPLES) !=
            STREAM_CHUNK_SAMPLES)
        {
            break;
        }
        (void)stream_txq_push(s_chunk, STREAM_CHUNK_SAMPLES);
        s_send_pos += STREAM_CHUNK_SAMPLES;
    }
}

static void drain_queue(void)
{
    const int16_t *chunk;

    while (stream_txq_peek(&chunk) != 0u)
    {
        uint8_t flags = 0u;

        if (net_tcp_sendable() < AUDIO_MSG_BYTES)
        {
            break;
        }
        if (!s_first_sent)
        {
            flags |= STREAM_FLAG_FIRST;
        }
        if ((s_start_pos + s_samples) < s_trigger_pos)
        {
            flags |= STREAM_FLAG_PREROLL;
        }

        stream_write_header(s_msg, STREAM_MSG_AUDIO, flags,
                            (uint16_t)(STREAM_CHUNK_SAMPLES * 2u));
        memcpy(&s_msg[STREAM_HDR_BYTES], chunk, STREAM_CHUNK_SAMPLES * 2u);

        if (net_tcp_send(s_msg, AUDIO_MSG_BYTES) != AUDIO_MSG_BYTES)
        {
            break;
        }
        stream_txq_pop();

        s_payload  += STREAM_CHUNK_SAMPLES * 2u;
        s_overhead += STREAM_HDR_BYTES;
        s_samples  += STREAM_CHUNK_SAMPLES;

        if (!s_first_sent)
        {
            s_first_us   = flower_us_since(s_trigger_cyc);
            s_first_sent = 1;
        }
    }
}

void stream_session_pump(void)
{
    if (!s_active)
    {
        return;
    }
    fill_queue();
    drain_queue();
}

void stream_session_end(uint32_t reason)
{
    stream_end_t end;
    uint8_t buf[STREAM_HDR_BYTES + sizeof(stream_end_t)];

    if (!s_active)
    {
        return;
    }

    drain_queue();

    end.utterance = s_utterance;
    end.samples   = s_samples;
    end.reason    = reason;
    stream_write_header(buf, STREAM_MSG_END, 0u, (uint16_t)sizeof(end));
    memcpy(&buf[STREAM_HDR_BYTES], &end, sizeof(end));

    if (net_tcp_send(buf, sizeof(buf)) == sizeof(buf))
    {
        s_overhead += sizeof(buf);
    }

    stream_txq_unbind();
    opt_arena_release(OPT_ARENA_STREAM);
    s_active = 0;
}

int stream_session_active(void)
{
    return s_active;
}

uint32_t stream_session_utterance(void)
{
    return s_utterance;
}

uint32_t stream_session_first_audio_us(void)
{
    return s_first_us;
}

uint32_t stream_session_payload_bytes(void)
{
    return s_payload;
}

uint32_t stream_session_overhead_bytes(void)
{
    return s_overhead;
}

uint32_t stream_session_samples_sent(void)
{
    return s_samples;
}

uint32_t stream_session_failed_handoffs(void)
{
    return s_failed;
}
