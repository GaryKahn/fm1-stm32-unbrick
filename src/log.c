// fm1-stm32-unbrick - the board's log: a ring of text lines in CCM RAM, read with the `log` request.
// Same behaviour as fm1-esp32-unbrick main/log.c.
#include "log.h"

#include <stdarg.h>
#include <stdio.h>

#include "compat.h"

#define RING 16384      // power of two

static char ring[RING] __attribute__((section(".ccmram")));
static uint32_t head, tail;

void dlog(const char *fmt, ...) {
    char line[160];
    uint64_t t = time_us_64();
    int n = snprintf(line, sizeof(line), "%4lu.%03lu ", (unsigned long)(t / 1000000u),
                     (unsigned long)((t / 1000u) % 1000u));
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    for (int i = 0; i < n; i++) {
        ring[head++ & (RING - 1)] = line[i];
        if (head - tail > RING) tail = head - RING;
    }
    __set_PRIMASK(primask);
}

uint32_t log_take(char *buf, uint32_t len) {
    uint32_t n = 0;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    while (tail != head && n < len) buf[n++] = ring[tail++ & (RING - 1)];
    __set_PRIMASK(primask);
    return n;
}
