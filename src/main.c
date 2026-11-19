// fm1-stm32-unbrick - FM-1 Transporter's recovery on an STM32F4DISCOVERY board (MB997C/D/E).
//
//   Mac ── CN1 mini-USB (ST-LINK: SWD + SEGGER RTT via OpenOCD) ──► STM32F407 ── CN5 micro-USB (PA12 D+, PA11 D-) ──► FM-1
//
// One core, one loop: USB_KEY and pulses (recovery.c), then the OTG FS host (host.c, TinyUSB) and the requests
// (proto.c). PA11/PA12 have one owner at a time: recovery hands them to the OTG host in one call sequence, with
// the ROM pulses running until the moment the host takes them.
//
// LEDs: green blinking = keying (switch the FM-1 on); orange = ROM answered, pulses / hand-off;
//       blue = FM-1 in UBOOT, ready; red = a flash sector is being written (do not unplug).
//       LD7 (green, beside CN5) must never light: it means the board is powering the FM-1.

#include "compat.h"
#include "host.h"
#include "jieli_uboot.h"
#include "log.h"
#include "proto.h"
#include "recovery.h"
#include "tusb.h"

#define ATTACHED_PULSE_MS 6000
#define REKEY_MAGIC 0x4B455931      // "KEY1": force the USB_KEY

extern uint32_t fm1_rekey_magic;

int main(void) {
    board_init();
    proto_start();

    dlog("fm1-stm32-unbrick: D+=PA12 D-=PA11 (CN5), data on RTT channel 0");
    if (!board_hse_ok) dlog("WARNING: 8 MHz crystal did not start - running from HSI, USB timing may be off");
    dlog("CN5 power switch: PC0=%lu (%s)", (unsigned long)(GPIOC->ODR & 1u),
         (GPIOC->ODR & 1u) ? "OFF - correct, LD7 must be dark" : "ON - WRONG, do not connect the FM-1");
    dlog("loader %s", jl_loader_builtin() ? "built in (24064 bytes)" : "MISSING: size mismatch");
    recovery_init();

#ifdef FM1_SELFTEST
    // Self-test build: no USB_KEY. Powers CN5 (VBUS on, LD7 lights) and enumerates a USB flash drive through
    // the same host code, up to INQUIRY. NEVER connect the FM-1 while this build is on the board.
    (void)REKEY_MAGIC;
    fm1_rekey_magic = 0;
    led(LED_GREEN, true);
    dlog("SELFTEST build: CN5 VBUS ON, no USB_KEY. Plug a USB flash drive into CN5 (OTG adapter). NOT the FM-1.");
    GPIOC->BSRR = 1u << (0 + 16);       // PC0 low: STMPS2141 on
    delay_us(100000);
#else
    bool force_key = fm1_rekey_magic == REKEY_MAGIC;
    fm1_rekey_magic = 0;
    if (force_key) {
        dlog("REKEY: waiting for D+ to drop (loader watchdog / power-off)");
        bool dropped = recovery_wait_detached(15000);
        dlog("REKEY: %s; USB_KEY forced now", dropped ? "D+ dropped" : "D+ still high after 15 s - power-cycle the FM-1");
        recovery_run();
    } else if (recovery_target_attached()) {
        dlog("D+ already pulled up (FM-1 app or a waiting UBOOT) - skipping USB_KEY");
        recovery_pulses_only(ATTACHED_PULSE_MS);
    } else {
        recovery_run();
    }
#endif

    uint64_t t_release = time_us_64();
    recovery_release_bus();
    fm1_host_start();
    led(LED_ORANGE, false);
    dlog("HANDOFF: pulses stopped, OTG host started in %lu us", (unsigned long)(time_us_64() - t_release));

    for (;;) {
        tuh_task();
        fm1_host_task();
        proto_rx_poll();
        proto_poll();
    }
}
