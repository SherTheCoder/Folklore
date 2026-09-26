#ifndef NET_ETH_H
#define NET_ETH_H

#include <stdint.h>

#define NET_ETH_ADDR_LEN   6u

typedef enum
{
    NET_LINK_DOWN = 0,
    NET_LINK_10,
    NET_LINK_100,
    NET_LINK_1000
} net_link_t;

int        net_eth_init(const uint8_t mac[NET_ETH_ADDR_LEN]);

net_link_t net_eth_poll_link(void);

net_link_t net_eth_link(void);

int        net_eth_send(const uint8_t *frame, uint32_t len);

uint32_t   net_eth_recv(const uint8_t **frame);

void       net_eth_recv_done(void);

const uint8_t *net_eth_mac(void);
uint32_t   net_eth_tx_frames(void);
uint32_t   net_eth_rx_frames(void);
uint32_t   net_eth_tx_drops(void);
uint32_t   net_eth_errors(void);

#endif /* NET_ETH_H */
