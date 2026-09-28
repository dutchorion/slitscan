/* Core interrupt and fault handlers. Peripheral IRQ handlers live with their drivers. */
#include "stm32h7xx_hal.h"
#include "board.h"

void SysTick_Handler(void)
{
    HAL_IncTick();
}

void HardFault_Handler(void)
{
    Error_Handler();
}

void MemManage_Handler(void)
{
    Error_Handler();
}

void BusFault_Handler(void)
{
    Error_Handler();
}

void UsageFault_Handler(void)
{
    Error_Handler();
}
