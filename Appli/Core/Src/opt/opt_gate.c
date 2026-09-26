#include "opt/opt_gate.h"
#include "app/kikkar_config.h"
#include "flower_ctl.h"
#include "stm32n6xx_hal.h"

static uint32_t s_open_tick;
static uint32_t s_ran;
static uint32_t s_skipped;
static int      s_forced;

void opt_gate_init(void)
{
    s_open_tick = 0u;
    s_ran       = 0u;
    s_skipped   = 0u;
    s_forced    = 0;
}

/* Open while the level is above the noise floor, plus the hang time. */
int opt_gate_is_open(void)
{
    uint32_t now = HAL_GetTick();
    int is_open;

#if OPT_GATED_INFERENCE
    if (g_flower_rms_dbfs > (g_flower_floor_dbfs + KWS_GATE_MARGIN_DB))
    {
        s_open_tick = now;
    }
    is_open = s_forced || ((now - s_open_tick) < KWS_GATE_HANG_MS);
#else
    (void)now;
    is_open = 1;
#endif

    if (is_open) { s_ran++; }
    else         { s_skipped++; }
    return is_open;
}

void opt_gate_force(int on)
{
    s_forced = on;
    if (on)
    {
        s_open_tick = HAL_GetTick();
    }
}

uint32_t opt_gate_steps_run(void)
{
    return s_ran;
}

uint32_t opt_gate_steps_skipped(void)
{
    return s_skipped;
}

uint32_t opt_gate_duty_pct(void)
{
    uint32_t total = s_ran + s_skipped;

    return (total == 0u) ? 0u : ((s_ran * 100u) / total);
}
