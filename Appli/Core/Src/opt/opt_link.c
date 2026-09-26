#include "opt/opt_link.h"
#include "net/net_eth.h"
#include "net/net_ip.h"
#include "net/net_tcp.h"
#include "app/kikkar_config.h"
#include "stm32n6xx_hal.h"

static uint32_t s_server_ip;
static uint16_t s_server_port;
static uint32_t s_last_link_poll;
static uint32_t s_last_attempt;
static uint32_t s_up_since;
static uint32_t s_connects;
static uint32_t s_prepares;

void opt_link_init(uint32_t server_ip, uint16_t port)
{
    s_server_ip      = server_ip;
    s_server_port    = port;
    s_last_link_poll = 0u;
    s_last_attempt   = 0u;
    s_up_since       = 0u;
    s_connects       = 0u;
    s_prepares       = 0u;
}

/* PHY poll, inbound packets, retransmit timer and reconnects. */
void opt_link_task(void)
{
    uint32_t now = HAL_GetTick();

    if ((now - s_last_link_poll) >= 200u)
    {
        s_last_link_poll = now;
        (void)net_eth_poll_link();
    }

    net_ip_poll();
    net_tcp_poll();

    if (net_eth_link() == NET_LINK_DOWN)
    {
        s_up_since = 0u;
        return;
    }

#if OPT_EARLY_LINK
    if (net_tcp_state() == NET_TCP_CLOSED &&
        (now - s_last_attempt) >= NET_RECONNECT_MS)
    {
        s_last_attempt = now;
        if (net_tcp_connect(s_server_ip, s_server_port) == 0)
        {
            s_connects++;
        }
    }
#endif

    if (net_tcp_state() == NET_TCP_ESTABLISHED && s_up_since == 0u)
    {
        s_up_since = now;
    }
    else if (net_tcp_state() != NET_TCP_ESTABLISHED)
    {
        s_up_since = 0u;
    }
}

/* Called when speech starts, so a dropped socket is back before the keyword. */
void opt_link_prepare(void)
{
    s_prepares++;

    if (net_tcp_state() == NET_TCP_CLOSED && net_eth_link() != NET_LINK_DOWN)
    {
        s_last_attempt = HAL_GetTick();
        if (net_tcp_connect(s_server_ip, s_server_port) == 0)
        {
            s_connects++;
        }
    }
}

int opt_link_ready(void)
{
    return (net_tcp_state() == NET_TCP_ESTABLISHED) ? 1 : 0;
}

uint32_t opt_link_connects(void)
{
    return s_connects;
}

uint32_t opt_link_prepares(void)
{
    return s_prepares;
}

uint32_t opt_link_uptime_ms(void)
{
    return (s_up_since == 0u) ? 0u : (HAL_GetTick() - s_up_since);
}
