#ifndef STREAM_SESSION_H
#define STREAM_SESSION_H

#include <stdint.h>

void     stream_session_init(void);

int      stream_session_begin(uint32_t trigger_pos, uint32_t trigger_cycles);

void     stream_session_pump(void);

void     stream_session_end(uint32_t reason);

int      stream_session_active(void);
uint32_t stream_session_utterance(void);

uint32_t stream_session_first_audio_us(void);

uint32_t stream_session_payload_bytes(void);
uint32_t stream_session_overhead_bytes(void);
uint32_t stream_session_samples_sent(void);
uint32_t stream_session_failed_handoffs(void);

#endif /* STREAM_SESSION_H */
