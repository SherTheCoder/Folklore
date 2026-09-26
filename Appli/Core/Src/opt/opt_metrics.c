#include "opt/opt_metrics.h"
#include "opt/opt_arena.h"
#include "opt/opt_gate.h"
#include "opt/opt_sleep.h"
#include "opt/opt_link.h"
#include "kws/kws_model.h"
#include "kws/kws_pipeline.h"
#include "kws/kws_detector.h"
#include "kws/kws_frontend.h"
#include "stream/stream_session.h"
#include "stream/stream_proto.h"
#include "stream/stream_txq.h"
#include "net/net_eth.h"
#include "net/net_ip.h"
#include "net/net_tcp.h"
#include "app/kikkar_config.h"
#include "flower_audio.h"
#include "flower_ctl.h"
#include <stdio.h>

extern uint32_t __skikkar_ram;
extern uint32_t __ekikkar_ram;

#define CAPTURE_RING_BYTES  (FLOWER_AUDIO_RING * 2u)
#define PREROLL_BYTES       (KWS_PREROLL_SAMPLES * 2u)
#define FRONTEND_STATE      (KWS_MEL_BINS * 8u)
#define TXQ_BYTES           (STREAM_TXQ_CHUNKS * STREAM_CHUNK_SAMPLES * 2u)
#define ETH_BYTES           ((NET_RX_DESC + NET_TX_DESC) * (NET_FRAME_MAX + 16u))
#define IP_BYTES            (NET_FRAME_MAX + NET_TCP_MSS)
#define STREAM_BYTES        (STREAM_HDR_BYTES + STREAM_CHUNK_SAMPLES * 4u)

uint32_t opt_metrics_reserved_bytes(void)
{
    return CAPTURE_RING_BYTES + PREROLL_BYTES + FRONTEND_STATE +
           OPT_ARENA_BYTES + ETH_BYTES + IP_BYTES + STREAM_BYTES;
}

/* Without sharing, the smaller side would need a block of its own. */
static uint32_t overlay_saving(void)
{
    uint32_t listen = kws_pipeline_workspace_bytes();

    return (listen < TXQ_BYTES) ? listen : TXQ_BYTES;
}

void opt_metrics_report_budget(void)
{
    const kws_model_header_t *m = kws_model_info();
    uint32_t total = opt_metrics_reserved_bytes();

    printf("\r\n=== Kikkar edge budget ===\r\n");
    printf("  capture ring     %7u B  48 kHz, shared with the animation\r\n",
           (unsigned)CAPTURE_RING_BYTES);
    printf("  pre-roll         %7u B  %u ms of 16 kHz retained audio\r\n",
           (unsigned)PREROLL_BYTES,
           (unsigned)(KWS_PREROLL_SAMPLES * 1000u / KWS_FS_HZ));
    printf("  front end        %7u B  running mean and band edges\r\n",
           (unsigned)FRONTEND_STATE);
    printf("  shared arena     %7u B  listening OR streaming, never both\r\n",
           (unsigned)OPT_ARENA_BYTES);
    printf("    listening      %7u B  model, front end, fft, gate look-back\r\n",
           (unsigned)kws_pipeline_workspace_bytes());
    printf("    streaming      %7u B  %u chunks of 20 ms audio\r\n",
           (unsigned)TXQ_BYTES, (unsigned)STREAM_TXQ_CHUNKS);
    printf("  ethernet rings   %7u B  %u rx + %u tx of %u B\r\n",
           (unsigned)ETH_BYTES, (unsigned)NET_RX_DESC, (unsigned)NET_TX_DESC,
           (unsigned)NET_FRAME_MAX);
    printf("  ip and tcp       %7u B  one frame staging, one segment in flight\r\n",
           (unsigned)IP_BYTES);
    printf("  sender           %7u B  one audio message, one chunk\r\n",
           (unsigned)STREAM_BYTES);
    printf("  ----------------------------\r\n");
    printf("  voice path total %7u B  of 262144 B\r\n", (unsigned)total);
    printf("  arena overlay saves %5u B against a buffer per phase\r\n",
           (unsigned)overlay_saving());
    printf("  .kikkar_ram      %7u B reserved at 0x%08lX\r\n",
           (unsigned)((uint32_t)&__ekikkar_ram - (uint32_t)&__skikkar_ram),
           (unsigned long)(uint32_t)&__skikkar_ram);

    if (m != 0)
    {
        printf("  model            %7u B in flash, %u ops, %u int8 weights, \"%s\"%s\r\n",
               (unsigned)kws_model_flash_bytes(), (unsigned)m->layer_count,
               (unsigned)kws_model_weights(), m->label,
               (m->flags & KWS_MODEL_FLAG_PLACEHOLDER) ? ", untrained placeholder" : "");
        printf("  model arena      %7u B  3 activation slots + %u B of streaming state\r\n",
               (unsigned)m->arena_bytes, (unsigned)m->state_bytes);
    }
    else
    {
        printf("  model            not loaded -- see model/README.md\r\n");
    }
    printf("\r\n");
}

void opt_metrics_report_live(void)
{
    printf("cpu=%lu.%lu%% peak=%lu.%lu%% | gate=%lu%% infer=%lu skip=%lu "
           "fe=%luus nn=%luus | score=%lu fire=%lu | "
           "link=%d tcp=%d up=%lums | tx=%luB ovh=%luB rexmit=%lu drop=%lu\r\n",
           (unsigned long)(opt_sleep_busy_permille() / 10u),
           (unsigned long)(opt_sleep_busy_permille() % 10u),
           (unsigned long)(opt_sleep_peak_permille() / 10u),
           (unsigned long)(opt_sleep_peak_permille() % 10u),
           (unsigned long)opt_gate_duty_pct(),
           (unsigned long)opt_gate_steps_run(),
           (unsigned long)opt_gate_steps_skipped(),
           (unsigned long)kws_frontend_last_us(),
           (unsigned long)kws_model_last_us(),
           (unsigned long)(kws_detector_score() * 1000.0f),
           (unsigned long)kws_detector_fire_count(),
           (int)net_eth_link(),
           (int)net_tcp_state(),
           (unsigned long)opt_link_uptime_ms(),
           (unsigned long)stream_session_payload_bytes(),
           (unsigned long)stream_session_overhead_bytes(),
           (unsigned long)net_tcp_retransmits(),
           (unsigned long)stream_txq_drops());
}

void opt_metrics_report_handoff(void)
{
    uint32_t payload  = stream_session_payload_bytes();
    uint32_t overhead = stream_session_overhead_bytes();
    uint32_t total    = payload + overhead;

    printf("handoff: utterance %lu | trigger->first audio %lu us | "
           "%lu samples | payload %lu B overhead %lu B (%lu.%lu%%) | "
           "queue peak %lu of %lu | arp %lu | failed %lu\r\n",
           (unsigned long)stream_session_utterance(),
           (unsigned long)stream_session_first_audio_us(),
           (unsigned long)stream_session_samples_sent(),
           (unsigned long)payload, (unsigned long)overhead,
           (unsigned long)((total != 0u) ? (overhead * 100u / total) : 0u),
           (unsigned long)((total != 0u) ? ((overhead * 1000u / total) % 10u) : 0u),
           (unsigned long)stream_txq_high_water(),
           (unsigned long)stream_txq_capacity(),
           (unsigned long)net_ip_arp_requests(),
           (unsigned long)stream_session_failed_handoffs());
}
