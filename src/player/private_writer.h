#ifndef NDVIDEO_PRIVATE_WRITER_H
#define NDVIDEO_PRIVATE_WRITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Exact-firmware adapter. One app-owned fiber, no OS thread, event, scheduler
 * sleep/yield or GUI suppression. Used by the player-owned storage contexts.
 *
 * The foreground MUST NOT reenter storage while a native transaction is
 * suspended (including our raw reader). The app scheduler owns this exclusion
 * until storage_native_end; idle contexts hold no transaction. Between steps
 * it may run independent CPU/render work. Finish a submitted job before
 * freeing its stack, restoring the OS, or unloading application code.
 *
 * Jobs call native stdio directly, NEVER Ndless SWIs. The SPI callback is
 * temporarily interposed only while the private stack is executing. We yield
 * only outside kernel protection and between completed SPI transactions.
 * This is cooperative: CPU-only native work and a single driver transaction
 * can exceed the requested budget. Hardware measurements must bound those. */
enum { PRIVATE_WRITER_STACK_BYTES = 65536, PRIVATE_WRITER_GUARD_BYTES = 64 };
enum {
    PW_OK,
    PW_UNSUPPORTED,
    PW_BAD_ARGUMENT,
    PW_BAD_TASK,
    PW_BAD_SPI,
    PW_BUSY,
    PW_STATE_CHANGED,
    PW_STACK_CORRUPT,
    PW_UNSAFE_RETURN
};

/* One initialization observation, including rejection before a fiber starts.
 * These describe the existing checks; they do not grant permission to retry
 * or weaken them. Unavailable fields stay zero and must be gated by valid. */
enum {
    PW_INIT_NONE,
    PW_INIT_ARGUMENT,
    PW_INIT_ENTRY_IRQ,
    PW_INIT_PLATFORM,
    PW_INIT_TABLE_ADDRESS,
    PW_INIT_TABLE_ALIAS,
    PW_INIT_TASK_SPAN,
    PW_INIT_TASK_MAGIC,
    PW_INIT_TASK_PROTECTED,
    PW_INIT_STACK_SPAN,
    PW_INIT_SPI_SPAN,
    PW_INIT_SPI_DESCRIPTOR,
    PW_INIT_SPI_BUSY,
    PW_INIT_ERRNO_SPAN
};
enum {
    PW_INIT_HAVE_ENTRY_MASK = 1U << 0,
    PW_INIT_HAVE_STACK_ARGUMENT = 1U << 1,
    PW_INIT_HAVE_TASK_ADDRESS = 1U << 2,
    PW_INIT_HAVE_TASK_FIELDS = 1U << 3,
    PW_INIT_HAVE_TTBR = 1U << 4,
    PW_INIT_HAVE_TABLE_ALIAS = 1U << 5,
    PW_INIT_HAVE_REJECTED_ADDRESS = 1U << 6,
    PW_INIT_HAVE_REJECTED_DESCRIPTOR = 1U << 7,
    PW_INIT_HAVE_ERRNO_ADDRESS = 1U << 8,
    PW_INIT_HAVE_SPI_ADDRESS = 1U << 9
};
typedef struct {
    uint32_t stage, valid, entry_mask;
    uint32_t task, task_magic, task_protection;
    uint32_t stack_address, stack_bytes;
    uint32_t ttbr, table_alias;
    uint32_t rejected_address, rejected_descriptor;
    uint32_t errno_address, spi_device;
} PrivateWriterInitDiagnostic;

typedef struct {
    uint32_t *foreground_sp, *worker_sp;
    uint32_t task, spi_device, original_callback;
    uint32_t original_stack[4], worker_stack[4];
    uint32_t *stack;
    uint32_t stack_bytes;
    int *native_errno;
    int foreground_errno, worker_errno;
    void (*job)(void *);
    void *argument;
    bool initialized, pending, running, done;
    unsigned error;
    uint32_t slice_start, budget, resumes, yields, callbacks, spi_yields, max_callback_ticks;
    uint32_t protected_callbacks, busy_callbacks, max_slice_ticks;
    uint32_t dispatch_start, dispatch_delta, mask_changes, task_changes;
    uint32_t commands[8], minimum_stack_remaining;
    PrivateWriterInitDiagnostic init_diagnostic;
} PrivateWriter;

/* Call with zero-initialized context, and an aligned, caller-owned stack. */
unsigned private_writer_init(PrivateWriter *, void *stack, size_t bytes);
/* Code/layout compatibility only; init also checks live task/stack ownership. */
bool private_writer_supported(void);
const char *private_writer_init_stage_name(unsigned stage);
unsigned private_writer_submit(PrivateWriter *, void (*job)(void *), void *);
/* Down-counting 32768 Hz app timer at 900C0004 must be running. */
bool private_writer_step(PrivateWriter *, uint32_t budget_ticks);
/* A job may voluntarily cooperate outside a native call too. */
void private_writer_cooperate(void);
bool private_writer_restored(const PrivateWriter *);

#endif
