#include "main.h"
#include <string.h>
#include <stdio.h>

#include "flower_config.h"
#include "flower_ctl.h"
#include "flower_display.h"
#include "flower_render.h"
#include "flower_audio.h"
#include "flower_anim.h"
#include "app/kikkar_app.h"
#include "opt/opt_sleep.h"

#define LABS10(f)  ((long)(((f) < 0.0f ? -(f) : (f)) * 10.0f) % 10L)

__asm__(".global _printf_float");

DMA_HandleTypeDef handle_GPDMA1_Channel0;
DMA_HandleTypeDef handle_GPDMA1_Channel1;
I2C_HandleTypeDef hi2c2;
MDF_HandleTypeDef MdfHandle0;
MDF_FilterConfigTypeDef MdfFilterConfig0;
SAI_HandleTypeDef hsai_BlockA1;
UART_HandleTypeDef huart1;

#define MIC_HALF     48U
#define MIC_SAMPLES  (MIC_HALF * 2U)
#define SPK_SAMPLES  (MIC_HALF * 4U)

#define NONCACHEABLE __attribute__((section(".noncacheable")))
static uint32_t        mic_buf_store[MIC_SAMPLES] NONCACHEABLE;
static int16_t         spk_buf_store[SPK_SAMPLES] NONCACHEABLE;
static DMA_NodeTypeDef MDF_DmaNode_store          NONCACHEABLE;
static DMA_NodeTypeDef SAI_DmaNode_store          NONCACHEABLE;

uint32_t        * const mic_buf     = mic_buf_store;
int16_t         * const spk_buf     = spk_buf_store;
DMA_NodeTypeDef * const MDF_DmaNode = &MDF_DmaNode_store;
DMA_NodeTypeDef * const SAI_DmaNode = &SAI_DmaNode_store;

static DMA_QListTypeDef MDF_DmaQueue;
static DMA_QListTypeDef SAI_DmaQueue;

volatile uint32_t mic_half_count = 0;
volatile uint32_t mic_cplt_count = 0;
volatile uint32_t sai_half_count = 0;
volatile uint32_t sai_cplt_count = 0;

#define RING_FRAMES   512U
#define RING_MASK     (RING_FRAMES - 1U)
#define RING_PRIME    (6U * MIC_HALF)
static int16_t           pcm_ring[RING_FRAMES * 2U];
static volatile uint32_t ring_w = 0;
static volatile uint32_t ring_r = 0;

static void MPU_Config(void);
static void MX_GPIO_Init(void);
static void MX_GPDMA1_Init(void);
static void MX_MDF1_Init(void);
static void MX_SAI1_Init(void);
static void MX_I2C2_Init(void);
static void MX_USART1_UART_Init(void);
static void WM8904_Init(void);
static void WM8904_OutputEnable(void);
static uint16_t WM8904_ReadReg(uint8_t reg);
static void Audio_DumpDiag(void);
static void Audio_Start(void);
static void Flower_Run(void);
#if FLOWER_SET_CPU_CLOCK
static void Flower_SystemClock_Config(void);
#endif
void Error_Handler(void);

#define OUTPUT_TEST_TONE   1

int _write(int file, char *ptr, int len)
{
    (void)file;
    HAL_UART_Transmit(&huart1, (uint8_t *)ptr, len, HAL_MAX_DELAY);
    return len;
}

/* Before HAL_Init, so the DMA buffers are never cached. */
static void MPU_Config(void)
{
    extern uint32_t __snoncacheable;
    extern uint32_t __enoncacheable;
    uint32_t base  = (uint32_t)&__snoncacheable;
    uint32_t limit = (uint32_t)&__enoncacheable;

    MPU_Attributes_InitTypeDef attr   = {0};
    MPU_Region_InitTypeDef     region = {0};

    HAL_MPU_Disable();

    attr.Number     = MPU_ATTRIBUTES_NUMBER0;
    attr.Attributes = INNER_OUTER(MPU_NOT_CACHEABLE);
    HAL_MPU_ConfigMemoryAttributes(&attr);

    if (limit > base)
    {
        region.Enable           = MPU_REGION_ENABLE;
        region.Number           = MPU_REGION_NUMBER0;
        region.BaseAddress      = base;
        region.LimitAddress     = limit - 1U;
        region.AttributesIndex  = MPU_ATTRIBUTES_NUMBER0;
        region.AccessPermission = MPU_REGION_ALL_RW;
        region.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
        region.IsShareable      = MPU_ACCESS_OUTER_SHAREABLE;
        HAL_MPU_ConfigRegion(&region);
    }

    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}
volatile uint32_t mdf_err_code = 0, mdf_err_count = 0;
volatile uint32_t sai_err_code = 0, sai_err_count = 0;

void HAL_MDF_ErrorCallback(MDF_HandleTypeDef *hmdf)
{
    mdf_err_code = hmdf->ErrorCode;
    mdf_err_count++;
}

void HAL_SAI_ErrorCallback(SAI_HandleTypeDef *hsai)
{
    sai_err_code = hsai->ErrorCode;
    sai_err_count++;
}
int main(void)
{
    flower_ctl_init();

    MPU_Config();
    HAL_Init();

#if FLOWER_SET_CPU_CLOCK
    Flower_SystemClock_Config();
#endif

    setvbuf(stdout, NULL, _IONBF, 0);

    RCC_OscInitTypeDef osc = {0};
    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState            = RCC_HSE_ON;
    osc.PLL1.PLLState       = RCC_PLL_NONE;
    osc.PLL2.PLLState       = RCC_PLL_NONE;
    osc.PLL3.PLLState       = RCC_PLL_ON;
    osc.PLL3.PLLSource      = RCC_PLLSOURCE_HSE;
    osc.PLL3.PLLM           = 6;
    osc.PLL3.PLLN           = 172;
    osc.PLL3.PLLFractional  = 0;
    osc.PLL3.PLLP1          = 7;
    osc.PLL3.PLLP2          = 4;
    osc.PLL4.PLLState       = RCC_PLL_NONE;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) { Error_Handler(); }

    RCC_PeriphCLKInitTypeDef clk = {0};
    clk.PeriphClockSelection                = RCC_PERIPHCLK_MDF1 | RCC_PERIPHCLK_SAI1;
    clk.Mdf1ClockSelection                  = RCC_MDF1CLKSOURCE_IC7;
    clk.Sai1ClockSelection                  = RCC_SAI1CLKSOURCE_IC7;
    clk.ICSelection[RCC_IC7].ClockSelection = RCC_ICCLKSOURCE_PLL3;
    clk.ICSelection[RCC_IC7].ClockDivider   = 1;
    if (HAL_RCCEx_PeriphCLKConfig(&clk) != HAL_OK) { Error_Handler(); }

    MX_GPIO_Init();
    MX_GPDMA1_Init();
    MX_USART1_UART_Init();

    printf("\r\n=== STM32N6 Audio Passthrough ===\r\n");
    printf("Clocks configured. Starting peripherals...\r\n");

    MX_MDF1_Init();
    MX_SAI1_Init();
    MX_I2C2_Init();
    WM8904_Init();

    printf("Peripherals initialised. Starting audio DMA...\r\n");

    printf("RISAF4[0]: CFGR=0x%08lX  CIDCFGR=0x%08lX  STARTR=0x%08lX  ENDR=0x%08lX\r\n",
           RISAF4_NS->REG[0].CFGR,
           RISAF4_NS->REG[0].CIDCFGR,
           RISAF4_NS->REG[0].STARTR,
           RISAF4_NS->REG[0].ENDR);

    {
        uint32_t saiker = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SAI1);
        uint32_t mdfker = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_MDF1);
        uint32_t mckdiv = hsai_BlockA1.Init.Mckdiv;
        printf("CLK CPU=%lu SAIker=%lu MDFker=%lu MCKDIV=%lu | MDF_fs=%lu SAI_fs=%lu\r\n",
               HAL_RCC_GetCpuClockFreq(), saiker, mdfker, mckdiv,
               mdfker / 1024UL,
               mckdiv ? saiker / (mckdiv * 256UL) : 0UL);
    }

    Audio_Start();

    WM8904_OutputEnable();

    Audio_DumpDiag();

#if OUTPUT_TEST_TONE
    printf(">>> OUTPUT_TEST_TONE active: SAI is playing a ~1 kHz tone.\r\n");
    printf(">>> If you hear it, the output path works (set OUTPUT_TEST_TONE 0 for mic passthrough).\r\n");
#endif
    printf("Audio running.\r\n");

    Flower_Run();
}

static void Flower_Report_Memory(void)
{
    extern uint32_t __sflower_atlas, __eflower_atlas;
    extern uint32_t __sflower_fb,    __eflower_fb;
    extern uint32_t __sflower_compose, __eflower_compose;
    extern uint32_t __gfx_lo_start, __gfx_lo_end;
    extern uint32_t __gfx_hi_start, __gfx_hi_end;

    uint32_t lo_used = (uint32_t)&__eflower_atlas   - (uint32_t)&__gfx_lo_start;
    uint32_t lo_cap  = (uint32_t)&__gfx_lo_end      - (uint32_t)&__gfx_lo_start;
    uint32_t hi_used = (uint32_t)&__eflower_compose - (uint32_t)&__gfx_hi_start;
    uint32_t hi_cap  = (uint32_t)&__gfx_hi_end      - (uint32_t)&__gfx_hi_start;

    printf("\r\n=== Flower assets in the memory map ===\r\n");
    printf("  atlas    %7lu B at 0x%08lX  (%u elements, %u textures, %u poses)\r\n",
           (unsigned long)((uint32_t)&__eflower_atlas - (uint32_t)&__sflower_atlas),
           (unsigned long)(uint32_t)&__sflower_atlas,
           (unsigned)FLOWER_ELEMENT_COUNT, (unsigned)FLOWER_TEXTURE_COUNT,
           (unsigned)FLOWER_POSE_COUNT);
    printf("  fb x2    %7lu B at 0x%08lX  (%ux%u RGB565, double buffered)\r\n",
           (unsigned long)((uint32_t)&__eflower_fb - (uint32_t)&__sflower_fb),
           (unsigned long)(uint32_t)&__sflower_fb,
           (unsigned)FLOWER_FB_W, (unsigned)FLOWER_FB_H);
    printf("  compose  %7lu B at 0x%08lX  (%ux%u ARGB4444, rotating subset)\r\n",
           (unsigned long)((uint32_t)&__eflower_compose - (uint32_t)&__sflower_compose),
           (unsigned long)(uint32_t)&__sflower_compose,
           (unsigned)FLOWER_COMPOSE_W, (unsigned)FLOWER_COMPOSE_H);
    printf("  GFX_LO   %7lu of %7lu B used (%lu spare)  [AXISRAM1/2, below the FSBL]\r\n",
           (unsigned long)lo_used, (unsigned long)lo_cap,
           (unsigned long)(lo_cap - lo_used));
    printf("  GFX_HI   %7lu of %7lu B used (%lu spare)  [AXISRAM3..6]\r\n",
           (unsigned long)hi_used, (unsigned long)hi_cap,
           (unsigned long)(hi_cap - hi_used));
    printf("  BUILD    " __DATE__ " " __TIME__ "\r\n\r\n");
}

#if FLOWER_SET_CPU_CLOCK
/* Same clocks as the FSBL: CPU at 600 MHz, buses at 400/200. */
static void Flower_SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    if (HAL_PWREx_ConfigSupply(PWR_EXTERNAL_SOURCE_SUPPLY) != HAL_OK) { Error_Handler(); }
    if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK) { Error_Handler(); }

    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState            = RCC_HSI_ON;
    osc.HSIDiv              = RCC_HSI_DIV1;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL1.PLLState       = RCC_PLL_NONE;
    osc.PLL2.PLLState       = RCC_PLL_NONE;
    osc.PLL3.PLLState       = RCC_PLL_NONE;
    osc.PLL4.PLLState       = RCC_PLL_NONE;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) { Error_Handler(); }

    HAL_RCC_GetClockConfig(&clk);
    if ((clk.CPUCLKSource == RCC_CPUCLKSOURCE_IC1) ||
        (clk.SYSCLKSource == RCC_SYSCLKSOURCE_IC2_IC6_IC11))
    {
        clk.ClockType     = RCC_CLOCKTYPE_CPUCLK | RCC_CLOCKTYPE_SYSCLK;
        clk.CPUCLKSource  = RCC_CPUCLKSOURCE_HSI;
        clk.SYSCLKSource  = RCC_SYSCLKSOURCE_HSI;
        if (HAL_RCC_ClockConfig(&clk) != HAL_OK) { Error_Handler(); }
    }

    osc.OscillatorType = RCC_OSCILLATORTYPE_NONE;
    osc.PLL1.PLLState  = RCC_PLL_ON;
    osc.PLL1.PLLSource = RCC_PLLSOURCE_HSI;
    osc.PLL1.PLLM      = 4;
    osc.PLL1.PLLN      = 75;
    osc.PLL1.PLLFractional = 0;
    osc.PLL1.PLLP1     = 1;
    osc.PLL1.PLLP2     = 1;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) { Error_Handler(); }

    clk.ClockType      = RCC_CLOCKTYPE_CPUCLK | RCC_CLOCKTYPE_HCLK |
                         RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 |
                         RCC_CLOCKTYPE_PCLK2  | RCC_CLOCKTYPE_PCLK5 |
                         RCC_CLOCKTYPE_PCLK4;
    clk.CPUCLKSource   = RCC_CPUCLKSOURCE_IC1;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_IC2_IC6_IC11;
    clk.AHBCLKDivider  = RCC_HCLK_DIV2;
    clk.APB1CLKDivider = RCC_APB1_DIV1;
    clk.APB2CLKDivider = RCC_APB2_DIV1;
    clk.APB4CLKDivider = RCC_APB4_DIV1;
    clk.APB5CLKDivider = RCC_APB5_DIV1;
    clk.IC1Selection.ClockSelection  = RCC_ICCLKSOURCE_PLL1;
    clk.IC1Selection.ClockDivider    = 2;
    clk.IC2Selection.ClockSelection  = RCC_ICCLKSOURCE_PLL1;
    clk.IC2Selection.ClockDivider    = 3;
    clk.IC6Selection.ClockSelection  = RCC_ICCLKSOURCE_PLL1;
    clk.IC6Selection.ClockDivider    = 4;
    clk.IC11Selection.ClockSelection = RCC_ICCLKSOURCE_PLL1;
    clk.IC11Selection.ClockDivider   = 3;
    if (HAL_RCC_ClockConfig(&clk) != HAL_OK) { Error_Handler(); }
}
#endif

static void Flower_Run(void)
{
    uint32_t prev_cyc;
    uint32_t last_telemetry = 0;
    int      was_dormant = 1;
    int      rc;

    Flower_Report_Memory();

    rc = flower_display_init();
    (void)flower_display_memcheck();
    flower_display_clock_dump();
    flower_display_pin_dump();
    flower_display_dump();
    if (rc != 0)
    {
        printf("flower_display_init FAILED (%d) — audio continues, screen stays dark\r\n", rc);
        while (1) { HAL_Delay(1000); }
    }
    printf("LTDC up: %ux%u panel, layer %ux%u at (%u,%u), pixel clock %lu Hz\r\n",
           (unsigned)FLOWER_PANEL_W, (unsigned)FLOWER_PANEL_H,
           (unsigned)FLOWER_FB_W, (unsigned)FLOWER_FB_H,
           (unsigned)FLOWER_LAYER_X, (unsigned)FLOWER_LAYER_Y,
           HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_LTDC));
    printf("  art scaled to %u%% of the manifest's 800x480; flower %ux%u\r\n",
           (unsigned)FLOWER_ART_SCALE_PCT,
           (unsigned)(FLOWER_BBOX_X1 - FLOWER_BBOX_X0),
           (unsigned)(FLOWER_BBOX_Y1 - FLOWER_BBOX_Y0));

    if (flower_render_init() != 0)
    {
        printf("flower_render_init FAILED\r\n");
        while (1) { HAL_Delay(1000); }
    }
    flower_audio_init();
    flower_anim_init();

    (void)kikkar_app_init();

    prev_cyc = flower_cycles();

    while (1)
    {
        uint32_t now_cyc = flower_cycles();
        float    dt      = flower_s_since(prev_cyc);
        uint32_t bloom;
        uint32_t compose_us = 0u, warp_us = 0u;

        prev_cyc = now_cyc;

        kikkar_app_tick();

        bloom = kikkar_app_is_blooming();
        flower_anim_tick(dt, bloom, flower_audio_energy());

        if (flower_anim_is_dormant())
        {
            if (!was_dormant)
            {
                flower_display_set_visible(0);
                flower_display_blank();
                flower_render_reset();
                was_dormant = 1;
            }
            {
                uint32_t until = HAL_GetTick() + FLOWER_DORMANT_POLL_MS;
                while (HAL_GetTick() < until)
                {
                    kikkar_app_tick();
                    opt_sleep_idle();
                }
            }
        }
        else
        {
            if (was_dormant)
            {
                flower_render_reset();
                was_dormant = 0;
            }

            flower_render_frame(flower_anim_draws(),
                                flower_anim_scale(),
                                flower_anim_flower_scale(),
                                flower_anim_rot_deg(),
                                &compose_us, &warp_us);
            flower_display_present();

            if (!flower_display_is_visible())
            {
                flower_display_set_visible(1);
            }
        }

        g_flower_compose_us = compose_us;
        g_flower_warp_us    = warp_us;
        g_flower_frame_us   = flower_us_since(now_cyc);
        if (dt > 1e-6f)
        {
            static int first = 1;
            float inst = 1.0f / dt;
            if (first)          { first = 0; }
            else if (g_flower_fps <= 0.0f) { g_flower_fps = inst; }
            else                { g_flower_fps += (inst - g_flower_fps) * 0.1f; }
        }

#if FLOWER_TELEMETRY_MS
        if ((HAL_GetTick() - last_telemetry) >= FLOWER_TELEMETRY_MS)
        {
            last_telemetry = HAL_GetTick();
            printf("%s st=%lu pres=%ld%% | energy=%ld raw=%ld dB=%ld.%ld "
                   "flr=%ld.%ld | scale=%ld rot=%ld.%ld glow=%ld | "
                   "fps=%ld.%ld frame=%luus comp=%luus warp=%luus aud=%luus | "
                   "mic h=%lu c=%lu ovr=%lu vbl=%lu d2derr=%lu/0x%08lX\r\n",
                   kikkar_app_state_name(),
                   (unsigned long)g_flower_state,
                   (long)(g_flower_presence * 100.0f),
                   (long)(g_flower_energy * 1000.0f),
                   (long)(g_flower_energy_raw * 1000.0f),
                   (long)g_flower_rms_dbfs,   (long)LABS10(g_flower_rms_dbfs),
                   (long)g_flower_floor_dbfs, (long)LABS10(g_flower_floor_dbfs),
                   (long)(g_flower_scale * 1000.0f),
                   (long)g_flower_rot_deg,    (long)LABS10(g_flower_rot_deg),
                   (long)(g_flower_glow_alpha * 1000.0f),
                   (long)g_flower_fps,        (long)LABS10(g_flower_fps),
                   (unsigned long)g_flower_frame_us,
                   (unsigned long)g_flower_compose_us,
                   (unsigned long)g_flower_warp_us,
                   (unsigned long)g_flower_audio_us,
                   (unsigned long)mic_half_count, (unsigned long)mic_cplt_count,
                   (unsigned long)g_flower_hop_overrun,
                   (unsigned long)g_flower_vblanks,
                   (unsigned long)g_flower_dma2d_err,
                   (unsigned long)g_flower_dma2d_code);
        }
#else
        (void)last_telemetry;
#endif
    }
}

static void MX_GPDMA1_Init(void)
{
    __HAL_RCC_GPDMA1_CLK_ENABLE();
    HAL_NVIC_SetPriority(GPDMA1_Channel0_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel0_IRQn);
    HAL_NVIC_SetPriority(GPDMA1_Channel1_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel1_IRQn);
}

/* ST's BSP settings for this mic: 3.072 MHz PDM, SINC4, 48 kHz out. */
static void MX_MDF1_Init(void)
{
    MdfHandle0.Instance                                          = MDF1_Filter0;
    MdfHandle0.Init.CommonParam.InterleavedFilters               = 0;
    MdfHandle0.Init.CommonParam.ProcClockDivider                 = 2;
    MdfHandle0.Init.CommonParam.OutputClock.Activation           = ENABLE;
    MdfHandle0.Init.CommonParam.OutputClock.Pins                 = MDF_OUTPUT_CLOCK_0;
    MdfHandle0.Init.CommonParam.OutputClock.Divider              = 8;
    MdfHandle0.Init.CommonParam.OutputClock.Trigger.Activation   = DISABLE;
    MdfHandle0.Init.SerialInterface.Activation                   = ENABLE;
    MdfHandle0.Init.SerialInterface.Mode                         = MDF_SITF_NORMAL_SPI_MODE;
    MdfHandle0.Init.SerialInterface.ClockSource                  = MDF_SITF_CCK0_SOURCE;
    MdfHandle0.Init.SerialInterface.Threshold                    = 31;
    MdfHandle0.Init.FilterBistream                               = MDF_BITSTREAM0_FALLING;
    if (HAL_MDF_Init(&MdfHandle0) != HAL_OK)
    {
        printf("HAL_MDF_Init FAILED\r\n");
        Error_Handler();
    }

    MdfFilterConfig0.DataSource                     = MDF_DATA_SOURCE_BSMX;
    MdfFilterConfig0.Delay                          = 0;
    MdfFilterConfig0.CicMode                        = MDF_ONE_FILTER_SINC4;
    MdfFilterConfig0.DecimationRatio                = 16;
    MdfFilterConfig0.Offset                         = 0;
    MdfFilterConfig0.Gain                           = 15;
    MdfFilterConfig0.ReshapeFilter.Activation       = ENABLE;
    MdfFilterConfig0.ReshapeFilter.DecimationRatio  = MDF_RSF_DECIMATION_RATIO_4;
    MdfFilterConfig0.HighPassFilter.Activation      = ENABLE;
    MdfFilterConfig0.HighPassFilter.CutOffFrequency = MDF_HPF_CUTOFF_0_000625FPCM;
    MdfFilterConfig0.Integrator.Activation          = DISABLE;
    MdfFilterConfig0.SoundActivity.Activation       = DISABLE;
    MdfFilterConfig0.AcquisitionMode                = MDF_MODE_ASYNC_CONT;
    MdfFilterConfig0.FifoThreshold                  = MDF_FIFO_THRESHOLD_NOT_EMPTY;
    MdfFilterConfig0.DiscardSamples                 = 0;

    printf("MDF1 init OK  (CCK0=3.072MHz, PCM~48kHz, SINC4 decim=16 + reshape/4, gain=10)\r\n");
}

/* OutputDrive must be enabled, or the I2S lines float. */
static void MX_SAI1_Init(void)
{
    hsai_BlockA1.Instance            = SAI1_Block_A;
    hsai_BlockA1.Init.AudioMode      = SAI_MODEMASTER_TX;
    hsai_BlockA1.Init.Synchro        = SAI_ASYNCHRONOUS;
    hsai_BlockA1.Init.OutputDrive    = SAI_OUTPUTDRIVE_ENABLE;
    hsai_BlockA1.Init.NoDivider      = SAI_MASTERDIVIDER_ENABLE;
    hsai_BlockA1.Init.FIFOThreshold  = SAI_FIFOTHRESHOLD_EMPTY;
    hsai_BlockA1.Init.AudioFrequency = SAI_AUDIO_FREQUENCY_48K;
    hsai_BlockA1.Init.SynchroExt     = SAI_SYNCEXT_DISABLE;
    hsai_BlockA1.Init.MckOutput      = SAI_MCK_OUTPUT_ENABLE;
    hsai_BlockA1.Init.MonoStereoMode = SAI_STEREOMODE;
    hsai_BlockA1.Init.CompandingMode = SAI_NOCOMPANDING;
    hsai_BlockA1.Init.TriState       = SAI_OUTPUT_NOTRELEASED;
    if (HAL_SAI_InitProtocol(&hsai_BlockA1, SAI_I2S_STANDARD,
                              SAI_PROTOCOL_DATASIZE_16BIT, 2) != HAL_OK)
    {
        printf("HAL_SAI_InitProtocol FAILED\r\n");
        Error_Handler();
    }
    printf("SAI1 Block A init OK  (48kHz stereo I2S master TX)\r\n");
}

static void MX_I2C2_Init(void)
{
    hi2c2.Instance             = I2C2;
    hi2c2.Init.Timing          = 0x30C0EDFF;
    hi2c2.Init.OwnAddress1     = 0;
    hi2c2.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
    hi2c2.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c2.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c2.Init.NoStretchMode   = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(&hi2c2) != HAL_OK)
    {
        printf("HAL_I2C2_Init FAILED\r\n");
        Error_Handler();
    }
    HAL_I2CEx_ConfigAnalogFilter(&hi2c2, I2C_ANALOGFILTER_ENABLE);
    HAL_I2CEx_ConfigDigitalFilter(&hi2c2, 0);
    printf("I2C2 init OK  (WM8904 codec bus ready)\r\n");
}

#define WM8904_ADDR  0x34U

static void WM8904_WriteReg(uint8_t reg, uint16_t val)
{
    uint8_t buf[2] = { (uint8_t)(val >> 8), (uint8_t)(val & 0xFF) };
    HAL_StatusTypeDef r = HAL_I2C_Mem_Write(&hi2c2, WM8904_ADDR,
                                             reg, I2C_MEMADD_SIZE_8BIT,
                                             buf, 2, 100);
    if (r != HAL_OK)
        printf("  WM8904 reg 0x%02X write FAIL (%d)\r\n", reg, r);
}

static uint16_t WM8904_ReadReg(uint8_t reg)
{
    uint8_t buf[2] = { 0xFF, 0xFF };
    HAL_StatusTypeDef r = HAL_I2C_Mem_Read(&hi2c2, WM8904_ADDR,
                                           reg, I2C_MEMADD_SIZE_8BIT,
                                           buf, 2, 100);
    if (r != HAL_OK)
    {
        printf("  WM8904 reg 0x%02X READ FAIL (%d)\r\n", reg, r);
        return 0xFFFFU;
    }
    return (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
}

/* Supplies, clocks and the I2S format: enough for the SAI to run. */
static void WM8904_Init(void)
{
    printf("WM8904 init...\r\n");

    WM8904_WriteReg(0x00, 0x0000);

    WM8904_WriteReg(0x16, 0x0004);
    WM8904_WriteReg(0x04, 0x0018);
    WM8904_WriteReg(0x05, 0x0047);
    HAL_Delay(100);
    WM8904_WriteReg(0x05, 0x0043);
    WM8904_WriteReg(0x04, 0x0019);
    WM8904_WriteReg(0x06, 0x0001);

    WM8904_WriteReg(0x0C, 0x0000);
    WM8904_WriteReg(0x0E, 0x0003);
    WM8904_WriteReg(0x16, 0x0006);
    WM8904_WriteReg(0x12, 0x000E);
    WM8904_WriteReg(0x27, 0x0000);

    WM8904_WriteReg(0x79, 0x0014);
    WM8904_WriteReg(0x28, 0x01AF);
    WM8904_WriteReg(0x0A, 0x0001);
    WM8904_WriteReg(0x21, 0x0648);

    WM8904_WriteReg(0x18, 0x0010);
    WM8904_WriteReg(0x19, 0x0002);
    WM8904_WriteReg(0x15, 0x0C05);
    WM8904_WriteReg(0x2D, 0x0100);
    WM8904_WriteReg(0x2E, 0x0100);

    printf("WM8904 configured (clocks/DAC/I2S 16-bit/48kHz). HP enable deferred until MCLK live.\r\n");
}

/* Must run after the SAI starts: the charge pump and DC servo need MCLK. */
static void WM8904_OutputEnable(void)
{
    WM8904_WriteReg(0x62, 0x0001);
    WM8904_WriteReg(0x5A, 0x0011);
    WM8904_WriteReg(0x5A, 0x0033);
    WM8904_WriteReg(0x43, 0x0003);
    WM8904_WriteReg(0x44, 0x0030);
    HAL_Delay(300);
    WM8904_WriteReg(0x5A, 0x0077);
    WM8904_WriteReg(0x5A, 0x00FF);
    WM8904_WriteReg(0x68, 0x0001);

    WM8904_WriteReg(0x1E, 0x01C0);
    WM8904_WriteReg(0x1F, 0x01C0);

    WM8904_WriteReg(0x21, 0x0640);

    WM8904_WriteReg(0x39, 0x01F9);
    WM8904_WriteReg(0x3A, 0x01F9);
    printf("WM8904 output enabled  (headphone, 0 dB, MCLK live)\r\n");
}

static void Audio_DumpDiag(void)
{
    printf("WM8904 ID(R0)=0x%04X  (MUST be 0x8904)\r\n", WM8904_ReadReg(0x00));
    printf("  R05=0x%04X R0C=0x%04X R0E=0x%04X R12=0x%04X R16=0x%04X\r\n",
           WM8904_ReadReg(0x05), WM8904_ReadReg(0x0C), WM8904_ReadReg(0x0E),
           WM8904_ReadReg(0x12), WM8904_ReadReg(0x16));
    printf("  R14=0x%04X R15=0x%04X R18=0x%04X R19=0x%04X R21=0x%04X\r\n",
           WM8904_ReadReg(0x14), WM8904_ReadReg(0x15), WM8904_ReadReg(0x18),
           WM8904_ReadReg(0x19), WM8904_ReadReg(0x21));
    printf("  R5A=0x%04X R62=0x%04X R68=0x%04X R39=0x%04X R3A=0x%04X\r\n",
           WM8904_ReadReg(0x5A), WM8904_ReadReg(0x62), WM8904_ReadReg(0x68),
           WM8904_ReadReg(0x39), WM8904_ReadReg(0x3A));

    uint32_t cr1 = hsai_BlockA1.Instance->CR1;
    printf("SAI CR1=0x%08lX  SAIEN=%lu DMAEN=%lu MCKEN=%lu MCKDIV=%lu NODIV=%lu\r\n",
           cr1, (cr1 >> 16) & 1U, (cr1 >> 17) & 1U, (cr1 >> 27) & 1U,
           (cr1 >> 20) & 0x3FU, (cr1 >> 19) & 1U);

    printf("GPIOG MODER=0x%08lX AFRL=0x%08lX  (PG7 mode=%lu af=%lu  want 2/6)\r\n",
           GPIOG->MODER, GPIOG->AFR[0],
           (GPIOG->MODER >> 14) & 3U, (GPIOG->AFR[0] >> 28) & 0xFU);
    printf("GPIOB MODER=0x%08lX AFRL=0x%08lX  (PB0 af=%lu PB6 af=%lu PB7 af=%lu  want 6)\r\n",
           GPIOB->MODER, GPIOB->AFR[0],
           (GPIOB->AFR[0] >> 0) & 0xFU, (GPIOB->AFR[0] >> 24) & 0xFU, (GPIOB->AFR[0] >> 28) & 0xFU);
}

static void MX_USART1_UART_Init(void)
{
    huart1.Instance            = USART1;
    huart1.Init.BaudRate       = 921600;
    huart1.Init.WordLength     = UART_WORDLENGTH_8B;
    huart1.Init.StopBits       = UART_STOPBITS_1;
    huart1.Init.Parity         = UART_PARITY_NONE;
    huart1.Init.Mode           = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl      = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling   = UART_OVERSAMPLING_16;
    huart1.Init.ClockPrescaler = UART_PRESCALER_DIV1;
    huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    if (HAL_UART_Init(&huart1) != HAL_OK) { Error_Handler(); }
    HAL_UARTEx_DisableFifoMode(&huart1);
}

static void MX_GPIO_Init(void)
{
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
}

static inline int16_t mdf_to_pcm16(uint32_t raw)
{
    int32_t v = (int32_t)raw / 256;
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

/* Mono in, stereo out. Only ever called from the capture DMA interrupt. */
static void ring_push_block(const uint32_t *src)
{
    uint32_t w = ring_w;
    for (uint32_t i = 0; i < MIC_HALF; i++)
    {
        int16_t  s   = mdf_to_pcm16(src[i]);
        uint32_t idx = ((w + i) & RING_MASK) * 2U;
        pcm_ring[idx]      = s;
        pcm_ring[idx + 1U] = s;

        flower_audio_isr_push(s);
    }
    ring_w = w + MIC_HALF;

    flower_audio_isr_notify();
}

/* On underrun, plays silence without moving the read cursor. */
__attribute__((unused)) static void ring_pop_block(int16_t *dst)
{
    uint32_t r = ring_r;
    if ((ring_w - r) < MIC_HALF)
    {
        for (uint32_t i = 0; i < MIC_HALF * 2U; i++) { dst[i] = 0; }
        return;
    }
    for (uint32_t i = 0; i < MIC_HALF; i++)
    {
        uint32_t idx = ((r + i) & RING_MASK) * 2U;
        dst[2U * i]      = pcm_ring[idx];
        dst[2U * i + 1U] = pcm_ring[idx + 1U];
    }
    ring_r = r + MIC_HALF;
}

#if OUTPUT_TEST_TONE
static uint32_t tone_phase = 0;
static void fill_tone_block(int16_t *dst)
{
    for (uint32_t i = 0; i < MIC_HALF; i++)
    {
        int16_t s = ((tone_phase / 24U) & 1U) ? (int16_t)8000 : (int16_t)-8000;
        tone_phase++;
        dst[2U * i]      = s;
        dst[2U * i + 1U] = s;
    }
}
#endif

/* No circular mode on this GPDMA: each queue's last node links to its first. */
static void Audio_Start(void)
{
    HAL_StatusTypeDef   r;
    DMA_NodeConfTypeDef nodeCfg;

    memset(MDF_DmaNode, 0, sizeof(DMA_NodeTypeDef));
    memset(SAI_DmaNode, 0, sizeof(DMA_NodeTypeDef));

    memset(&nodeCfg, 0, sizeof(nodeCfg));
    nodeCfg.NodeType                            = DMA_GPDMA_LINEAR_NODE;
    nodeCfg.Init.Request                        = GPDMA1_REQUEST_MDF1_FLT0;
    nodeCfg.Init.BlkHWRequest                   = DMA_BREQ_SINGLE_BURST;
    nodeCfg.Init.Direction                      = DMA_PERIPH_TO_MEMORY;
    nodeCfg.Init.SrcInc                         = DMA_SINC_FIXED;
    nodeCfg.Init.DestInc                        = DMA_DINC_INCREMENTED;
    nodeCfg.Init.SrcDataWidth                   = DMA_SRC_DATAWIDTH_WORD;
    nodeCfg.Init.DestDataWidth                  = DMA_DEST_DATAWIDTH_WORD;
    nodeCfg.Init.SrcBurstLength                 = 1;
    nodeCfg.Init.DestBurstLength                = 1;
    nodeCfg.Init.Priority                       = DMA_HIGH_PRIORITY;
    nodeCfg.Init.TransferEventMode              = DMA_TCEM_BLOCK_TRANSFER;
    nodeCfg.Init.TransferAllocatedPort          = DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT1;
    nodeCfg.DataHandlingConfig.DataExchange     = DMA_EXCHANGE_NONE;
    nodeCfg.DataHandlingConfig.DataAlignment    = DMA_DATA_RIGHTALIGN_ZEROPADDED;
    nodeCfg.TriggerConfig.TriggerPolarity       = DMA_TRIG_POLARITY_MASKED;
    nodeCfg.SrcSecure                               = DMA_CHANNEL_SRC_SEC;
    nodeCfg.DestSecure                              = DMA_CHANNEL_DEST_SEC;

    r = HAL_DMAEx_List_BuildNode(&nodeCfg, MDF_DmaNode);
    printf("CH0 BuildNode:  %d\r\n", r);
    r = HAL_DMAEx_List_InsertNode_Tail(&MDF_DmaQueue, MDF_DmaNode);
    printf("CH0 Insert:     %d\r\n", r);
    r = HAL_DMAEx_List_SetCircularMode(&MDF_DmaQueue);
    printf("CH0 Circular:   %d\r\n", r);

    handle_GPDMA1_Channel0.Instance                         = GPDMA1_Channel0;
    handle_GPDMA1_Channel0.InitLinkedList.Priority          = DMA_HIGH_PRIORITY;
    handle_GPDMA1_Channel0.InitLinkedList.LinkStepMode      = DMA_LSM_FULL_EXECUTION;
    handle_GPDMA1_Channel0.InitLinkedList.LinkAllocatedPort = DMA_LINK_ALLOCATED_PORT1;
    handle_GPDMA1_Channel0.InitLinkedList.TransferEventMode = DMA_TCEM_LAST_LL_ITEM_TRANSFER;
    handle_GPDMA1_Channel0.InitLinkedList.LinkedListMode    = DMA_LINKEDLIST_CIRCULAR;

    r = HAL_DMAEx_List_Init(&handle_GPDMA1_Channel0);
    printf("CH0 LL init:    %d\r\n", r);
    r = HAL_DMA_ConfigChannelAttributes(&handle_GPDMA1_Channel0,
            DMA_CHANNEL_PRIV | DMA_CHANNEL_SEC |
            DMA_CHANNEL_SRC_SEC | DMA_CHANNEL_DEST_SEC);
    printf("CH0 SecAttr:    %d\r\n", r);
    r = HAL_DMAEx_List_LinkQ(&handle_GPDMA1_Channel0, &MDF_DmaQueue);
    printf("CH0 LinkQ:      %d\r\n", r);

    __HAL_LINKDMA(&MdfHandle0, hdma, handle_GPDMA1_Channel0);

    MDF_DmaConfigTypeDef mdf_cfg = {0};
    mdf_cfg.Address    = (uint32_t)mic_buf;
    mdf_cfg.DataLength = MIC_SAMPLES * sizeof(uint32_t);

    r = HAL_MDF_AcqStart_DMA(&MdfHandle0, &MdfFilterConfig0, &mdf_cfg);
    printf("MDF start:      %d  MDF_Err=0x%08lX  DMA_Err=0x%08lX\r\n",
           r, MdfHandle0.ErrorCode, handle_GPDMA1_Channel0.ErrorCode);

    memset(spk_buf, 0, SPK_SAMPLES * sizeof(int16_t));

    memset(&nodeCfg, 0, sizeof(nodeCfg));
    nodeCfg.NodeType                            = DMA_GPDMA_LINEAR_NODE;
    nodeCfg.Init.Request                        = GPDMA1_REQUEST_SAI1_A;
    nodeCfg.Init.BlkHWRequest                   = DMA_BREQ_SINGLE_BURST;
    nodeCfg.Init.Direction                      = DMA_MEMORY_TO_PERIPH;
    nodeCfg.Init.SrcInc                         = DMA_SINC_INCREMENTED;
    nodeCfg.Init.DestInc                        = DMA_DINC_FIXED;
    nodeCfg.Init.SrcDataWidth                   = DMA_SRC_DATAWIDTH_HALFWORD;
    nodeCfg.Init.DestDataWidth                  = DMA_DEST_DATAWIDTH_HALFWORD;
    nodeCfg.Init.SrcBurstLength                 = 1;
    nodeCfg.Init.DestBurstLength                = 1;
    nodeCfg.Init.Priority                       = DMA_HIGH_PRIORITY;
    nodeCfg.Init.TransferEventMode              = DMA_TCEM_BLOCK_TRANSFER;
    nodeCfg.Init.TransferAllocatedPort          = DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT1;
    nodeCfg.DataHandlingConfig.DataExchange     = DMA_EXCHANGE_NONE;
    nodeCfg.DataHandlingConfig.DataAlignment    = DMA_DATA_RIGHTALIGN_ZEROPADDED;
    nodeCfg.TriggerConfig.TriggerPolarity       = DMA_TRIG_POLARITY_MASKED;
    nodeCfg.SrcSecure                               = DMA_CHANNEL_SRC_SEC;
    nodeCfg.DestSecure                              = DMA_CHANNEL_DEST_SEC;

    r = HAL_DMAEx_List_BuildNode(&nodeCfg, SAI_DmaNode);
    printf("CH1 BuildNode:  %d\r\n", r);
    r = HAL_DMAEx_List_InsertNode_Tail(&SAI_DmaQueue, SAI_DmaNode);
    printf("CH1 Insert:     %d\r\n", r);
    r = HAL_DMAEx_List_SetCircularMode(&SAI_DmaQueue);
    printf("CH1 Circular:   %d\r\n", r);

    handle_GPDMA1_Channel1.Instance                         = GPDMA1_Channel1;
    handle_GPDMA1_Channel1.InitLinkedList.Priority          = DMA_HIGH_PRIORITY;
    handle_GPDMA1_Channel1.InitLinkedList.LinkStepMode      = DMA_LSM_FULL_EXECUTION;
    handle_GPDMA1_Channel1.InitLinkedList.LinkAllocatedPort = DMA_LINK_ALLOCATED_PORT1;
    handle_GPDMA1_Channel1.InitLinkedList.TransferEventMode = DMA_TCEM_LAST_LL_ITEM_TRANSFER;
    handle_GPDMA1_Channel1.InitLinkedList.LinkedListMode    = DMA_LINKEDLIST_CIRCULAR;

    r = HAL_DMAEx_List_Init(&handle_GPDMA1_Channel1);
    printf("CH1 LL init:    %d\r\n", r);
    r = HAL_DMA_ConfigChannelAttributes(&handle_GPDMA1_Channel1,
            DMA_CHANNEL_PRIV | DMA_CHANNEL_SEC |
            DMA_CHANNEL_SRC_SEC | DMA_CHANNEL_DEST_SEC);
    printf("CH1 SecAttr:    %d\r\n", r);
    r = HAL_DMAEx_List_LinkQ(&handle_GPDMA1_Channel1, &SAI_DmaQueue);
    printf("CH1 LinkQ:      %d\r\n", r);

    __HAL_LINKDMA(&hsai_BlockA1, hdmatx, handle_GPDMA1_Channel1);

#if OUTPUT_TEST_TONE
    fill_tone_block(&spk_buf[0]);
    fill_tone_block(&spk_buf[SPK_SAMPLES / 2]);
#else
    {
        uint32_t guard = 0;
        while (((ring_w - ring_r) < RING_PRIME) && (++guard < 2000000U)) { __NOP(); }
    }
    ring_pop_block(&spk_buf[0]);
    ring_pop_block(&spk_buf[SPK_SAMPLES / 2]);
#endif

    r = HAL_SAI_Transmit_DMA(&hsai_BlockA1, (uint8_t *)spk_buf, SPK_SAMPLES);
    printf("SAI start:      %d  SAI_Err=0x%08lX\r\n", r, hsai_BlockA1.ErrorCode);
}

void HAL_MDF_AcqHalfCpltCallback(MDF_HandleTypeDef *hmdf)
{
    (void)hmdf;
    mic_half_count++;
    ring_push_block(&mic_buf[0]);
}

void HAL_MDF_AcqCpltCallback(MDF_HandleTypeDef *hmdf)
{
    (void)hmdf;
    mic_cplt_count++;
    ring_push_block(&mic_buf[MIC_HALF]);
}

void HAL_SAI_TxHalfCpltCallback(SAI_HandleTypeDef *hsai)
{
    (void)hsai;
    sai_half_count++;
#if OUTPUT_TEST_TONE
    fill_tone_block(&spk_buf[0]);
#else
    ring_pop_block(&spk_buf[0]);
#endif
}

void HAL_SAI_TxCpltCallback(SAI_HandleTypeDef *hsai)
{
    (void)hsai;
    sai_cplt_count++;
#if OUTPUT_TEST_TONE
    fill_tone_block(&spk_buf[SPK_SAMPLES / 2]);
#else
    ring_pop_block(&spk_buf[SPK_SAMPLES / 2]);
#endif
}

void Error_Handler(void)
{
    __disable_irq();
    const char *msg = "\r\n*** ERROR_HANDLER — system halted ***\r\n";
    HAL_UART_Transmit(&huart1, (uint8_t *)msg, strlen(msg), 1000);
    while (1) {}
}
