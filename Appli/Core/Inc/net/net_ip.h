#ifndef NET_IP_H
#define NET_IP_H

#include <stdint.h>

#define NET_PROTO_ICMP   1u
#define NET_PROTO_TCP    6u

#define NET_IP4(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
     ((uint32_t)(c) << 8)  |  (uint32_t)(d))

void     net_ip_init(uint32_t local, uint32_t netmask, uint32_t gateway);

void     net_ip_poll(void);

int      net_ip_resolve(uint32_t ip, uint8_t mac[6]);

uint8_t *net_ip_tx_begin(uint32_t dst_ip, uint8_t proto);

int      net_ip_tx_commit(uint32_t payload_len);

uint32_t net_ip_tx_room(void);

uint16_t net_ip_checksum(const void *data, uint32_t len);
uint16_t net_ip_transport_checksum(uint32_t src, uint32_t dst, uint8_t proto,
                                   const void *data, uint32_t len);

uint32_t net_ip_local(void);
uint32_t net_ip_arp_requests(void);
uint32_t net_ip_rx_dropped(void);

#endif /* NET_IP_H */
