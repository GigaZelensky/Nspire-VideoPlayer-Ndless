#ifndef NDVIDEO_RAW_PLAYER_IO_H
#define NDVIDEO_RAW_PLAYER_IO_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef struct RawPlayerIo RawPlayerIo;
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
void raw_player_after_clock_reset(RawPlayerIo *);
int raw_player_error(const RawPlayerIo *);
uint32_t raw_player_file_bytes(const RawPlayerIo *);
size_t raw_player_memory_bytes(void);
bool raw_player_last_read(RawPlayerIo *, uint32_t *, uint32_t *, uint32_t *);
void raw_player_debug(FILE *, const RawPlayerIo *);
#endif
