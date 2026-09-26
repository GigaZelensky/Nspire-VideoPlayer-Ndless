#include "app_task_io.h"
#include "native_interrupts.h"
#include "private_writer.h"
#include "raw_player_io.h"
#include "storage_activity.h"
#include "storage_mutation.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define APP_TASKS 4U
typedef struct {
    PrivateWriter writer;
    AppTaskIo *owner;
    void (*work)(void *);
    void *argument;
    void *allocation;
    uint32_t stack[PRIVATE_WRITER_STACK_BYTES / 4U] __attribute__((aligned(8)));
} AppFiber;
static AppTaskIo *tasks[APP_TASKS], *executing, *storage_owner;
static unsigned cursor;
static AppTaskIoStats completed = {.minimum_stack_remaining = UINT32_MAX};
static uint32_t counter(void)
{
    return *(const volatile uint32_t *)0x900C0004U;
}
unsigned app_task_io_critical_enter(void)
{
    return native_critical_enter();
}
void app_task_io_critical_leave(unsigned mask)
{
    native_critical_leave(mask);
}
bool app_task_io_in_job(void)
{
    return executing != NULL;
}
static bool runnable(AppTaskIo *task)
{
    if (!task || !task->started || task->returned)
        return false;
    if (!task->waiting && !task->sleeping)
        return true;
    if (task->waiting && (task->signals & task->wait_mask))
        return true;
    return task->wait_ticks != UINT_MAX &&
           (uint32_t)(task->wait_started - counter()) >= task->wait_ticks;
}
bool app_task_io_pending(void)
{
    for (unsigned i = 0; i < APP_TASKS; ++i)
        if (runnable(tasks[i]))
            return true;
    return false;
}
static void entry(void *p)
{
    AppFiber *fiber = p;
    fiber->work(fiber->argument);
    fiber->owner->returned = true;
}
int app_task_io_init(AppTaskIo *task)
{
    if (!task || task->private_state || task->started)
        return NATIVE_FILE_IO_BAD_STATE;
    memset(task, 0, sizeof(*task));
    int error = native_file_io_init(&task->files);
    if (error)
        return error;
    task->supported = task->initialized = true;
    return 0;
}
int app_task_io_start(AppTaskIo *task, void (*work)(void *), void *argument)
{
    if (!task || !task->initialized || task->started || !work || executing)
        return NATIVE_FILE_IO_BAD_STATE;
    unsigned slot = 0;
    while (slot < APP_TASKS && tasks[slot])
        ++slot;
    if (slot == APP_TASKS)
        return NATIVE_FILE_IO_BUSY;
    /* The native heap may return only four-byte alignment. A member's
     * aligned(8) attribute cannot strengthen the allocator's guarantee. */
    unsigned allocation_mask = native_interrupt_mask();
    void *allocation = calloc(1, sizeof(AppFiber) + 7U);
    native_critical_leave(allocation_mask);
    if (!allocation)
        return NATIVE_FILE_IO_BAD_STATE;
    AppFiber *fiber = (AppFiber *)(((uintptr_t)allocation + 7U) & ~(uintptr_t)7U);
    fiber->allocation = allocation;
    fiber->owner = task;
    fiber->work = work;
    fiber->argument = argument;
    task->private_state = fiber;
    task->started = true;
    task->returned = false;
    tasks[slot] = task;
    return 0;
}
int app_task_io_signal(AppTaskIo *task, unsigned bits)
{
    if (!task || !task->initialized)
        return NATIVE_FILE_IO_BAD_STATE;
    task->signals |= bits;
    return 0;
}
static uint32_t ticks(unsigned os_ticks)
{
    if (os_ticks == UINT_MAX)
        return UINT_MAX;
    uint64_t result = ((uint64_t)os_ticks * 32768U + 99U) / 100U;
    return result >= 0x80000000U ? 0x7fffffffU : (uint32_t)result;
}
int app_task_io_wait(AppTaskIo *task, unsigned bits, bool consume, unsigned timeout,
                     unsigned *received)
{
    if (!task || executing != task || !received)
        return NATIVE_FILE_IO_BAD_STATE;
    *received = 0;
    task->wait_started = counter();
    task->wait_ticks = ticks(timeout);
    task->wait_mask = bits;
    for (;;) {
        unsigned ready = task->signals & bits;
        if (ready) {
            if (consume)
                task->signals &= ~ready;
            *received = ready;
            task->waiting = false;
            return 0;
        }
        if (!timeout || (task->wait_ticks != UINT_MAX &&
                         (uint32_t)(task->wait_started - counter()) >= task->wait_ticks)) {
            task->waiting = false;
            return -36;
        }
        task->waiting = true;
        private_writer_cooperate();
    }
}
void app_task_io_yield(AppTaskIo *task)
{
    (void)task;
    if (executing)
        private_writer_cooperate();
    else
        (void)app_task_io_service(32U);
}
int app_task_io_sleep(AppTaskIo *task, unsigned amount)
{
    if (!task || executing != task || amount == UINT_MAX)
        return NATIVE_FILE_IO_BAD_STATE;
    task->wait_started = counter();
    task->wait_ticks = ticks(amount);
    task->sleeping = true;
    do {
        private_writer_cooperate();
    } while ((uint32_t)(task->wait_started - counter()) < task->wait_ticks);
    task->sleeping = false;
    return 0;
}
bool app_task_io_service(uint32_t budget)
{
    if (executing || !budget)
        return false;
    AppTaskIo *task = storage_owner;
    if (!task) {
        for (unsigned n = 0; n < APP_TASKS; ++n) {
            unsigned i = (cursor + n) % APP_TASKS;
            if (runnable(tasks[i])) {
                task = tasks[i];
                cursor = (i + 1U) % APP_TASKS;
                break;
            }
        }
    }
    if (!task)
        return false;
    /* Drain a read over multiple foreground visits; never enter the native
     * filesystem while our SPI read or borrowed mapping view is still live. */
    if (!raw_player_prepare_native())
        return true;
    AppFiber *fiber = task->private_state;
    /* Submit is allocation-only: the reader may own an in-flight SPI command
     * then. Validate/install the private context only after the handoff. */
    if (!fiber->writer.initialized) {
        unsigned error = private_writer_init(&fiber->writer, fiber->stack, sizeof(fiber->stack));
        if (!error)
            error = private_writer_submit(&fiber->writer, entry, fiber);
        if (error) {
            task->error = -4000 - (int)error;
            task->returned = true;
            storage_mutation_invalidate();
            raw_player_native_released();
            return true;
        }
    }
    executing = task;
    bool stepped = private_writer_step(&fiber->writer, budget);
    executing = NULL;
    storage_owner = storage_native_active() ? task : NULL;
    if (task->error || fiber->writer.error)
        storage_mutation_invalidate();
    raw_player_native_released();
    return stepped;
}
int app_task_io_join(AppTaskIo *task)
{
    if (!task || executing)
        return NATIVE_FILE_IO_BAD_STATE;
    if (!task->started)
        return 0;
    AppFiber *fiber = task->private_state;
    if (!task->returned) {
        app_task_io_service(32U);
        return NATIVE_FILE_IO_BUSY;
    }
    if (fiber->writer.resumes && !private_writer_restored(&fiber->writer))
        return NATIVE_FILE_IO_BAD_STATE;
    task->started = false;
    return 0;
}
int app_task_io_error(const AppTaskIo *task)
{
    return task ? task->error : NATIVE_FILE_IO_BAD_STATE;
}
int app_task_io_destroy(AppTaskIo *task)
{
    if (!task || task->started || executing)
        return NATIVE_FILE_IO_BAD_STATE;
    if (task->private_state) {
        PrivateWriter *w = &((AppFiber *)task->private_state)->writer;
        ++completed.contexts;
        completed.resumes += w->resumes;
        completed.spi_yields += w->spi_yields;
        if (w->max_slice_ticks > completed.max_step_ticks)
            completed.max_step_ticks = w->max_slice_ticks;
        if (w->max_callback_ticks > completed.max_callback_ticks)
            completed.max_callback_ticks = w->max_callback_ticks;
        if (w->resumes && w->minimum_stack_remaining < completed.minimum_stack_remaining)
            completed.minimum_stack_remaining = w->minimum_stack_remaining;
        if (w->error || task->error) {
            ++completed.errors;
            completed.last_error = task->error ? task->error : -4000 - (int)w->error;
        }
        if (!w->initialized && task->error && w->init_diagnostic.valid) {
            completed.init_stage = w->init_diagnostic.stage;
            completed.init_error = task->error;
        }
    }
    for (unsigned i = 0; i < APP_TASKS; ++i)
        if (tasks[i] == task)
            tasks[i] = NULL;
    if (task->private_state) {
        unsigned allocation_mask = native_interrupt_mask();
        free(((AppFiber *)task->private_state)->allocation);
        native_critical_leave(allocation_mask);
    }
    memset(task, 0, sizeof(*task));
    return 0;
}
void app_task_io_drain(void)
{
    if (executing)
        return;
    while (app_task_io_pending() || storage_native_active())
        app_task_io_service(32U);
}
size_t app_task_io_memory_bytes(void)
{
    size_t total = storage_mutation_memory_bytes();
    for (unsigned i = 0; i < APP_TASKS; ++i)
        if (tasks[i])
            total += sizeof(AppFiber) + 7U;
    return total;
}
void app_task_io_stats(AppTaskIoStats *stats)
{
    if (!stats)
        return;
    *stats = completed;
    for (unsigned i = 0; i < APP_TASKS; ++i)
        if (tasks[i]) {
            PrivateWriter *w = &((AppFiber *)tasks[i]->private_state)->writer;
            ++stats->contexts;
            ++stats->active_contexts;
            stats->resumes += w->resumes;
            stats->spi_yields += w->spi_yields;
            if (w->max_slice_ticks > stats->max_step_ticks)
                stats->max_step_ticks = w->max_slice_ticks;
            if (w->max_callback_ticks > stats->max_callback_ticks)
                stats->max_callback_ticks = w->max_callback_ticks;
            if (w->resumes && w->minimum_stack_remaining < stats->minimum_stack_remaining)
                stats->minimum_stack_remaining = w->minimum_stack_remaining;
            stats->io_phases |= tasks[i]->files.io_phase;
            if (w->error || tasks[i]->error) {
                ++stats->errors;
                stats->last_error = tasks[i]->error ? tasks[i]->error : -4000 - (int)w->error;
            }
            if (!w->initialized && tasks[i]->error && w->init_diagnostic.valid) {
                stats->init_stage = w->init_diagnostic.stage;
                stats->init_error = tasks[i]->error;
            }
        }
}
void app_task_io_debug(FILE *file)
{
    if (!file)
        return;
    AppTaskIoStats stats;
    app_task_io_stats(&stats);
    fprintf(
        file,
        "app_io contexts=%lu active=%lu resumes=%lu spi_yields=%lu max_step_ticks=%lu max_callback_ticks=%lu stack_remaining=%lu io_phases=%lu errors=%lu last_error=%d init_stage=%lu init_error=%d\n",
        (unsigned long)stats.contexts, (unsigned long)stats.active_contexts,
        (unsigned long)stats.resumes, (unsigned long)stats.spi_yields,
        (unsigned long)stats.max_step_ticks, (unsigned long)stats.max_callback_ticks,
        (unsigned long)stats.minimum_stack_remaining, (unsigned long)stats.io_phases,
        (unsigned long)stats.errors, stats.last_error, (unsigned long)stats.init_stage,
        stats.init_error);
}
/* These wrappers never create a task. Native stdio executes on the app fiber. */
void *app_task_io_file_open(AppTaskIo *b, const char *p)
{
    void *r = native_file_io_open(&b->files, p);
    if (!r)
        storage_mutation_invalidate();
    return r;
}
void *app_task_io_file_open_write(AppTaskIo *b, const char *p)
{
    void *r = native_file_io_open_write(&b->files, p);
    if (!r)
        storage_mutation_invalidate();
    return r;
}
void *app_task_io_file_open_append(AppTaskIo *b, const char *p)
{
    void *r = native_file_io_open_append(&b->files, p);
    if (!r)
        storage_mutation_invalidate();
    return r;
}
size_t app_task_io_file_write(AppTaskIo *b, void *f, const void *p, size_t n)
{
    size_t r = native_file_io_write(&b->files, f, p, n);
    if (r != n)
        storage_mutation_invalidate();
    return r;
}
int app_task_io_file_close(AppTaskIo *b, void *f)
{
    int r = native_file_io_close(&b->files, f);
    if (r)
        storage_mutation_invalidate();
    return r;
}
int app_task_io_file_exists(AppTaskIo *b, const char *p, int *e)
{
    int r = native_file_io_exists(&b->files, p, e);
    if (r < 0)
        storage_mutation_invalidate();
    return r;
}
int app_task_io_file_remove(AppTaskIo *b, const char *p)
{
    int r = native_file_io_remove(&b->files, p);
    if (r)
        storage_mutation_invalidate();
    return r;
}
