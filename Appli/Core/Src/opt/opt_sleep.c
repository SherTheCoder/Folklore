#include "opt/opt_sleep.h"
#include "app/kikkar_config.h"
#include "flower_display.h"
#include "stm32n6xx_hal.h"

static uint32_t s_window_start;
static uint32_t s_busy;
static uint32_t s_permille;
static uint32_t s_peak;
static uint32_t s_wfi;

void opt_sleep_init(void)
{
    s_window_start = flower_cycles();
    s_busy         = 0u;
    s_permille     = 0u;
    s_peak         = 0u;
    s_wfi          = 0u;
}

void opt_sleep_note_busy(uint32_t cycles)
{
    uint32_t now = flower_cycles();
    uint32_t elapsed = now - s_window_start;
    uint32_t per_second = flower_cycles_per_us() * 1000000u;

    s_busy += cycles;

    if (per_second != 0u && elapsed >= per_second)
    {
        s_permille = (uint32_t)(((uint64_t)s_busy * 1000u) / elapsed);
        if (s_permille > s_peak)
        {
            s_peak = s_permille;
        }
        s_busy         = 0u;
        s_window_start = now;
    }
}

void opt_sleep_idle(void)
{
#if OPT_IDLE_SLEEP
    s_wfi++;
    __WFI();
#else
    __NOP();
#endif
}

uint32_t opt_sleep_busy_permille(void)
{
    return s_permille;
}

uint32_t opt_sleep_peak_permille(void)
{
    return s_peak;
}

uint32_t opt_sleep_wfi_count(void)
{
    return s_wfi;
}
