#ifndef KIKKAR_APP_H
#define KIKKAR_APP_H

#include <stdint.h>

typedef enum
{
    KIKKAR_BOOT = 0,
    KIKKAR_LISTENING,
    KIKKAR_STREAMING,
    KIKKAR_RECOVERING
} kikkar_state_t;

int  kikkar_app_init(void);

void kikkar_app_audio_task(void);

void kikkar_app_tick(void);

kikkar_state_t kikkar_app_state(void);
const char    *kikkar_app_state_name(void);

uint32_t       kikkar_app_is_blooming(void);

#endif /* KIKKAR_APP_H */
