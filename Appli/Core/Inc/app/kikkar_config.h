#ifndef KIKKAR_CONFIG_H
#define KIKKAR_CONFIG_H

#define KIKKAR_KEYWORD              "Utho Kikkar"

#define KWS_FS_HZ                   16000
#define KWS_DECIM                   3
#define KWS_FIR_TAPS                24

#define KWS_FRAME_LEN               480
#define KWS_FRAME_HOP               160
#define KWS_FFT_LEN                 512
#define KWS_MEL_BINS                40
#define KWS_MEL_LO_HZ               20.0f
#define KWS_MEL_HI_HZ               7600.0f
#define KWS_LOG_FLOOR               1e-8f
#define KWS_CMS_TAU_MS              3000
#define KWS_CMS_WARMUP_FRAMES       50

#define KWS_STEP_FRAMES             3

#define KWS_PREROLL_SAMPLES         8192u
#define KWS_LOOKBACK_MS             320

#define KWS_SMOOTH_STEPS            3
#define KWS_THRESHOLD               0.70f
#define KWS_REFRACTORY_MS           1000

#define KWS_GATE_MARGIN_DB          6.0f
#define KWS_GATE_HANG_MS            300
#define KWS_GATE_LOOKBACK_STEPS     10

#define STREAM_CHUNK_SAMPLES        320
#define STREAM_TXQ_CHUNKS           29
#define STREAM_END_SILENCE_MS       800
#define STREAM_MAX_UTTERANCE_MS     10000
#define STREAM_PROTO_VERSION        1

#define NET_LOCAL_IP0               192
#define NET_LOCAL_IP1               168
#define NET_LOCAL_IP2               1
#define NET_LOCAL_IP3               50

#define NET_NETMASK0                255
#define NET_NETMASK1                255
#define NET_NETMASK2                255
#define NET_NETMASK3                0

#define NET_GATEWAY_IP0             192
#define NET_GATEWAY_IP1             168
#define NET_GATEWAY_IP2             1
#define NET_GATEWAY_IP3             1

#define NET_SERVER_IP0              192
#define NET_SERVER_IP1              168
#define NET_SERVER_IP2              1
#define NET_SERVER_IP3              10
#define NET_SERVER_PORT             5555

#define NET_MAC0                    0x02
#define NET_MAC1                    0x00
#define NET_MAC2                    0x4B
#define NET_MAC3                    0x49
#define NET_MAC4                    0x4B
#define NET_MAC5                    0x01

#define NET_FRAME_MAX               1536u
#define NET_RX_DESC                 4u
#define NET_TX_DESC                 4u
#define NET_TCP_MSS                 1460u
#define NET_TCP_RTO_MS              200u
#define NET_TCP_MAX_RETRIES         6u
#define NET_RECONNECT_MS            1000u

#define OPT_GATED_INFERENCE         1
#define OPT_SHARED_ARENA            1
#define OPT_IDLE_SLEEP              1
#define OPT_EARLY_LINK              1
#define OPT_MVE_KERNELS             1

#define KIKKAR_REPORT_MS            2000

#endif /* KIKKAR_CONFIG_H */
