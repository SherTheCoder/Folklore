#ifndef STREAM_TXQ_H
#define STREAM_TXQ_H

#include <stdint.h>

void     stream_txq_bind(void *base, uint32_t bytes);

void     stream_txq_unbind(void);

int      stream_txq_push(const int16_t *samples, uint32_t count);

uint32_t stream_txq_peek(const int16_t **samples);

void     stream_txq_pop(void);

uint32_t stream_txq_depth(void);
uint32_t stream_txq_capacity(void);
uint32_t stream_txq_high_water(void);
uint32_t stream_txq_drops(void);

#endif /* STREAM_TXQ_H */
