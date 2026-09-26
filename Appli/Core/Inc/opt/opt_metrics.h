#ifndef OPT_METRICS_H
#define OPT_METRICS_H

#include <stdint.h>

void opt_metrics_report_budget(void);

void opt_metrics_report_live(void);

void opt_metrics_report_handoff(void);

uint32_t opt_metrics_reserved_bytes(void);

#endif /* OPT_METRICS_H */
