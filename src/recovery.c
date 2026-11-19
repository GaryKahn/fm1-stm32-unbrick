// fm1-stm32-unbrick - JieLi USB_KEY entry and ROM keep-alive pulses on the STM32F407's OTG FS pins.
//
// A port of fm1-esp32-unbrick main/recovery.c (MIT), itself a port of FM-1 Transporter src/recovery.c and
// pio/usb_key.pio (MIT, Leo Kuroshita / Hugelton Instruments). Same packet, timings and hand-off rules (us):
//   key packet  0x16EF MSB first, polarity A: D+ the clock, D- the data. Each bit: data set with D+ low for 8,
//               D+ high for 12 (10 for the last bit); both pins driven low before and released after.
//   gap         4 probes 35 apart: D- briefly driven high, then read back (a target holding it low = ACK),
//               D+ high twice in a row = the ROM already pulled up.
//   pulses      D+ driven low for 4 every 1000 (the ROM trims its clock from them) until it releases D+
//               after calibration (>= 1 s) or 6 s have passed; the host must start then.
//
// STM32 differences:
//   - PA12 = D+, PA11 = D- (CN5 on the Discovery board), plain GPIO until the hand-off, then AF10 / OTG FS.
//   - The ESP32 and RP2040 drive the probe weakly and read while driving, so a target can win. STM32 pins drive
//     harder, so the probe drives D- high for 0.5 us only, releases it, and reads 0.3 us later: a free line is
//     still high (its charge decays slowly through the 40 kOhm pull-down), a line the target holds low is low.
//     A false ACK costs nothing: the original then waits for the ROM's D+ pull-up and resumes keying without it.
//   - Interrupts are off for each key packet and pulse; nothing else runs on this core during those.

#include "recovery.h"

#include "compat.h"
#include "log.h"
#include "proto.h"

#define USB_KEY_WORD 0x16EF
#define KEY_PACKET_US 340
#define GAP_PROBES 4
#define GAP_PROBE_SPACING_US 35
#define ACK_PROBE_LOWS 2
#define DP_HIGH_TIMEOUT_MS 500
#define PULSE_SAMPLE_US 100
#define PULSE_LOW_SAMPLES 3         // 300 us low = not just one of our 4 us pulses
#define HANDOFF_PULSE_MS 6000
#define CAL_DONE_MIN_MS 1000

#define BIT_DP (1u << PIN_DP)
#define BIT_DM (1u << PIN_DM)
#define MODER_MASK(pin) (3u << ((pin) * 2))
#define MODER_OUT(pin) (1u << ((pin) * 2))
#define PUPD_DOWN(pin) (2u << ((pin) * 2))

static inline void out_low(uint32_t m) { GPIOA->BSRR = m << 16; }
static inline void out_high(uint32_t m) { GPIOA->BSRR = m; }

static inline void drive(uint32_t m) {
    uint32_t v = GPIOA->MODER;
    if (m & BIT_DP) v = (v & ~MODER_MASK(PIN_DP)) | MODER_OUT(PIN_DP);
    if (m & BIT_DM) v = (v & ~MODER_MASK(PIN_DM)) | MODER_OUT(PIN_DM);
    GPIOA->MODER = v;
}

static inline void release(uint32_t m) {
    uint32_t v = GPIOA->MODER;
    if (m & BIT_DP) v &= ~MODER_MASK(PIN_DP);
    if (m & BIT_DM) v &= ~MODER_MASK(PIN_DM);
    GPIOA->MODER = v;
}

static inline bool dp(void) { return GPIOA->IDR & BIT_DP; }
static inline bool dm(void) { return GPIOA->IDR & BIT_DM; }

static void set_pulldowns(bool on) {
    uint32_t v = GPIOA->PUPDR & ~(MODER_MASK(PIN_DP) | MODER_MASK(PIN_DM));
    if (on) v |= PUPD_DOWN(PIN_DP) | PUPD_DOWN(PIN_DM);
    GPIOA->PUPDR = v;
}

static inline void delay_cycles(uint32_t c) {
    uint32_t s = DWT->CYCCNT;
    while (DWT->CYCCNT - s < c) {}
}

void recovery_release_bus(void) {
    release(BIT_DP | BIT_DM);
    out_low(BIT_DP | BIT_DM);
    set_pulldowns(false);
}

static void bus_listen(void) {
    release(BIT_DP | BIT_DM);
    out_low(BIT_DP | BIT_DM);
    set_pulldowns(true);            // weak (~40 kOhm) pull-downs, as the RP2040 pads: the target's pull-up wins
}

static bool probe_high(uint32_t bit) {
    __disable_irq();
    out_high(bit);
    drive(bit);
    delay_cycles(84);               // 0.5 us: charge the line
    release(bit);
    delay_cycles(50);               // 0.3 us: a held-low line is low by now, a free one still high
    bool v = GPIOA->IDR & bit;
    out_low(bit);
    __enable_irq();
    return v;
}

static void key_send_packet(void) {
    __disable_irq();
    out_low(BIT_DP | BIT_DM);
    drive(BIT_DP | BIT_DM);
    delay_us(3);
    for (int i = 15; i >= 0; i--) {
        if ((USB_KEY_WORD >> i) & 1) out_high(BIT_DM);
        else out_low(BIT_DM);
        delay_us(8);
        out_high(BIT_DP);
        delay_us(i ? 12 : 10);
        out_low(BIT_DP);
    }
    release(BIT_DP | BIT_DM);
    out_low(BIT_DM);
    __enable_irq();
    delay_us(KEY_PACKET_US - 3 - 16 * 20);
}

static int gap_sense(bool *alive) {
    int lows = 0, dp_highs = 0;
    for (int i = 0; i < GAP_PROBES; i++) {
        delay_us(GAP_PROBE_SPACING_US);
        if (dp()) {
            if (++dp_highs >= 2) return 2;
        } else {
            dp_highs = 0;
        }
        if (probe_high(BIT_DM)) {
            *alive = true;
            lows = 0;
        } else if (*alive && ++lows >= ACK_PROBE_LOWS) {
            return 1;
        }
    }
    return 0;
}

static void pulse(void) {
    __disable_irq();
    out_low(BIT_DP);
    drive(BIT_DP);
    delay_us(4);
    release(BIT_DP);
    __enable_irq();
}

// 1 ms pulses for up to ms; stop_on_release: return once the ROM drops D+ after calibration
static void pulses(uint32_t ms, bool stop_on_release) {
    led(LED_ORANGE, true);
    uint64_t start = time_us_64(), next = start;
    uint32_t low_samples = 0;
    for (;;) {
        uint64_t now = time_us_64();
        if (now >= next) {
            pulse();
            next += 1000;
        }
        delay_us(PULSE_SAMPLE_US);
        now = time_us_64();
        uint64_t elapsed = now - start;
        if (stop_on_release && !dp()) {
            if (++low_samples == PULSE_LOW_SAMPLES) {
                if (elapsed >= CAL_DONE_MIN_MS * 1000ull) {
                    dlog("PULSE: D+ released after %.1f ms - calibration done, host takes over", elapsed / 1000.0);
                    break;
                }
                dlog("PULSE: early D+ blip at %.1f ms, ignored", elapsed / 1000.0);
            }
        } else {
            low_samples = 0;
        }
        if (elapsed >= ms * 1000ull) {
            dlog("PULSE: %lu ms of pulses, D+=%d - handing off", (unsigned long)ms, dp());
            break;
        }
    }
}

static bool wait_dp_high(uint32_t timeout_ms) {
    uint64_t deadline = time_us_64() + timeout_ms * 1000ull;
    int highs = 0, dm_highs = 0;
    while (time_us_64() < deadline) {
        if (dp()) {
            if (++highs >= 5) return true;
        } else {
            highs = 0;
        }
        if (dm()) {
            if (++dm_highs == 5) dlog("D- pulled up instead of D+ - are D+/D- swapped?");
        } else {
            dm_highs = 0;
        }
        delay_us(50);
    }
    return false;
}

void recovery_run(void) {
    uint32_t packets = 0;
    bool alive = false, was_alive = false;
    uint64_t last_status = time_us_64(), last_blink = last_status;

    dlog("KEY: sending 0x%04X, polarity A (D+ clock, D- data). Switch the FM-1 ON now.", USB_KEY_WORD);
    for (;;) {
        key_send_packet();
        packets++;
        int s = gap_sense(&alive);
        if (alive && !was_alive) {
            dlog("data line probe responds (not proof of a target)");
            was_alive = true;
        }
        if (s) {
            bus_listen();
            dlog("%s after %lu packets", s == 1 ? "ACK (data line held low)" : "D+ high in gap (ACK missed?)",
                 (unsigned long)packets);
            if (s == 2 || wait_dp_high(DP_HIGH_TIMEOUT_MS)) {
                led(LED_GREEN, false);
                dlog("PULSE: D+ high (ROM pull-up), sending 4 us pulses every 1 ms");
                pulses(HANDOFF_PULSE_MS, true);
                return;
            }
            dlog("no D+ pull-up within %d ms (D+=%d D-=%d) - false ACK, resuming key", DP_HIGH_TIMEOUT_MS, dp(), dm());
            alive = was_alive = false;
            continue;
        }
        uint64_t now = time_us_64();
        if (now - last_status > 2000000) {
            dlog("KEY: %lu packets", (unsigned long)packets);
            last_status = now;
        }
        if (now - last_blink > 250000) {
            led_toggle(LED_GREEN);      // green blinking: keying, switch the FM-1 on
            last_blink = now;
        }
        proto_rx_poll();                // ping / log / rekey answered while keying (the ESP32's receive task)
    }
}

void recovery_pulses_only(uint32_t ms) {
    dlog("PULSE: %lu ms of 1 ms pulses for an already attached target", (unsigned long)ms);
    pulses(ms, false);
}

bool recovery_wait_detached(uint32_t timeout_ms) {
    uint64_t deadline = time_us_64() + timeout_ms * 1000ull;
    int lows = 0;
    while (time_us_64() < deadline) {
        if (!dp()) {
            if (++lows >= 20) return true;      // 2 ms steadily low
        } else {
            lows = 0;
        }
        delay_us(100);
    }
    return false;
}

bool recovery_target_attached(void) {
    for (int i = 0; i < 50; i++) {
        if (!dp()) return false;
        delay_us(1000);
    }
    return true;
}

void recovery_init(void) {
    // PA11/PA12: GPIO inputs, push-pull when driven, low speed; the OTG FS block stays unclocked until hand-off
    GPIOA->OTYPER &= ~(BIT_DP | BIT_DM);
    GPIOA->OSPEEDR &= ~(MODER_MASK(PIN_DP) | MODER_MASK(PIN_DM));
    GPIOA->AFR[1] &= ~((0xFu << ((PIN_DP - 8) * 4)) | (0xFu << ((PIN_DM - 8) * 4)));
    bus_listen();
    delay_us(1000);
    dlog("bus idle: D+=%d D-=%d (both 0 with nothing attached)", dp(), dm());
}
