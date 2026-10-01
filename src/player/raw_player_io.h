#ifndef NDVIDEO_RAW_PLAYER_IO_H
#define NDVIDEO_RAW_PLAYER_IO_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef struct RawPlayerIo RawPlayerIo;
/* Cost of the last foreground join. Writer service can include reader
 * handoff work, so writer_service_ticks is inclusive, not additive. */
typedef struct {
    uint32_t wall_ticks, reader_ticks, crypto_ticks, writer_service_ticks;
    uint32_t physical_reads, region_rebuilds, view_captures, start_phase;
} RawPlayerWaitStats;
bool raw_player_last_wait(const RawPlayerIo *, RawPlayerWaitStats *);
RawPlayerIo *raw_player_create(const char *path);
void raw_player_destroy(RawPlayerIo *);
void raw_player_cancel(RawPlayerIo *);
/* wait=false only submits/collects; raw_player_service/idle makes progress. */
int raw_player_read(RawPlayerIo *, uint64_t, void *, size_t, bool wait);
bool raw_player_idle(unsigned milliseconds);
void raw_player_before_native(void);
/* Serialize another independent reader without pretending it changed flash. */
void raw_player_before_read(void);
bool raw_player_prepare_native(void);
void raw_player_native_released(void);
void raw_player_service(uint32_t budget_ticks);
/* One guarded RAM-only crypto step; never starts physical storage work. */
bool raw_player_crypto_step(uint32_t spare_ticks);
/* One read step, including record boundaries, with an already-owned view. */
bool raw_player_read_step(uint32_t spare_ticks);
/* Only idle/READY requests may be submitted/collected in a short wait. */
bool raw_player_needs_request(const RawPlayerIo *);
void raw_player_after_clock_reset(RawPlayerIo *);
int raw_player_error(const RawPlayerIo *);
uint32_t raw_player_file_bytes(const RawPlayerIo *);
size_t raw_player_memory_bytes(void);
bool raw_player_last_read(RawPlayerIo *, uint32_t *, uint32_t *, uint32_t *);
void raw_player_debug(FILE *, const RawPlayerIo *);
#endif
