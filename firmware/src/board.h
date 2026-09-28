/* Board support: clocks, memory protection, safe pin states, status LEDs. */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>
#include <stdbool.h>

/* Core board LED (blue, active low) */
#define LED_BOARD_PORT GPIOC
#define LED_BOARD_PIN  GPIO_PIN_13

void     board_early_init(void);      /* first thing in main(), before HAL_Init() */
void     board_init(void);            /* call right after HAL_Init() */
uint32_t board_sysclk_hz(void);
uint32_t board_reset_cause(void);     /* RCC->RSR as it was at boot */
const char *board_reset_cause_str(void);

void board_led(bool on);
void board_leds_carrier(uint8_t bits); /* 5 LEDs on the carrier, bit0..bit4 */

/* DWT cycle counter for timing */
static inline uint32_t board_cycles(void) { return *(volatile uint32_t *)0xE0001004; }

void Error_Handler(void);

#endif
