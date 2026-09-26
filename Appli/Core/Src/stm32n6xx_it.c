/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32n6xx_it.c
  * @brief   Interrupt Service Routines.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

#include "main.h"
#include "stm32n6xx_it.h"
/* USER CODE BEGIN Includes */
#include "flower_audio.h"
#include "app/kikkar_app.h"
#include "flower_display.h"
#include <stdio.h>
/* USER CODE END Includes */

/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* USER CODE BEGIN 0 */
extern UART_HandleTypeDef huart1;

/* Re-arms the UART first, since the fault may have cut a transfer short. */
void Fault_Report(const char *name)
{
    uint32_t cfsr  = SCB->CFSR;
    uint32_t hfsr  = SCB->HFSR;
    uint32_t bfar  = SCB->BFAR;
    uint32_t mmfar = SCB->MMFAR;
    huart1.gState = HAL_UART_STATE_READY;
    huart1.Lock   = HAL_UNLOCKED;
    printf("\r\n*** %s  CFSR=0x%08lX HFSR=0x%08lX BFAR=0x%08lX MMFAR=0x%08lX ***\r\n",
           name, cfsr, hfsr, bfar, mmfar);
}
/* USER CODE END 0 */

extern DMA_HandleTypeDef handle_GPDMA1_Channel1;
extern DMA_HandleTypeDef handle_GPDMA1_Channel0;
/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
   while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
void HardFault_Handler(void)
{
  /* USER CODE BEGIN HardFault_IRQn 0 */
  {
    extern UART_HandleTypeDef huart1;
    uint32_t *sp;
    char buf[256];
    int n;

    __asm volatile ("tst lr, #4       \n"
                    "ite eq           \n"
                    "mrseq %0, msp    \n"
                    "mrsne %0, psp    \n" : "=r" (sp));

    n = snprintf(buf, sizeof buf,
                 "\r\n*** HARDFAULT ***\r\n"
                 "  HFSR=0x%08lX CFSR=0x%08lX MMFAR=0x%08lX BFAR=0x%08lX\r\n"
                 "  stacked PC=0x%08lX LR=0x%08lX\r\n"
                 "  (CFSR bit 15 = PRECISERR, bit 10 = IMPRECISERR; BFAR names the address)\r\n",
                 (unsigned long)SCB->HFSR, (unsigned long)SCB->CFSR,
                 (unsigned long)SCB->MMFAR, (unsigned long)SCB->BFAR,
                 (unsigned long)sp[6], (unsigned long)sp[5]);
    if (n > 0)
    {
      HAL_UART_Transmit(&huart1, (uint8_t *)buf, (uint16_t)n, 1000);
    }
  }
  /* USER CODE END HardFault_IRQn 0 */
  while (1)
  {
  }
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */
  Fault_Report("MemManage");
  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Prefetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */
  Fault_Report("BusFault");
  /* USER CODE END BusFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */
  Fault_Report("UsageFault");
  /* USER CODE END UsageFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_UsageFault_IRQn 0 */
    /* USER CODE END W1_UsageFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Secure fault.
  */
void SecureFault_Handler(void)
{
  /* USER CODE BEGIN SecureFault_IRQn 0 */
  huart1.gState = HAL_UART_STATE_READY;
  huart1.Lock   = HAL_UNLOCKED;
  printf("\r\n*** SecureFault  SFSR=0x%08lX SFAR=0x%08lX ***\r\n",
         (uint32_t)SAU->SFSR, (uint32_t)SAU->SFAR);
  /* USER CODE END SecureFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_SecureFault_IRQn 0 */
    /* USER CODE END W1_SecureFault_IRQn 0 */
  }
}

/**
  * @brief This function handles System service call via SWI instruction.
  */
void SVC_Handler(void)
{
  /* USER CODE BEGIN SVCall_IRQn 0 */

  /* USER CODE END SVCall_IRQn 0 */
  /* USER CODE BEGIN SVCall_IRQn 1 */

  /* USER CODE END SVCall_IRQn 1 */
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/**
  * @brief This function handles Pendable request for system service.
  */
void PendSV_Handler(void)
{
  /* USER CODE BEGIN PendSV_IRQn 0 */
  flower_audio_task();
  kikkar_app_audio_task();
  /* USER CODE END PendSV_IRQn 0 */
  /* USER CODE BEGIN PendSV_IRQn 1 */

  /* USER CODE END PendSV_IRQn 1 */
}

/**
  * @brief This function handles System tick timer.
  */
void SysTick_Handler(void)
{
  /* USER CODE BEGIN SysTick_IRQn 0 */

  /* USER CODE END SysTick_IRQn 0 */
  HAL_IncTick();
  /* USER CODE BEGIN SysTick_IRQn 1 */

  /* USER CODE END SysTick_IRQn 1 */
}

/******************************************************************************/
/******************************************************************************/

/* USER CODE BEGIN 1 */
extern DMA_HandleTypeDef handle_GPDMA1_Channel0;
extern DMA_HandleTypeDef handle_GPDMA1_Channel1;

void GPDMA1_Channel0_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&handle_GPDMA1_Channel0);
}

void GPDMA1_Channel1_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&handle_GPDMA1_Channel1);
}

/* Every LTDC vector needs a handler; the default one is a while (1). */
void LTDC_LO_IRQHandler(void)
{
    flower_display_irq();
}

void LTDC_LO_ERR_IRQHandler(void)
{
    flower_display_irq();
}

void LTDC_UP_IRQHandler(void)
{
    flower_display_irq();
}

void LTDC_UP_ERR_IRQHandler(void)
{
    flower_display_irq();
}
/* USER CODE END 1 */
