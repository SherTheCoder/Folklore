#include "app/kikkar_app.h"
#include "app/kikkar_config.h"
#include "kws/kws_pipeline.h"
#include "kws/kws_detector.h"
#include "stream/stream_session.h"
#include "stream/stream_proto.h"
#include "net/net_eth.h"
#include "net/net_ip.h"
#include "net/net_tcp.h"
#include "opt/opt_arena.h"
#include "opt/opt_gate.h"
#include "opt/opt_link.h"
#include "opt/opt_metrics.h"
#include "opt/opt_sleep.h"
#include "flower_ctl.h"
#include "stm32n6xx_hal.h"
#include <stdio.h>

static kikkar_state_t s_state;
static uint32_t s_stream_started;
static uint32_t s_last_speech;
static uint32_t s_last_report;

static int speech_present(void)
{
    return (g_flower_rms_dbfs > (g_flower_floor_dbfs + KWS_GATE_MARGIN_DB));
}

int kikkar_app_init(void)
{
    static const uint8_t mac[6] = {
        NET_MAC0, NET_MAC1, NET_MAC2, NET_MAC3, NET_MAC4, NET_MAC5
    };
    void *arena;
    int rc;

    opt_arena_init();
    opt_sleep_init();

    rc = kws_pipeline_init();
    if (rc != 0)
    {
        printf("kikkar: model rejected (%d) -- see model/README.md\r\n", rc);
    }

    arena = opt_arena_claim(OPT_ARENA_KWS, kws_pipeline_workspace_bytes());
    if (arena == 0)
    {
        printf("kikkar: arena unavailable\r\n");
        return -1;
    }
    (void)kws_pipeline_attach(arena, kws_pipeline_workspace_bytes());

    if (net_eth_init(mac) != 0)
    {
        printf("kikkar: ETH1 did not come out of reset\r\n");
    }
    net_ip_init(NET_IP4(NET_LOCAL_IP0, NET_LOCAL_IP1, NET_LOCAL_IP2, NET_LOCAL_IP3),
                NET_IP4(NET_NETMASK0, NET_NETMASK1, NET_NETMASK2, NET_NETMASK3),
                NET_IP4(NET_GATEWAY_IP0, NET_GATEWAY_IP1, NET_GATEWAY_IP2,
                        NET_GATEWAY_IP3));
    net_tcp_init();
    opt_link_init(NET_IP4(NET_SERVER_IP0, NET_SERVER_IP1, NET_SERVER_IP2,
                          NET_SERVER_IP3), NET_SERVER_PORT);
    stream_session_init();

    opt_metrics_report_budget();
    printf("Listening for \"%s\". Say it and the flower opens.\r\n\r\n",
           KIKKAR_KEYWORD);

    s_state       = KIKKAR_LISTENING;
    s_last_report = HAL_GetTick();
    return rc;
}

void kikkar_app_audio_task(void)
{
    kws_pipeline_task();
}

/* Listening has to stop before its workspace is handed to the sender. */
static void enter_streaming(void)
{
    uint32_t pos = kws_detector_trigger_pos();
    uint32_t cyc = kws_detector_trigger_cycles();

    kws_pipeline_set_inference(0);
    opt_arena_release(OPT_ARENA_KWS);

    if (stream_session_begin(pos, cyc) != 0)
    {
        (void)kws_pipeline_attach(opt_arena_claim(OPT_ARENA_KWS,
                                                  kws_pipeline_workspace_bytes()),
                                  kws_pipeline_workspace_bytes());
        kws_pipeline_set_inference(1);
        kws_detector_rearm();
        s_state = KIKKAR_RECOVERING;
        return;
    }

    opt_gate_force(1);
    s_stream_started = HAL_GetTick();
    s_last_speech    = s_stream_started;
    s_state          = KIKKAR_STREAMING;
}

static void leave_streaming(uint32_t reason)
{
    void *arena;

    stream_session_end(reason);
    opt_metrics_report_handoff();

    opt_gate_force(0);

    arena = opt_arena_claim(OPT_ARENA_KWS, kws_pipeline_workspace_bytes());
    (void)kws_pipeline_attach(arena, kws_pipeline_workspace_bytes());
    kws_pipeline_set_inference(1);
    kws_detector_rearm();

    s_state = KIKKAR_LISTENING;
}

static int utterance_over(uint32_t *reason)
{
    uint32_t now = HAL_GetTick();

    if (speech_present())
    {
        s_last_speech = now;
    }
    if ((now - s_stream_started) >= STREAM_MAX_UTTERANCE_MS)
    {
        *reason = STREAM_END_TIMEOUT;
        return 1;
    }
    if ((now - s_last_speech) >= STREAM_END_SILENCE_MS)
    {
        *reason = STREAM_END_SILENCE;
        return 1;
    }
    if (net_tcp_state() != NET_TCP_ESTABLISHED)
    {
        *reason = STREAM_END_ABORT;
        return 1;
    }
    return 0;
}

void kikkar_app_tick(void)
{
    kws_event_t event;
    uint32_t reason;
    uint32_t now;

    opt_link_task();

    event = kws_pipeline_take_event();
    if (event == KWS_EVENT_SPEECH)
    {
        opt_link_prepare();
    }

    switch (s_state)
    {
    case KIKKAR_LISTENING:
        if (event == KWS_EVENT_KEYWORD)
        {
            enter_streaming();
        }
        break;

    case KIKKAR_STREAMING:
        stream_session_pump();
        if (utterance_over(&reason))
        {
            leave_streaming(reason);
        }
        break;

    case KIKKAR_RECOVERING:
        if (opt_link_ready())
        {
            s_state = KIKKAR_LISTENING;
        }
        break;

    default:
        s_state = KIKKAR_LISTENING;
        break;
    }

    now = HAL_GetTick();
    if ((now - s_last_report) >= KIKKAR_REPORT_MS)
    {
        s_last_report = now;
        opt_metrics_report_live();
    }
}

kikkar_state_t kikkar_app_state(void)
{
    return s_state;
}

uint32_t kikkar_app_is_blooming(void)
{
    return (s_state == KIKKAR_STREAMING) ? 1u : 0u;
}

const char *kikkar_app_state_name(void)
{
    switch (s_state)
    {
    case KIKKAR_LISTENING:  return "listening";
    case KIKKAR_STREAMING:  return "streaming";
    case KIKKAR_RECOVERING: return "recovering";
    default:                return "boot";
    }
}
