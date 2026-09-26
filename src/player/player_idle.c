#include "player_idle.h"
#include "native_interrupts.h"
#include <stdint.h>
#define CLOCK_VALUE 0x900C0004U
#define CLOCK_CONTROL 0x900C0008U
#define CLOCK_SOURCE 0x900C0080U
#define CLOCK_HZ 32768U
#define STALLED_POLL_LIMIT 65536U
#define TOTAL_POLL_LIMIT 16777216U
static uint32_t calls, unavailable_clock, stalled_clock, poll_limit, longest_ticks;

static uint32_t clock_read(uint32_t address)
{
    return *(const volatile uint32_t *)(uintptr_t)address;
}

void player_idle_sleep(unsigned milliseconds)
{
    if (!milliseconds)
        return;
    unsigned saved = native_critical_enter();
    ++calls;
    /* Pause/menu waits must return to input polling. Use the player's running
     * timer instead of the SDK's unbounded, interrupt-dependent WFI loop.
     * This consumes CPU while waiting, but needs no timer/VIC reconfiguration
     * and never enters an OS interrupt handler. Actual standby is separate. */
    if ((clock_read(CLOCK_CONTROL) & 0x82U) != 0x82U ||
        clock_read(CLOCK_SOURCE) != 0x0AU) {
        ++unavailable_clock;
        native_critical_leave(saved);
        return;
    }
    uint64_t requested = ((uint64_t)milliseconds * CLOCK_HZ + 999U) / 1000U;
    uint32_t duration = requested > 0x7fffffffU ? 0x7fffffffU : (uint32_t)requested;
    uint32_t start = clock_read(CLOCK_VALUE), previous = start, elapsed = 0;
    unsigned stagnant = 0, polls = 0;
    do {
        uint32_t current = clock_read(CLOCK_VALUE);
        elapsed = start - current; /* Free-running 32-bit downcounter. */
        if (current != previous) {
            previous = current;
            stagnant = 0;
        } else if (++stagnant == STALLED_POLL_LIMIT) {
            ++stalled_clock;
            break;
        }
        if (++polls == TOTAL_POLL_LIMIT) {
            ++poll_limit;
            break;
        }
    } while (elapsed < duration);
    if (elapsed > longest_ticks)
        longest_ticks = elapsed;
    native_critical_leave(saved);
}

void player_idle_debug(FILE *file)
{
    fprintf(file,
            "hardware_idle mode=counter_poll calls=%lu unavailable_clock=%lu stalled_clock=%lu poll_limit=%lu longest_ticks=%lu\n",
            (unsigned long)calls, (unsigned long)unavailable_clock,
            (unsigned long)stalled_clock, (unsigned long)poll_limit,
            (unsigned long)longest_ticks);
}
