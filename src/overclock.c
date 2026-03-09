#include "overclock.h"

#include <libndls.h>
#include <os.h>

#define CX2_CLOCK_MEM30 0x90140030U
#define CX2_CLOCK_MEM20 0x90140020U
#define CX2_CLOCK_MEM10 0x90140810U

#define CLOCK_BASE_MHZ 12U
#define CLOCK_MULT_BITS 6U
#define CLOCK_DIV1_BITS 5U
#define CLOCK_DIV2_BITS 4U

/*
 * Based on noverII:
 * - stock CX II appears to sit around multiplier 33 (396 MHz)
 * - users report 492 MHz as the rough upper edge
 *
 * The player uses a conservative preset below that ceiling to reduce the
 * chance of hard resets while still giving the decoder real extra headroom.
 */
#define CX2_OVERCLOCK_MULT 39U  /* 468 MHz */
#define CX2_OVERCLOCK_DIV1 1U
#define CX2_OVERCLOCK_DIV2 0U

static uint32_t bits_mask(unsigned bits)
{
    return (1U << bits) - 1U;
}

static uint32_t read_reg(uint32_t address)
{
    return *(volatile unsigned *) address;
}

static void write_reg(uint32_t address, uint32_t value)
{
    *(volatile unsigned *) address = value;
}

static void apply_cx2_config(uint32_t config30, uint32_t config20, uint32_t config10)
{
    uint32_t current30 = read_reg(CX2_CLOCK_MEM30);
    uint32_t current20 = read_reg(CX2_CLOCK_MEM20);
    uint32_t current10 = read_reg(CX2_CLOCK_MEM10);
    int interrupt_mask = TCT_Local_Control_Interrupts(0);

    if ((((config30 >> 24) & bits_mask(CLOCK_MULT_BITS)) != ((current30 >> 24) & bits_mask(CLOCK_MULT_BITS))) ||
        (((config30 >> 16) & bits_mask(CLOCK_DIV1_BITS)) != ((current30 >> 16) & bits_mask(CLOCK_DIV1_BITS)))) {
        write_reg(CX2_CLOCK_MEM30, config30);
    }
    if (((config20 >> 20) & bits_mask(CLOCK_DIV2_BITS)) != ((current20 >> 20) & bits_mask(CLOCK_DIV2_BITS))) {
        write_reg(CX2_CLOCK_MEM20, config20);
    }
    if ((config10 & (1U << 4)) != (current10 & (1U << 4))) {
        write_reg(CX2_CLOCK_MEM10, config10);
    }

    msleep(1);
    TCT_Local_Control_Interrupts(interrupt_mask);
}

static void make_cx2_config(
    uint32_t *config30,
    uint32_t *config20,
    uint32_t *config10,
    uint8_t mult,
    uint8_t div1,
    uint8_t div2)
{
    *config30 |= 1U;
    *config30 &= ~(1U << 4);
    *config30 &= ~(bits_mask(8U) << 24);
    *config30 |= ((uint32_t) mult & bits_mask(CLOCK_MULT_BITS)) << 24;
    *config30 &= ~(bits_mask(CLOCK_DIV1_BITS) << 16);
    *config30 |= ((uint32_t) div1 & bits_mask(CLOCK_DIV1_BITS)) << 16;

    if (div2 != 0U) {
        *config10 |= 1U << 4;
    } else {
        *config10 &= ~(1U << 4);
    }

    *config20 &= ~(bits_mask(CLOCK_DIV2_BITS) << 20);
    *config20 |= ((uint32_t) div2 & bits_mask(CLOCK_DIV2_BITS)) << 20;
}

void clock_state_init(ClockState *state)
{
    if (!state) {
        return;
    }

    state->cx2_hardware = is_cx2;
    state->cx2_applied = false;
    state->legacy_cpu_speed = 0U;
    state->original_config30 = 0U;
    state->original_config20 = 0U;
    state->original_config10 = 0U;

    if (state->cx2_hardware) {
        state->original_config30 = read_reg(CX2_CLOCK_MEM30);
        state->original_config20 = read_reg(CX2_CLOCK_MEM20);
        state->original_config10 = read_reg(CX2_CLOCK_MEM10);
    } else {
        state->legacy_cpu_speed = set_cpu_speed(CPU_SPEED_150MHZ);
    }
}

void clock_state_apply_boost(ClockState *state)
{
    uint32_t config30;
    uint32_t config20;
    uint32_t config10;

    if (!state || state->cx2_applied) {
        return;
    }

    if (!state->cx2_hardware) {
        return;
    }

    config30 = state->original_config30;
    config20 = state->original_config20;
    config10 = state->original_config10;
    make_cx2_config(&config30, &config20, &config10, CX2_OVERCLOCK_MULT, CX2_OVERCLOCK_DIV1, CX2_OVERCLOCK_DIV2);
    apply_cx2_config(config30, config20, config10);
    state->cx2_applied = true;
}

void clock_state_restore(ClockState *state)
{
    if (!state) {
        return;
    }

    if (state->cx2_hardware) {
        if (state->cx2_applied) {
            apply_cx2_config(state->original_config30, state->original_config20, state->original_config10);
            state->cx2_applied = false;
        }
    } else {
        set_cpu_speed(state->legacy_cpu_speed);
    }
}

const char *clock_state_label(const ClockState *state)
{
    if (!state) {
        return "clk?";
    }
    if (state->cx2_hardware) {
        if (state->cx2_applied) {
            return "468MHz";
        }
        return "stock";
    }
    return "150MHz";
}
