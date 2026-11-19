// fm1-stm32-unbrick - clocks, timers, LEDs and the CN5 VBUS switch on the STM32F4DISCOVERY.
#include "compat.h"

bool board_hse_ok;

// 8 MHz HSE -> PLL: SYSCLK 168 MHz, USB 48 MHz (PLLQ 7), APB1 42 MHz (TIM2 84 MHz), APB2 84 MHz.
// If the crystal does not start, 16 MHz HSI instead (same frequencies, USB timing less accurate; logged).
static void clock_init(void) {
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    PWR->CR |= PWR_CR_VOS;

    RCC->CR |= RCC_CR_HSEON;
    for (uint32_t i = 0; i < 2000000u && !(RCC->CR & RCC_CR_HSERDY); i++) {}
    board_hse_ok = RCC->CR & RCC_CR_HSERDY;

    RCC->PLLCFGR = ((board_hse_ok ? 8u : 16u) << RCC_PLLCFGR_PLLM_Pos) | (336u << RCC_PLLCFGR_PLLN_Pos) |
                   (0u << RCC_PLLCFGR_PLLP_Pos) /* /2 */ | (board_hse_ok ? RCC_PLLCFGR_PLLSRC_HSE : 0) |
                   (7u << RCC_PLLCFGR_PLLQ_Pos);
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) {}

    FLASH->ACR = FLASH_ACR_ICEN | FLASH_ACR_DCEN | FLASH_ACR_PRFTEN | FLASH_ACR_LATENCY_5WS;
    RCC->CFGR = RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV4 | RCC_CFGR_PPRE2_DIV2;
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {}

    SystemCoreClock = 168000000u;
}

static void timers_init(void) {
    // DWT cycle counter for microsecond busy-waits
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    // TIM2: 32-bit free-running at 1 MHz (84 MHz / 84)
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    TIM2->PSC = 83;
    TIM2->ARR = 0xFFFFFFFFu;
    TIM2->EGR = TIM_EGR_UG;
    TIM2->CR1 = TIM_CR1_CEN;
}

uint64_t time_us_64(void) {
    static uint32_t last, high;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    uint32_t now = TIM2->CNT;
    if (now < last) high++;
    last = now;
    uint64_t t = ((uint64_t)high << 32) | now;
    __set_PRIMASK(primask);
    return t;
}

void delay_us(uint32_t us) {
    uint32_t start = DWT->CYCCNT, cycles = us * 168u;
    while (DWT->CYCCNT - start < cycles) {}
}

// TinyUSB time base (OPT_OS_NONE)
uint32_t tusb_time_millis_api(void) {
    return (uint32_t)(time_us_64() / 1000u);
}

static void gpio_init(void) {
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOCEN | RCC_AHB1ENR_GPIODEN;
    __DSB();
    (void)RCC->AHB1ENR;
    // Errata ES0182 "delay after an RCC peripheral clock enabling": a GPIO write issued right after the enable
    // can be lost. The first build wrote PC0 high immediately and the write was dropped (PC0 read back 0,
    // CN5 powered, LD7 on). Wait, then write, then check and repeat.
    for (volatile int i = 0; i < 100; i++) {}

    // PC0 drives the STMPS2141 VBUS switch for CN5, active low. Held HIGH: the board never powers the FM-1
    // (it runs on its own battery, as with the ESP32-S3 and RP2040 tools). LD7 next to CN5 must stay dark.
    GPIOC->BSRR = 1u << 0;
    GPIOC->MODER = (GPIOC->MODER & ~(3u << 0)) | (1u << 0);
    for (int i = 0; i < 100 && !(GPIOC->ODR & 1u); i++) GPIOC->BSRR = 1u << 0;

    // LEDs PD12..PD15: outputs, off
    GPIOD->BSRR = (0xFu << 12) << 16;
    for (int p = 12; p <= 15; p++) GPIOD->MODER = (GPIOD->MODER & ~(3u << (p * 2))) | (1u << (p * 2));
}

void board_init(void) {
    clock_init();
    timers_init();
    gpio_init();
}
