#ifndef OPT_LINK_H
#define OPT_LINK_H

#include <stdint.h>

void     opt_link_init(uint32_t server_ip, uint16_t port);
void     opt_link_task(void);
void     opt_link_prepare(void);
int      opt_link_ready(void);

uint32_t opt_link_connects(void);
uint32_t opt_link_prepares(void);
uint32_t opt_link_uptime_ms(void);

#endif /* OPT_LINK_H */
