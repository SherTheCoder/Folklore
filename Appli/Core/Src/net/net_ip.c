#include "net/net_ip.h"
#include "net/net_eth.h"
#include "net/net_tcp.h"
#include "app/kikkar_config.h"
#include <string.h>

#define ETHERTYPE_IP    0x0800u
#define ETHERTYPE_ARP   0x0806u
#define ARP_REQUEST     1u
#define ARP_REPLY       2u
#define ICMP_ECHO_REQ   8u
#define ICMP_ECHO_REP   0u
#define ETH_HDR_LEN     14u
#define IP_HDR_LEN      20u

static uint8_t  s_frame[NET_FRAME_MAX] __attribute__((aligned(4),
                                                      section(".kikkar_ram")));
static uint32_t s_local;
static uint32_t s_netmask;
static uint32_t s_gateway;
static uint32_t s_arp_ip;
static uint8_t  s_arp_mac[6];
static int      s_arp_valid;
static uint32_t s_arp_requests;
static uint32_t s_rx_dropped;
static uint32_t s_tx_dst;
static uint8_t  s_tx_proto;

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

void net_ip_init(uint32_t local, uint32_t netmask, uint32_t gateway)
{
    s_local        = local;
    s_netmask      = netmask;
    s_gateway      = gateway;
    s_arp_ip       = 0u;
    s_arp_valid    = 0;
    s_arp_requests = 0u;
    s_rx_dropped   = 0u;
}

uint16_t net_ip_checksum(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0u;

    while (len > 1u)
    {
        sum += rd16(p);
        p += 2;
        len -= 2u;
    }
    if (len != 0u)
    {
        sum += (uint32_t)p[0] << 8;
    }
    while ((sum >> 16) != 0u)
    {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)(~sum);
}

uint16_t net_ip_transport_checksum(uint32_t src, uint32_t dst, uint8_t proto,
                                   const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0u;
    uint32_t i;

    sum += (src >> 16) & 0xFFFFu;
    sum += src & 0xFFFFu;
    sum += (dst >> 16) & 0xFFFFu;
    sum += dst & 0xFFFFu;
    sum += proto;
    sum += len;

    for (i = 0u; (i + 1u) < len; i += 2u)
    {
        sum += rd16(&p[i]);
    }
    if ((len & 1u) != 0u)
    {
        sum += (uint32_t)p[len - 1u] << 8;
    }
    while ((sum >> 16) != 0u)
    {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)(~sum);
}

static void arp_request(uint32_t ip)
{
    uint8_t buf[42];
    const uint8_t *mac = net_eth_mac();

    memset(buf, 0xFF, 6);
    memcpy(&buf[6], mac, 6);
    wr16(&buf[12], ETHERTYPE_ARP);

    wr16(&buf[14], 1u);
    wr16(&buf[16], ETHERTYPE_IP);
    buf[18] = 6u;
    buf[19] = 4u;
    wr16(&buf[20], ARP_REQUEST);
    memcpy(&buf[22], mac, 6);
    wr32(&buf[28], s_local);
    memset(&buf[32], 0, 6);
    wr32(&buf[38], ip);

    (void)net_eth_send(buf, sizeof(buf));
    s_arp_requests++;
}

static void arp_input(const uint8_t *f, uint32_t len)
{
    uint16_t op;
    uint32_t sender_ip;
    uint32_t target_ip;

    if (len < 42u)
    {
        s_rx_dropped++;
        return;
    }
    op        = rd16(&f[20]);
    sender_ip = rd32(&f[28]);
    target_ip = rd32(&f[38]);

    if (sender_ip == s_arp_ip && s_arp_ip != 0u)
    {
        memcpy(s_arp_mac, &f[22], 6);
        s_arp_valid = 1;
    }

    if (op == ARP_REQUEST && target_ip == s_local)
    {
        uint8_t buf[42];
        const uint8_t *mac = net_eth_mac();

        memcpy(&buf[0], &f[22], 6);
        memcpy(&buf[6], mac, 6);
        wr16(&buf[12], ETHERTYPE_ARP);
        wr16(&buf[14], 1u);
        wr16(&buf[16], ETHERTYPE_IP);
        buf[18] = 6u;
        buf[19] = 4u;
        wr16(&buf[20], ARP_REPLY);
        memcpy(&buf[22], mac, 6);
        wr32(&buf[28], s_local);
        memcpy(&buf[32], &f[22], 6);
        wr32(&buf[38], sender_ip);
        (void)net_eth_send(buf, sizeof(buf));
    }
}

static void icmp_input(uint32_t src, const uint8_t *p, uint32_t len)
{
    uint8_t *out;

    if (len < 8u || p[0] != ICMP_ECHO_REQ || len > net_ip_tx_room())
    {
        return;
    }
    out = net_ip_tx_begin(src, NET_PROTO_ICMP);
    if (out == 0)
    {
        return;
    }
    memcpy(out, p, len);
    out[0] = ICMP_ECHO_REP;
    out[2] = 0u;
    out[3] = 0u;
    wr16(&out[2], net_ip_checksum(out, len));
    (void)net_ip_tx_commit(len);
}

static void ip_input(const uint8_t *f, uint32_t len)
{
    const uint8_t *ip = &f[ETH_HDR_LEN];
    uint32_t ihl;
    uint32_t total;
    uint32_t src;

    if (len < (ETH_HDR_LEN + IP_HDR_LEN))
    {
        s_rx_dropped++;
        return;
    }
    if ((ip[0] >> 4) != 4u)
    {
        s_rx_dropped++;
        return;
    }
    ihl   = (uint32_t)(ip[0] & 0x0Fu) * 4u;
    total = rd16(&ip[2]);
    src   = rd32(&ip[12]);

    if ((rd16(&ip[6]) & 0x3FFFu) != 0u)
    {
        s_rx_dropped++;
        return;
    }
    if (rd32(&ip[16]) != s_local)
    {
        return;
    }
    if (total < ihl || (ETH_HDR_LEN + total) > len)
    {
        s_rx_dropped++;
        return;
    }

    if (ip[9] == NET_PROTO_TCP)
    {
        net_tcp_input(src, &ip[ihl], total - ihl);
    }
    else if (ip[9] == NET_PROTO_ICMP)
    {
        icmp_input(src, &ip[ihl], total - ihl);
    }
}

void net_ip_poll(void)
{
    const uint8_t *f;
    uint32_t len;

    while ((len = net_eth_recv(&f)) != 0u)
    {
        uint16_t type = rd16(&f[12]);

        if (type == ETHERTYPE_ARP)
        {
            arp_input(f, len);
        }
        else if (type == ETHERTYPE_IP)
        {
            ip_input(f, len);
        }
        net_eth_recv_done();
    }
}

int net_ip_resolve(uint32_t ip, uint8_t mac[6])
{
    uint32_t hop = (((ip ^ s_local) & s_netmask) == 0u) ? ip : s_gateway;

    if (s_arp_valid && s_arp_ip == hop)
    {
        memcpy(mac, s_arp_mac, 6);
        return 0;
    }
    s_arp_ip    = hop;
    s_arp_valid = 0;
    arp_request(hop);
    return -1;
}

uint8_t *net_ip_tx_begin(uint32_t dst_ip, uint8_t proto)
{
    uint8_t mac[6];

    if (net_ip_resolve(dst_ip, mac) != 0)
    {
        return 0;
    }
    memcpy(&s_frame[0], mac, 6);
    memcpy(&s_frame[6], net_eth_mac(), 6);
    wr16(&s_frame[12], ETHERTYPE_IP);

    s_tx_dst   = dst_ip;
    s_tx_proto = proto;
    return &s_frame[ETH_HDR_LEN + IP_HDR_LEN];
}

int net_ip_tx_commit(uint32_t payload_len)
{
    uint8_t *ip = &s_frame[ETH_HDR_LEN];
    static uint16_t ident;

    if (payload_len > net_ip_tx_room())
    {
        return -1;
    }

    ip[0] = 0x45u;
    ip[1] = 0u;
    wr16(&ip[2], (uint16_t)(IP_HDR_LEN + payload_len));
    wr16(&ip[4], ++ident);
    wr16(&ip[6], 0x4000u);
    ip[8] = 64u;
    ip[9] = s_tx_proto;
    wr16(&ip[10], 0u);
    wr32(&ip[12], s_local);
    wr32(&ip[16], s_tx_dst);
    wr16(&ip[10], net_ip_checksum(ip, IP_HDR_LEN));

    return net_eth_send(s_frame, ETH_HDR_LEN + IP_HDR_LEN + payload_len);
}

uint32_t net_ip_tx_room(void)
{
    return NET_FRAME_MAX - ETH_HDR_LEN - IP_HDR_LEN;
}

uint32_t net_ip_local(void)
{
    return s_local;
}

uint32_t net_ip_arp_requests(void)
{
    return s_arp_requests;
}

uint32_t net_ip_rx_dropped(void)
{
    return s_rx_dropped;
}
