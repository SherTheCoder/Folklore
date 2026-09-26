#ifndef OPT_GATE_H
#define OPT_GATE_H

#include <stdint.h>

void opt_gate_init(void);
int  opt_gate_is_open(void);
void opt_gate_force(int on);

uint32_t opt_gate_steps_run(void);
uint32_t opt_gate_steps_skipped(void);
uint32_t opt_gate_duty_pct(void);

#endif /* OPT_GATE_H */
