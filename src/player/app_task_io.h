#ifndef NDVIDEO_APP_TASK_IO_H
#define NDVIDEO_APP_TASK_IO_H
#include "native_file_io.h"
#include <stdio.h>

/* App-owned cooperative contexts. NativeFileIo below is used ONLY as the
 * verified direct-stdio ABI. No native event/thread/scheduler is created. */
typedef struct {
    NativeFileIo files;
    void *private_state;
    bool supported, initialized, started, returned, waiting, sleeping;
    unsigned signals, wait_mask;
    int error;
    uint32_t wait_started, wait_ticks;
} AppTaskIo;

int app_task_io_init(AppTaskIo *);
int app_task_io_start(AppTaskIo *, void (*)(void *), void *);
int app_task_io_signal(AppTaskIo *, unsigned);
int app_task_io_wait(AppTaskIo *, unsigned, bool, unsigned, unsigned *);
void app_task_io_yield(AppTaskIo *);
int app_task_io_sleep(AppTaskIo *, unsigned);
int app_task_io_join(AppTaskIo *);
int app_task_io_destroy(AppTaskIo *);
int app_task_io_error(const AppTaskIo *);
unsigned app_task_io_critical_enter(void);
void app_task_io_critical_leave(unsigned);
bool app_task_io_service(uint32_t budget_ticks);
bool app_task_io_pending(void);
bool app_task_io_in_job(void);
void app_task_io_drain(void);
typedef struct {
    uint32_t contexts, active_contexts, resumes, spi_yields;
    uint32_t max_step_ticks, max_callback_ticks, minimum_stack_remaining;
    uint32_t errors, init_stage, io_phases;
    int last_error, init_error;
} AppTaskIoStats;
/* Completed and live app-owned contexts; no native scheduler inspection. */
void app_task_io_stats(AppTaskIoStats *);
void app_task_io_debug(FILE *);
size_t app_task_io_memory_bytes(void);

void *app_task_io_file_open(AppTaskIo *, const char *);
void *app_task_io_file_open_write(AppTaskIo *, const char *);
void *app_task_io_file_open_append(AppTaskIo *, const char *);
size_t app_task_io_file_write(AppTaskIo *, void *, const void *, size_t);
int app_task_io_file_close(AppTaskIo *, void *);
int app_task_io_file_exists(AppTaskIo *, const char *, int *);
int app_task_io_file_remove(AppTaskIo *, const char *);
#endif
