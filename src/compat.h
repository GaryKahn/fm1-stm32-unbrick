// fm1-stm32-unbrick - the few platform calls the ported FM-1 Transporter / fm1-esp32-unbrick sources use,
// on a bare-metal STM32F407 (STM32F4DISCOVERY, MB997C/D/E).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "stm32f4xx.h"

// CN5 (micro-USB "USB USER") is the STM32's own OTG FS port: PA12 = D+, PA11 = D-.
#define PIN_DP 12
#define PIN_DM 11

uint64_t time_us_64(void);          // TIM2 at 1 MHz, extended to 64 bits
void delay_us(uint32_t us);         // DWT cycle counter busy-wait (168 MHz)

static inline void sleep_us(uint32_t us) { delay_us(us); }
static inline void sleep_ms(uint32_t ms) { while (ms--) delay_us(1000); }
#define __dmb() __DMB()

// Status LEDs on the Discovery board (PD12 green, PD13 orange, PD14 red, PD15 blue)
enum { LED_GREEN = 12, LED_ORANGE = 13, LED_RED = 14, LED_BLUE = 15 };
static inline void led(int pin, bool on) { GPIOD->BSRR = on ? (1u << pin) : (1u << (pin + 16)); }
static inline void led_toggle(int pin) { GPIOD->ODR ^= (1u << pin); }

void board_init(void);
extern bool board_hse_ok;           // false: the 8 MHz crystal did not start, running from HSI
