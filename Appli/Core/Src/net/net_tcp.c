#include "net/net_tcp.h"
#include "net/net_ip.h"
#include "app/kikkar_config.h"
#include "stm32n6xx_hal.h"
#include <string.h>

#define TCP_FIN   0x01u
#define TCP_SYN   0x02u
#define TCP_RST   0x04u
#define TCP_PSH   0x08u
#define TCP_ACK   0x10u
#define TCP_HDR   20u

static uint8_t  s_pending[NET_TCP_MSS] __attribute__((section(".kikkar_ram")));
static uint32_t s_pending_len;
static uint32_t s_pending_seq;
static uint32_t s_pending_tick;
static uint32_t s_retries;

static net_tcp_state_t s_state;
static uint32_t s_peer_ip;
static uint16_t s_peer_port;
static uint16_t s_local_port = 49152u;
static uint32_t s_snd_nxt;
static uint32_t s_snd_una;
static uint32_t s_rcv_nxt;
static uint32_t s_bytes_sent;
static uint32_t s_retransmits;
static uint32_t s_resets;

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int emit(uint8_t flags, uint32_t seq, const uint8_t *payload, uint32_t len)
{
    uint8_t *tcp = net_ip_tx_begin(s_peer_ip, NET_PROTO_TCP);
    uint32_t total;

    if (tcp == 0)
    {
        return -1;
    }

    wr16(&tcp[0], s_local_port);
    wr16(&tcp[2], s_peer_port);
    wr32(&tcp[4], seq);
    wr32(&tcp[8], ((flags & TCP_ACK) != 0u) ? s_rcv_nxt : 0u);
    tcp[12] = 0x50u;
    tcp[13] = flags;
    wr16(&tcp[14], 4096u);
    wr16(&tcp[16], 0u);
    wr16(&tcp[18], 0u);

    if (len != 0u)
    {
        memcpy(&tcp[TCP_HDR], payload, len);
    }
    total = TCP_HDR + len;
    wr16(&tcp[16], net_ip_transport_checksum(net_ip_local(), s_peer_ip,
                                             NET_PROTO_TCP, tcp, total));
    return net_ip_tx_commit(total);
}

void net_tcp_init(void)
{
    s_state        = NET_TCP_CLOSED;
    s_pending_len  = 0u;
    s_retries      = 0u;
    s_snd_nxt      = 0u;
    s_snd_una      = 0u;
    s_rcv_nxt      = 0u;
    s_bytes_sent   = 0u;
    s_retransmits  = 0u;
    s_resets       = 0u;
}

int net_tcp_connect(uint32_t ip, uint16_t port)
{
    if (s_state != NET_TCP_CLOSED)
    {
        return -1;
    }

    s_peer_ip     = ip;
    s_peer_port   = port;
    s_local_port  = (uint16_t)(49152u + (HAL_GetTick() & 0x3FFFu));
    s_snd_nxt     = HAL_GetTick() * 2654435761u;
    s_snd_una     = s_snd_nxt;
    s_rcv_nxt     = 0u;
    s_pending_len = 0u;
    s_retries     = 0u;

    if (emit(TCP_SYN, s_snd_nxt, 0, 0u) != 0)
    {
        return -2;
    }
    s_snd_nxt++;
    s_pending_tick = HAL_GetTick();
    s_state = NET_TCP_CONNECTING;
    return 0;
}

void net_tcp_close(void)
{
    if (s_state == NET_TCP_ESTABLISHED)
    {
        (void)emit(TCP_FIN | TCP_ACK, s_snd_nxt, 0, 0u);
        s_snd_nxt++;
        s_state = NET_TCP_CLOSING;
    }
    else
    {
        s_state = NET_TCP_CLOSED;
    }
}

net_tcp_state_t net_tcp_state(void)
{
    return s_state;
}

uint32_t net_tcp_sendable(void)
{
    if (s_state != NET_TCP_ESTABLISHED || s_pending_len != 0u)
    {
        return 0u;
    }
    return NET_TCP_MSS;
}

/* No Nagle and no batching: each chunk goes out at once. */
uint32_t net_tcp_send(const uint8_t *data, uint32_t len)
{
    if (net_tcp_sendable() == 0u)
    {
        return 0u;
    }
    if (len > NET_TCP_MSS)
    {
        len = NET_TCP_MSS;
    }

    memcpy(s_pending, data, len);
    s_pending_len  = len;
    s_pending_seq  = s_snd_nxt;
    s_pending_tick = HAL_GetTick();
    s_retries      = 0u;

    if (emit(TCP_PSH | TCP_ACK, s_pending_seq, s_pending, len) != 0)
    {
        s_pending_len = 0u;
        return 0u;
    }
    s_snd_nxt    += len;
    s_bytes_sent += len;
    return len;
}

void net_tcp_poll(void)
{
    uint32_t now = HAL_GetTick();

    if (s_state == NET_TCP_CLOSED)
    {
        return;
    }
    if (s_state == NET_TCP_CONNECTING)
    {
        if ((now - s_pending_tick) >= NET_TCP_RTO_MS)
        {
            if (++s_retries > NET_TCP_MAX_RETRIES)
            {
                s_state = NET_TCP_CLOSED;
                return;
            }
            (void)emit(TCP_SYN, s_snd_una, 0, 0u);
            s_pending_tick = now;
        }
        return;
    }

    if (s_pending_len != 0u && (now - s_pending_tick) >= NET_TCP_RTO_MS)
    {
        if (++s_retries > NET_TCP_MAX_RETRIES)
        {
            s_state       = NET_TCP_CLOSED;
            s_pending_len = 0u;
            return;
        }
        (void)emit(TCP_PSH | TCP_ACK, s_pending_seq, s_pending, s_pending_len);
        s_pending_tick = now;
        s_retransmits++;
    }
}

/* Payload from the server is acknowledged and dropped; it never carries audio. */
void net_tcp_input(uint32_t src_ip, const uint8_t *seg, uint32_t len)
{
    uint8_t  flags;
    uint32_t seq;
    uint32_t ack;
    uint32_t data_off;
    uint32_t data_len;

    if (len < TCP_HDR || src_ip != s_peer_ip || s_state == NET_TCP_CLOSED)
    {
        return;
    }
    if (rd16(&seg[2]) != s_local_port || rd16(&seg[0]) != s_peer_port)
    {
        return;
    }

    flags    = seg[13];
    seq      = rd32(&seg[4]);
    ack      = rd32(&seg[8]);
    data_off = (uint32_t)(seg[12] >> 4) * 4u;
    data_len = (len > data_off) ? (len - data_off) : 0u;

    if ((flags & TCP_RST) != 0u)
    {
        s_state       = NET_TCP_CLOSED;
        s_pending_len = 0u;
        s_resets++;
        return;
    }

    if (s_state == NET_TCP_CONNECTING)
    {
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK))
        {
            s_rcv_nxt = seq + 1u;
            s_snd_una = ack;
            s_state   = NET_TCP_ESTABLISHED;
            (void)emit(TCP_ACK, s_snd_nxt, 0, 0u);
        }
        return;
    }

    if ((flags & TCP_ACK) != 0u)
    {
        s_snd_una = ack;
        if (s_pending_len != 0u &&
            (int32_t)(ack - (s_pending_seq + s_pending_len)) >= 0)
        {
            s_pending_len = 0u;
            s_retries     = 0u;
        }
    }

    if (data_len != 0u && seq == s_rcv_nxt)
    {
        s_rcv_nxt += data_len;
        (void)emit(TCP_ACK, s_snd_nxt, 0, 0u);
    }

    if ((flags & TCP_FIN) != 0u)
    {
        s_rcv_nxt++;
        (void)emit(TCP_ACK, s_snd_nxt, 0, 0u);
        s_state = NET_TCP_CLOSED;
    }
}

uint32_t net_tcp_bytes_sent(void)
{
    return s_bytes_sent;
}

uint32_t net_tcp_retransmits(void)
{
    return s_retransmits;
}

uint32_t net_tcp_resets(void)
{
    return s_resets;
}
