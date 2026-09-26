#ifndef OPT_SLEEP_H
#define OPT_SLEEP_H

#include <stdint.h>

void opt_sleep_init(void);

void opt_sleep_note_busy(uint32_t cycles);

void opt_sleep_idle(void);

uint32_t opt_sleep_busy_permille(void);

uint32_t opt_sleep_peak_permille(void);

uint32_t opt_sleep_wfi_count(void);

#endif /* OPT_SLEEP_H */
