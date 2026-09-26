#ifndef NET_TCP_H
#define NET_TCP_H

#include <stdint.h>

typedef enum
{
    NET_TCP_CLOSED = 0,
    NET_TCP_CONNECTING,
    NET_TCP_ESTABLISHED,
    NET_TCP_CLOSING
} net_tcp_state_t;

void            net_tcp_init(void);

int             net_tcp_connect(uint32_t ip, uint16_t port);

void            net_tcp_close(void);
net_tcp_state_t net_tcp_state(void);

uint32_t        net_tcp_send(const uint8_t *data, uint32_t len);

uint32_t        net_tcp_sendable(void);

void            net_tcp_poll(void);

void            net_tcp_input(uint32_t src_ip, const uint8_t *seg, uint32_t len);

uint32_t        net_tcp_bytes_sent(void);
uint32_t        net_tcp_retransmits(void);
uint32_t        net_tcp_resets(void);

#endif /* NET_TCP_H */
