#ifndef NDVIDEO_OVERCLOCK_H
#define NDVIDEO_OVERCLOCK_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool cx2_hardware;
    bool cx2_applied;
    unsigned legacy_cpu_speed;
    uint32_t original_config30;
    uint32_t original_config20;
    uint32_t original_config10;
} ClockState;

void clock_state_init(ClockState *state);
void clock_state_apply_boost(ClockState *state);
void clock_state_restore(ClockState *state);
const char *clock_state_label(const ClockState *state);

#endif
