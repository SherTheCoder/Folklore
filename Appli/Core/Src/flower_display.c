#include "main.h"
#include "flower_display.h"
#include "flower_config.h"
#include "flower_ctl.h"
#include <string.h>
#include <stdio.h>

#define RK_HSYNC   4u
#define RK_HBP     4u
#define RK_HFP     4u
#define RK_VSYNC   4u
#define RK_VBP     4u
#define RK_VFP     4u

#define LTDC_VBLANK_LINE  (RK_VSYNC + RK_VBP + FLOWER_PANEL_H)

#define LTDC_FRAME_US           (1000000u / 60u)
#define LTDC_RELOAD_TIMEOUT_US  (2u * LTDC_FRAME_US)

#define FLOWER_FB_SECTION      __attribute__((section(".flower_fb"), aligned(32)))
#define FLOWER_COMPOSE_SECTION __attribute__((section(".flower_compose"), aligned(32)))

static uint16_t s_fb[2][FLOWER_FB_W * FLOWER_FB_H]           FLOWER_FB_SECTION;
static uint16_t s_compose[FLOWER_COMPOSE_W * FLOWER_COMPOSE_H] FLOWER_COMPOSE_SECTION;

static LTDC_HandleTypeDef s_hltdc;
static volatile uint32_t  s_vblank_seen;
static uint8_t            s_front;
static uint8_t            s_visible;

static uint32_t s_cycles_per_us     = 600u;
static float    s_seconds_per_cycle = 1.0f / 600000000.0f;

void flower_clock_init(void)
{
    uint32_t cpu = HAL_RCC_GetCpuClockFreq();
    s_cycles_per_us = (cpu + 500000u) / 1000000u;
    if (s_cycles_per_us == 0u)
    {
        s_cycles_per_us = 1u;
    }
    s_seconds_per_cycle = 1.0f / (float)((cpu != 0u) ? cpu : 1u);

    DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}

uint32_t flower_cycles(void)
{
    return DWT->CYCCNT;
}

uint32_t flower_us_since(uint32_t start_cycles)
{
    return (DWT->CYCCNT - start_cycles) / s_cycles_per_us;
}

uint32_t flower_cycles_per_us(void)
{
    return s_cycles_per_us;
}

float flower_s_since(uint32_t start_cycles)
{
    return (float)(DWT->CYCCNT - start_cycles) * s_seconds_per_cycle;
}

#if FLOWER_ENABLE_DCACHE
/* Write-back, non-shareable: the M55 does not cache shareable memory. */
static void flower_display_mpu_config(void)
{
    extern uint32_t __sflower_atlas;
    extern uint32_t __eflower_atlas;
    extern uint32_t __sflower_fb;
    extern uint32_t __eflower_compose;

    MPU_Attributes_InitTypeDef attr   = {0};
    MPU_Region_InitTypeDef     region = {0};

    HAL_MPU_Disable();

    attr.Number     = MPU_ATTRIBUTES_NUMBER1;
#if FLOWER_GFX_WRITEBACK
    attr.Attributes = INNER_OUTER(MPU_WRITE_BACK | MPU_NON_TRANSIENT |
                                  MPU_R_ALLOCATE | MPU_W_ALLOCATE);
#else
    attr.Attributes = INNER_OUTER(MPU_WRITE_THROUGH | MPU_NON_TRANSIENT |
                                  MPU_R_ALLOCATE);
#endif
    HAL_MPU_ConfigMemoryAttributes(&attr);

    region.Enable           = MPU_REGION_ENABLE;
    region.Number           = MPU_REGION_NUMBER1;
    region.BaseAddress      = (uint32_t)&__sflower_atlas & ~0x1Fu;
    region.LimitAddress     = (((uint32_t)&__eflower_atlas + 31u) & ~0x1Fu) - 1u;
    region.AttributesIndex  = MPU_ATTRIBUTES_NUMBER1;
    region.AccessPermission = MPU_REGION_ALL_RW;
    region.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
#if FLOWER_GFX_WRITEBACK
    region.IsShareable      = MPU_ACCESS_NOT_SHAREABLE;
#else
    region.IsShareable      = MPU_ACCESS_OUTER_SHAREABLE;
#endif
    HAL_MPU_ConfigRegion(&region);

    region.Number           = MPU_REGION_NUMBER2;
    region.BaseAddress      = (uint32_t)&__sflower_fb & ~0x1Fu;
    region.LimitAddress     = (((uint32_t)&__eflower_compose + 31u) & ~0x1Fu) - 1u;
    HAL_MPU_ConfigRegion(&region);

    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}
#endif

/* 25 MHz pixel clock: PLL4 at 1600 MHz / 64. A slower PLL4 gives no clock. */
static int flower_display_clock_config(void)
{
    RCC_OscInitTypeDef       osc = {0};
    RCC_PeriphCLKInitTypeDef clk = {0};

    osc.OscillatorType     = RCC_OSCILLATORTYPE_NONE;
    osc.PLL1.PLLState      = RCC_PLL_NONE;
    osc.PLL2.PLLState      = RCC_PLL_NONE;
    osc.PLL3.PLLState      = RCC_PLL_NONE;
    osc.PLL4.PLLState      = RCC_PLL_ON;
    osc.PLL4.PLLSource     = RCC_PLLSOURCE_HSI;
    osc.PLL4.PLLM          = 1;
    osc.PLL4.PLLN          = 25;
    osc.PLL4.PLLFractional = 0;
    osc.PLL4.PLLP1         = 1;
    osc.PLL4.PLLP2         = 1;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK)
    {
        return -1;
    }

    clk.PeriphClockSelection                 = RCC_PERIPHCLK_LTDC;
    clk.LtdcClockSelection                   = RCC_LTDCCLKSOURCE_IC16;
    clk.ICSelection[RCC_IC16].ClockSelection = RCC_ICCLKSOURCE_PLL4;
    clk.ICSelection[RCC_IC16].ClockDivider   = 64;
    if (HAL_RCCEx_PeriphCLKConfig(&clk) != HAL_OK)
    {
        return -2;
    }
    return 0;
}

/* The boot ROM leaves LTDC and DMA2D firewalled, and then they draw nothing. */
static void flower_display_rif_config(void)
{
    RIMC_MasterConfig_t master = {0};
    RIMC_MasterConfig_t back   = {0};

    __HAL_RCC_RIFSC_CLK_ENABLE();
    __DSB();

#if FLOWER_CONFIG_RIF

    master.MasterCID = RIF_CID_1;
    master.SecPriv   = RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV;

    HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_LTDC1, &master);
    HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_LTDC2, &master);
    HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_DMA2D, &master);

    HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_LTDC,
                                          RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
    HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_LTDCL1,
                                          RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
    HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_DMA2D,
                                          RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
    __DSB();

    HAL_RIF_RIMC_GetConfigMasterAttributes(RIF_MASTER_INDEX_LTDC1, &back);
    printf("RIF: LTDC1 master CID=0x%08lX SecPriv=0x%08lX %s\r\n",
           (unsigned long)back.MasterCID, (unsigned long)back.SecPriv,
           ((back.MasterCID == RIF_CID_1) &&
            (back.SecPriv == (RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV)))
               ? "(applied)" : "*** NOT APPLIED ***");
    HAL_RIF_RIMC_GetConfigMasterAttributes(RIF_MASTER_INDEX_DMA2D, &back);
    printf("RIF: DMA2D master CID=0x%08lX SecPriv=0x%08lX\r\n",
           (unsigned long)back.MasterCID, (unsigned long)back.SecPriv);
#else
    (void)master; (void)back;
    printf("RIF: master configuration SKIPPED (FLOWER_CONFIG_RIF=0)\r\n");
#endif
}

static void flower_display_risaf_dump(void)
{
    static RISAF_TypeDef *const risafs[2] = { RISAF2, RISAF3 };
    static const char *const names[2]     = { "RISAF2/AXISRAM1", "RISAF3/AXISRAM2" };
    RISAF_BaseRegionConfig_t cfg;
    uint32_t i, r;

    for (i = 0u; i < 3u; i++)
    {
        for (r = 0u; r < 2u; r++)
        {
            memset(&cfg, 0, sizeof cfg);
            HAL_RIF_RISAF_GetConfigBaseRegion(risafs[i],
                                              (r == 0u) ? RISAF_REGION_1 : RISAF_REGION_2,
                                              &cfg);
            printf("  %s region%lu: filt=%lu sec=%lu rd=0x%02lX wr=0x%02lX "
                   "0x%08lX..0x%08lX\r\n",
                   names[i], (unsigned long)(r + 1u),
                   (unsigned long)cfg.Filtering, (unsigned long)cfg.Secure,
                   (unsigned long)cfg.ReadWhitelist, (unsigned long)cfg.WriteWhitelist,
                   (unsigned long)cfg.StartAddress, (unsigned long)cfg.EndAddress);
        }
    }
}

/* Two of the red lines are also JTAG pins; see FLOWER_KEEP_JTAG_PINS. */
static void flower_display_gpio_config(void)
{
    GPIO_InitTypeDef g = {0};
    int keep_jtag;

#if FLOWER_KEEP_JTAG_PINS == FLOWER_JTAG_AUTO
    keep_jtag = ((DCB->DHCSR & DCB_DHCSR_C_DEBUGEN_Msk) != 0u) ? 1 : 0;
#else
    keep_jtag = (FLOWER_KEEP_JTAG_PINS != 0) ? 1 : 0;
#endif

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
    __HAL_RCC_GPIOH_CLK_ENABLE();
    __HAL_RCC_GPIOQ_CLK_ENABLE();

    g.Mode      = GPIO_MODE_AF_PP;
    g.Pull      = GPIO_NOPULL;
    g.Speed     = GPIO_SPEED_FREQ_HIGH;
    g.Alternate = GPIO_AF14_LCD;

    if (keep_jtag == 0)
    {
        printf("LCD: PA15/PB4 taken for LCD R5/R3 (no debugger attached)\r\n");
    }
    else
    {
        printf("LCD: PA15/PB4 left as JTDI/NJTRST so the debug session "
               "survives; red channel loses bits 3 and 5\r\n");
    }

    g.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7 | GPIO_PIN_8;
    if (keep_jtag == 0) { g.Pin |= GPIO_PIN_15; }
    HAL_GPIO_Init(GPIOA, &g);

    g.Pin = GPIO_PIN_2 | GPIO_PIN_11 | GPIO_PIN_12 |
            GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
    if (keep_jtag == 0) { g.Pin |= GPIO_PIN_4; }
    HAL_GPIO_Init(GPIOB, &g);

    g.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_15;
    HAL_GPIO_Init(GPIOD, &g);

    g.Pin = GPIO_PIN_11;
    HAL_GPIO_Init(GPIOE, &g);

    g.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_6 | GPIO_PIN_8 |
            GPIO_PIN_11 | GPIO_PIN_12;
    HAL_GPIO_Init(GPIOG, &g);

    g.Pin = GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_6;
    HAL_GPIO_Init(GPIOH, &g);

    g.Mode      = GPIO_MODE_OUTPUT_PP;
    g.Alternate = 0;

    g.Pin = GPIO_PIN_1;
    HAL_GPIO_Init(GPIOE, &g);
    g.Pin = GPIO_PIN_13;
    HAL_GPIO_Init(GPIOG, &g);
    g.Pin = GPIO_PIN_3 | GPIO_PIN_6;
    HAL_GPIO_Init(GPIOQ, &g);

    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_1, GPIO_PIN_RESET);
    HAL_Delay(10);
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_1, GPIO_PIN_SET);
    HAL_Delay(10);

    HAL_GPIO_WritePin(GPIOQ, GPIO_PIN_3,  GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_13, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOQ, GPIO_PIN_6,  GPIO_PIN_SET);
}

/* Mask of the SRAM blocks that do not read back what was written. */
static uint32_t probe_blocks(uint32_t *failed_mask)
{
    extern uint32_t __sflower_atlas, __sflower_fb, __eflower_fb, __sflower_compose;
    extern uint32_t __eflower_compose;

    const struct { const char *name; uint32_t addr; } probes[] = {
        { "atlas       ", (uint32_t)&__sflower_atlas },
        { "fb[0]       ", (uint32_t)&__sflower_fb },
        { "fb[1] (mid) ", ((uint32_t)&__sflower_fb + (uint32_t)&__eflower_fb) / 2u },
        { "fb end      ", (uint32_t)&__eflower_fb - 4u },
        { "compose     ", (uint32_t)&__sflower_compose },
        { "compose end ", (uint32_t)&__eflower_compose - 4u },
    };
    uint32_t failed = 0u;
    uint32_t i;

    for (i = 0u; i < (sizeof probes / sizeof probes[0]); i++)
    {
        volatile uint32_t *p = (volatile uint32_t *)probes[i].addr;
        uint32_t a, b, save;

        printf("  %s 0x%08lX ... ", probes[i].name, (unsigned long)probes[i].addr);

        save = *p;
        *p = 0xA5A5A5A5u;  __DSB();  a = *p;
        *p = 0x5A5A5A5Au;  __DSB();  b = *p;
        *p = save;         __DSB();

        if ((a == 0xA5A5A5A5u) && (b == 0x5A5A5A5Au))
        {
            printf("ok\r\n");
        }
        else
        {
            printf("FAIL (read back %08lX then %08lX)\r\n",
                   (unsigned long)a, (unsigned long)b);
            failed |= (1u << i);
        }
    }
    *failed_mask = failed;
    return failed;
}

/* A sleeping SRAM block swallows writes without faulting, so probe it. */
uint32_t flower_display_memcheck(void)
{
    uint32_t failed = 0u;

    printf("\r\n=== SRAM probe (if this stops, the last line names the block) ===\r\n");
    printf("  RCC->MEMENR = 0x%08lX\r\n", (unsigned long)RCC->MEMENR);
    (void)probe_blocks(&failed);

    if (failed != 0u)
    {
        printf("\r\n  *** Some of the graphics arena does not answer. Nothing can\r\n"
               "      appear on the panel until it moves. ***\r\n");
    }
    return failed;
}

void flower_display_scan_test(void)
{
    uint32_t cdsr_seen = 0u, cdsr_changes = 0u;
    uint32_t x_min = 0xFFFFu, x_max = 0u, y_min = 0xFFFFu, y_max = 0u;
    uint32_t prev_cdsr = LTDC->CDSR;
    uint32_t start = flower_cycles();
    uint32_t n = 0u;

    while ((n < 400000u) && (flower_us_since(start) < 50000u))
    {
        uint32_t pos  = LTDC->CPSR;
        uint32_t cd   = LTDC->CDSR;
        uint32_t x    = (pos & LTDC_CPSR_CXPOS_Msk) >> LTDC_CPSR_CXPOS_Pos;
        uint32_t y    = (pos & LTDC_CPSR_CYPOS_Msk) >> LTDC_CPSR_CYPOS_Pos;

        if (x < x_min) { x_min = x; }
        if (x > x_max) { x_max = x; }
        if (y < y_min) { y_min = y; }
        if (y > y_max) { y_max = y; }

        cdsr_seen |= cd;
        if (cd != prev_cdsr) { cdsr_changes++; prev_cdsr = cd; }
        n++;
    }

    printf("LTDC scan test: %lu samples in %lu us\r\n",
           (unsigned long)n, (unsigned long)flower_us_since(start));
    printf("  CPSR X %lu..%lu   Y %lu..%lu   (expect X 0..%u, Y 0..%u if scanning)\r\n",
           (unsigned long)x_min, (unsigned long)x_max,
           (unsigned long)y_min, (unsigned long)y_max,
           (unsigned)(RK_HSYNC + RK_HBP + FLOWER_PANEL_W + RK_HFP),
           (unsigned)(RK_VSYNC + RK_VBP + FLOWER_PANEL_H + RK_VFP));
    printf("  CDSR changed %lu times, bits ever seen 0x%08lX\r\n",
           (unsigned long)cdsr_changes, (unsigned long)cdsr_seen);

    if ((x_min == x_max) && (y_min == y_max))
    {
        printf("  *** The raster position never moved: LTDC is enabled but NOT\r\n"
               "      scanning, despite a locked PLL4 and an enabled IC16. ***\r\n");
    }
    else
    {
        printf("  LTDC IS scanning. The signal exists, so a black panel is\r\n"
               "  downstream of here: pin mux, panel power or backlight.\r\n");
    }
}

void flower_display_clock_dump(void)
{
    printf("RCC: PLL4 ready=%lu  PLL4CFGR1=0x%08lX CFGR2=0x%08lX CFGR3=0x%08lX\r\n",
           (unsigned long)(LL_RCC_PLL4_IsReady() ? 1u : 0u),
           (unsigned long)RCC->PLL4CFGR1, (unsigned long)RCC->PLL4CFGR2,
           (unsigned long)RCC->PLL4CFGR3);
    printf("  IC16CFGR=0x%08lX  DIVENR=0x%08lX (IC16EN=%lu)  LTDC kernel=%lu Hz\r\n",
           (unsigned long)RCC->IC16CFGR, (unsigned long)RCC->DIVENR,
           (unsigned long)((RCC->DIVENR & RCC_DIVENR_IC16EN) ? 1u : 0u),
           (unsigned long)HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_LTDC));
}

void flower_display_pin_dump(void)
{
    static const struct { GPIO_TypeDef *port; const char *name; uint32_t pins; }
    afports[] = {
        { GPIOA, "A", 0x8187u },
        { GPIOB, "B", 0xF014u },
        { GPIOD, "D", 0x8300u },
        { GPIOE, "E", 0x0800u },
        { GPIOG, "G", 0x1943u },
        { GPIOH, "H", 0x0058u },
    };
    uint32_t i, p;

    printf("LCD pins (want mode=2 af=14):\r\n");
    for (i = 0u; i < (sizeof afports / sizeof afports[0]); i++)
    {
        uint32_t bad = 0u;
        for (p = 0u; p < 16u; p++)
        {
            if ((afports[i].pins & (1u << p)) == 0u) { continue; }
            if (((afports[i].port->MODER >> (p * 2u)) & 3u) != 2u) { bad |= (1u << p); }
            else if (((afports[i].port->AFR[p >> 3] >> ((p & 7u) * 4u)) & 0xFu) != 14u)
            { bad |= (1u << p); }
        }
        printf("  GPIO%s MODER=0x%08lX AFRL=0x%08lX AFRH=0x%08lX  %s\r\n",
               afports[i].name, (unsigned long)afports[i].port->MODER,
               (unsigned long)afports[i].port->AFR[0],
               (unsigned long)afports[i].port->AFR[1],
               bad ? "*** WRONG on pins 0x" : "ok");
        if (bad) { printf("      (mask 0x%04lX)\r\n", (unsigned long)bad); }
    }
    printf("  PE1 reset=%lu  PG13 DE=%lu  PQ3 panel=%lu  PQ6 backlight=%lu\r\n",
           (unsigned long)HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_1),
           (unsigned long)HAL_GPIO_ReadPin(GPIOG, GPIO_PIN_13),
           (unsigned long)HAL_GPIO_ReadPin(GPIOQ, GPIO_PIN_3),
           (unsigned long)HAL_GPIO_ReadPin(GPIOQ, GPIO_PIN_6));
}

void flower_display_dump(void)
{
    LTDC_Layer_TypeDef *l = LTDC_Layer1;

    printf("LTDC GCR=0x%08lX (LTDCEN=%lu) ISR=0x%08lX CDSR=0x%08lX\r\n",
           (unsigned long)LTDC->GCR,
           (unsigned long)((LTDC->GCR & LTDC_GCR_LTDCEN) ? 1u : 0u),
           (unsigned long)LTDC->ISR, (unsigned long)LTDC->CDSR);
    printf("  layer CR=0x%08lX (LEN=%lu) CFBAR=0x%08lX\r\n",
           (unsigned long)l->CR,
           (unsigned long)((l->CR & LTDC_LxCR_LEN) ? 1u : 0u),
           (unsigned long)l->CFBAR);
    printf("  WHPCR=0x%08lX WVPCR=0x%08lX PFCR=0x%08lX CFBLR=0x%08lX CFBLNR=0x%08lX\r\n\r\n",
           (unsigned long)l->WHPCR, (unsigned long)l->WVPCR,
           (unsigned long)l->PFCR, (unsigned long)l->CFBLR,
           (unsigned long)l->CFBLNR);
}

int flower_display_init(void)
{
    LTDC_LayerCfgTypeDef layer = {0};
    int rc;

    __HAL_RCC_NPU_CLK_ENABLE();
    __DSB();
    __HAL_RCC_AXISRAM3_MEM_CLK_ENABLE();
    __HAL_RCC_AXISRAM4_MEM_CLK_ENABLE();
    __HAL_RCC_AXISRAM5_MEM_CLK_ENABLE();
    __HAL_RCC_AXISRAM6_MEM_CLK_ENABLE();

    RCC->MEMENSR = RCC_MEMENSR_AXISRAM3ENS | RCC_MEMENSR_AXISRAM4ENS |
                   RCC_MEMENSR_AXISRAM5ENS | RCC_MEMENSR_AXISRAM6ENS |
                   RCC_MEMENSR_CACHEAXIRAMENS;
    RAMCFG_SRAM3_AXI->CR &= ~RAMCFG_CR_SRAMSD;
    RAMCFG_SRAM4_AXI->CR &= ~RAMCFG_CR_SRAMSD;
    RAMCFG_SRAM5_AXI->CR &= ~RAMCFG_CR_SRAMSD;
    RAMCFG_SRAM6_AXI->CR &= ~RAMCFG_CR_SRAMSD;
    __DSB();
    __DSB();

    flower_clock_init();

#if FLOWER_PROBE_AXISRAM
    {
        static const uint32_t bases[4] = { 0x34200000u, 0x34280000u,
                                           0x34300000u, 0x34380000u };
        uint32_t i;
        printf("AXISRAM3..6 probe (MEMENR=0x%08lX):\r\n",
               (unsigned long)RCC->MEMENR);
        for (i = 0u; i < 4u; i++)
        {
            volatile uint32_t *p = (volatile uint32_t *)bases[i];
            uint32_t back;
            p[0] = 0xA5A5A5A0u + i;
            p[1] = 0x5A5A5A50u + i;
            __DSB();
            back = p[0];
            printf("  0x%08lX wrote 0x%08lX read 0x%08lX  %s\r\n",
                   (unsigned long)bases[i], (unsigned long)(0xA5A5A5A0u + i),
                   (unsigned long)back,
                   (back == (0xA5A5A5A0u + i)) ? "OK" : "DEAD");
        }
    }
#endif

    rc = flower_display_clock_config();
    if (rc != 0)
    {
        return rc;
    }

    flower_display_rif_config();
    flower_display_risaf_dump();
    flower_display_gpio_config();

    __HAL_RCC_LTDC_CLK_ENABLE();
    __HAL_RCC_LTDC_FORCE_RESET();
    __HAL_RCC_LTDC_RELEASE_RESET();
    __HAL_RCC_DMA2D_CLK_ENABLE();
    __HAL_RCC_DMA2D_FORCE_RESET();
    __HAL_RCC_DMA2D_RELEASE_RESET();

    s_hltdc.Instance                = LTDC;
    s_hltdc.Init.HSPolarity         = LTDC_HSPOLARITY_AL;
    s_hltdc.Init.VSPolarity         = LTDC_VSPOLARITY_AL;
    s_hltdc.Init.DEPolarity         = LTDC_DEPOLARITY_AL;
    s_hltdc.Init.PCPolarity         = LTDC_PCPOLARITY_IPC;
    s_hltdc.Init.HorizontalSync     = RK_HSYNC - 1u;
    s_hltdc.Init.AccumulatedHBP     = RK_HSYNC + RK_HBP - 1u;
    s_hltdc.Init.AccumulatedActiveW = RK_HSYNC + RK_HBP + FLOWER_PANEL_W - 1u;
    s_hltdc.Init.TotalWidth         = RK_HSYNC + RK_HBP + FLOWER_PANEL_W + RK_HFP - 1u;
    s_hltdc.Init.VerticalSync       = RK_VSYNC - 1u;
    s_hltdc.Init.AccumulatedVBP     = RK_VSYNC + RK_VBP - 1u;
    s_hltdc.Init.AccumulatedActiveH = RK_VSYNC + RK_VBP + FLOWER_PANEL_H - 1u;
    s_hltdc.Init.TotalHeigh         = RK_VSYNC + RK_VBP + FLOWER_PANEL_H + RK_VFP - 1u;
    s_hltdc.Init.Backcolor.Red      = 0;
    s_hltdc.Init.Backcolor.Green    = 0;
    s_hltdc.Init.Backcolor.Blue     = 0;
    if (HAL_LTDC_Init(&s_hltdc) != HAL_OK)
    {
        return -3;
    }

    memset(s_fb, 0, sizeof(s_fb));
    memset(s_compose, 0, sizeof(s_compose));
    __DSB();

    s_front   = 0u;
    s_visible = 0u;

    layer.WindowX0        = FLOWER_LAYER_X;
    layer.WindowX1        = FLOWER_LAYER_X + FLOWER_FB_W;
    layer.WindowY0        = FLOWER_LAYER_Y;
    layer.WindowY1        = FLOWER_LAYER_Y + FLOWER_FB_H;
    layer.PixelFormat     = LTDC_PIXEL_FORMAT_RGB565;
    layer.Alpha           = 255;
    layer.Alpha0          = 0;
    layer.BlendingFactor1 = LTDC_BLENDING_FACTOR1_CA;
    layer.BlendingFactor2 = LTDC_BLENDING_FACTOR2_CA;
    layer.FBStartAdress   = (uint32_t)s_fb[0];
    layer.ImageWidth      = FLOWER_FB_W;
    layer.ImageHeight     = FLOWER_FB_H;
    layer.Backcolor.Red   = 0;
    layer.Backcolor.Green = 0;
    layer.Backcolor.Blue  = 0;
    if (HAL_LTDC_ConfigLayer(&s_hltdc, &layer, 0) != HAL_OK)
    {
        return -4;
    }

    __HAL_LTDC_LAYER_DISABLE(&s_hltdc, 0);
    __HAL_LTDC_RELOAD_IMMEDIATE_CONFIG(&s_hltdc);

#if FLOWER_ENABLE_DCACHE
    flower_display_mpu_config();
    SCB_EnableICache();
    SCB_EnableDCache();
    flower_display_mpu_dump();
#if FLOWER_MEMBENCH
    flower_display_membench();
#endif
#else
    SCB_EnableICache();
#endif

    return 0;
}

uint16_t *flower_display_back_buffer(void)
{
    return s_fb[s_front ^ 1u];
}

uint32_t flower_display_back_index(void)
{
    return (uint32_t)(s_front ^ 1u);
}

uint16_t *flower_display_compose_buffer(void)
{
    return s_compose;
}

void flower_display_irq(void)
{
    HAL_LTDC_IRQHandler(&s_hltdc);
}

void HAL_LTDC_LineEventCallback(LTDC_HandleTypeDef *hltdc)
{
    (void)hltdc;
    s_vblank_seen++;
    g_flower_vblanks = s_vblank_seen;

    LTDC->LIPCR = (uint32_t)LTDC_VBLANK_LINE;
    LTDC->IER  |= LTDC_IER_LIE;
}

#define LTDC_ACTIVE_Y0   (RK_VSYNC + RK_VBP)
#define LTDC_ACTIVE_Y1   (RK_VSYNC + RK_VBP + FLOWER_PANEL_H)

static uint32_t ltdc_line(void)
{
    return (LTDC->CPSR & LTDC_CPSR_CYPOS_Msk) >> LTDC_CPSR_CYPOS_Pos;
}

static int wait_for_vblank(uint32_t start_cycles)
{
    while ((ltdc_line() >= LTDC_ACTIVE_Y1 || ltdc_line() < LTDC_ACTIVE_Y0) &&
           (flower_us_since(start_cycles) < LTDC_RELOAD_TIMEOUT_US))
    {
    }
    while ((ltdc_line() < LTDC_ACTIVE_Y1) &&
           (flower_us_since(start_cycles) < LTDC_RELOAD_TIMEOUT_US))
    {
    }
    return (flower_us_since(start_cycles) < LTDC_RELOAD_TIMEOUT_US);
}

void flower_display_mpu_dump(void)
{
{
    uint32_t i;
    printf("MPU: CTRL=0x%08lX MAIR0=0x%08lX MAIR1=0x%08lX CCR=0x%08lX "
           "(DC=%lu IC=%lu)\r\n",
           (unsigned long)MPU->CTRL, (unsigned long)MPU->MAIR0,
           (unsigned long)MPU->MAIR1, (unsigned long)SCB->CCR,
           (unsigned long)((SCB->CCR >> 16) & 1u),
           (unsigned long)((SCB->CCR >> 17) & 1u));
    for (i = 0u; i < 3u; i++)
    {
        MPU->RNR = i;
        printf("  region%lu RBAR=0x%08lX RLAR=0x%08lX\r\n",
               (unsigned long)i, (unsigned long)MPU->RBAR,
               (unsigned long)MPU->RLAR);
    }
}
}

#if FLOWER_MEMBENCH
void flower_display_membench(void)
{
    volatile uint16_t *fb  = s_fb[0];
    volatile uint16_t *cmp = s_compose;
    const uint32_t     n   = 128u * 1024u;
    uint32_t t, us, i;
    uint32_t acc = 0u;

    t = flower_cycles();
    for (i = 0u; i < n; i++) { fb[i] = (uint16_t)i; }
    us = flower_us_since(t);
    printf("membench: seq strh  %lu px in %luus -> %lu cyc/px\r\n",
           (unsigned long)n, (unsigned long)us,
           (unsigned long)((uint64_t)us * flower_cycles_per_us() / n));

    t = flower_cycles();
    for (i = 0u; i < n; i++) { acc += cmp[i]; }
    us = flower_us_since(t);
    printf("membench: seq ldrh  %lu px in %luus -> %lu cyc/px (acc=%lu)\r\n",
           (unsigned long)n, (unsigned long)us,
           (unsigned long)((uint64_t)us * flower_cycles_per_us() / n),
           (unsigned long)acc);

    t = flower_cycles();
    for (i = 0u; i < n; i++) { acc += cmp[(i * FLOWER_COMPOSE_W) & 0x1FFFFu]; }
    us = flower_us_since(t);
    printf("membench: strided   %lu px in %luus -> %lu cyc/px\r\n",
           (unsigned long)n, (unsigned long)us,
           (unsigned long)((uint64_t)us * flower_cycles_per_us() / n));

    t = flower_cycles();
    for (i = 0u; i < n; i++) { fb[i] = (uint16_t)(cmp[i] + 1u); }
    us = flower_us_since(t);
    printf("membench: copy      %lu px in %luus -> %lu cyc/px\r\n",
           (unsigned long)n, (unsigned long)us,
           (unsigned long)((uint64_t)us * flower_cycles_per_us() / n));

    memset((void *)fb, 0, FLOWER_FB_BYTES);
}
#endif

void flower_display_present(void)
{
    uint32_t start = flower_cycles();

#if FLOWER_ENABLE_DCACHE
    SCB_CleanDCache_by_Addr((void *)s_fb[s_front ^ 1u], (int32_t)FLOWER_FB_BYTES);
#endif

    HAL_LTDC_SetAddress_NoReload(&s_hltdc, (uint32_t)s_fb[s_front ^ 1u], 0);

    __DSB();

    (void)wait_for_vblank(start);

    __HAL_LTDC_VERTICAL_BLANKING_RELOAD_CONFIG(&s_hltdc);

    while (((s_hltdc.Instance->SRCR & LTDC_SRCR_VBR) != 0u) &&
           (flower_us_since(start) < LTDC_RELOAD_TIMEOUT_US))
    {
    }

    s_front ^= 1u;
}

void flower_display_set_visible(int visible)
{
    if (visible == (int)s_visible)
    {
        return;
    }
    s_visible = visible ? 1u : 0u;

    if (visible)
    {
        __HAL_LTDC_LAYER_ENABLE(&s_hltdc, 0);
    }
    else
    {
        __HAL_LTDC_LAYER_DISABLE(&s_hltdc, 0);
    }
    __HAL_LTDC_VERTICAL_BLANKING_RELOAD_CONFIG(&s_hltdc);
}

int flower_display_is_visible(void)
{
    return (int)s_visible;
}

void flower_display_blank(void)
{
    memset(s_fb, 0, sizeof(s_fb));
    __DSB();
}
