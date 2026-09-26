#include "private_writer.h"
#include "native_firmware.h"
#include "storage_mutation.h"
#include "../platform/arm926_ram_span.h"
#include <string.h>
#include <os.h>

#define WORD(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define TASK_POINTER 0x1148F060U
#define SPI_POINTER 0x113C4268U
#define SPI_CALLBACK 0x1001A9FCU
#define STACK_GUARD 0xBADC0FFEU
static PrivateWriter *executing;
extern void private_context_swap(uint32_t **save_sp, uint32_t *next_sp);
extern void private_context_entry(void);

static uint32_t counter(void)
{
    return WORD(0x900C0004U);
}
static uint32_t mask(void)
{
    uint32_t value;
    __asm__ volatile("mrs %0,cpsr" : "=r"(value));
    return value & 0xC0U;
}
static uint32_t lock(void)
{
    uint32_t value, masked;
    __asm__ volatile("mrs %0,cpsr\norr %1,%0,#0xc0\nmsr cpsr_c,%1"
                     : "=&r"(value), "=&r"(masked)::"memory", "cc");
    return value & 0xC0U;
}
static void unlock(uint32_t saved)
{
    uint32_t value;
    __asm__ volatile("mrs %0,cpsr\nbic %0,%0,#0xc0\norr %0,%0,%1\nmsr cpsr_c,%0"
                     : "=&r"(value)
                     : "r"(saved)
                     : "memory", "cc");
}
static bool reject_span(PrivateWriterInitDiagnostic *diagnostic, unsigned stage, uint32_t address)
{
    diagnostic->stage = stage;
    diagnostic->rejected_address = address;
    diagnostic->valid |= PW_INIT_HAVE_REJECTED_ADDRESS;
    return false;
}
static uint32_t mapping_word(uint32_t address, void *context)
{
    (void)context;
    return WORD(address);
}
static bool span(uint32_t address, uint32_t bytes, PrivateWriterInitDiagnostic *diagnostic,
                 unsigned stage)
{
    if ((address & 3U) || !bytes || bytes > 0x4000000U || address < 0x10000000U ||
        address > 0x14000000U - bytes)
        return reject_span(diagnostic, stage, address);
    uint32_t ttbr;
    __asm__ volatile("mrc p15,0,%0,c2,c0,0" : "=r"(ttbr));
    ttbr &= 0xFFFFC000U;
    diagnostic->ttbr = ttbr;
    diagnostic->valid |= PW_INIT_HAVE_TTBR;
    if (ttbr != 0xA4004000U) {
        /* The player's SRAM clone keeps the OS fixed L1 alias and CP15's
         * active L1 on the same DRAM table. Accept only that exact relation. */
        if (ttbr < 0x10000000U || ttbr > 0x13FFC000U)
            return reject_span(diagnostic, PW_INIT_TABLE_ADDRESS, ttbr);
        uint32_t alias = WORD(0xA4004000U + 0xA40U * 4U);
        diagnostic->table_alias = alias;
        diagnostic->valid |= PW_INIT_HAVE_TABLE_ALIAS;
        if ((alias & 3U) != 2U || (alias & 0xFFF00000U) + 0x4000U != ttbr) {
            diagnostic->rejected_descriptor = alias;
            diagnostic->valid |= PW_INIT_HAVE_REJECTED_DESCRIPTOR;
            return reject_span(diagnostic, PW_INIT_TABLE_ALIAS, 0xA4004000U + 0xA40U * 4U);
        }
    }
    Arm926RamSpanFailure failure;
    if (!arm926_ram_span(address, bytes, 0xA4004000U, mapping_word, NULL, &failure)) {
        if (failure.have_descriptor) {
            diagnostic->rejected_descriptor = failure.descriptor;
            diagnostic->valid |= PW_INIT_HAVE_REJECTED_DESCRIPTOR;
        }
        return reject_span(diagnostic, stage, failure.address);
    }
    return true;
}
bool private_writer_supported(void)
{
    if (!native_firmware_memory())
        return false;
    /* Require the driver ABI we can yield through, regardless of OS label. */
    static const struct {
        uint32_t address, words[4];
    } code[] = {{0x1042B5F0U, {0xE51F0210U, 0xE5900000U, 0xE3500000U, 0xE3A03000U}},
                {0x103A3310U, {0xE92D4038U, 0xE1A04000U, 0xE1A05001U, 0xEB0220B3U}},
                {0x10074FCCU, {0xE3510000U, 0xE92D4070U, 0x08BD8070U, 0xE3500000U}},
                {0x1001A9FCU, {0xE92D4030U, 0xE59F21E8U, 0xE5913000U, 0xE3A05000U}},
                {0x1048D768U, {0xE92D4008U, 0xEBFE6EA3U, 0xE3500000U, 0x059F0014U}},
                {0x10423C64U, {0xE92D40F0U, 0xE1A07000U, 0xE24DD044U, 0xE1A06001U}},
                {0x10423E08U, {0xE92D45F8U, 0xE2534000U, 0xE1A06000U, 0xE1A07001U}},
                {0x1042429CU, {0xE92D47F0U, 0xE2534000U, 0xE1A05000U, 0xE1A07001U}},
                {0x104236E4U, {0xE92D4038U, 0xE1A04000U, 0xEBFFFFC4U, 0xE3500000U}},
                {0x10493C78U, {0xE92D4070U, 0xE24DDD09U, 0xE1A05001U, 0xE1A04000U}}};
    for (unsigned i = 0; i < sizeof(code) / sizeof(*code); ++i)
        for (unsigned j = 0; j < 4; ++j)
            if (WORD(code[i].address + j * 4U) != code[i].words[j])
                return false;
    return WORD(0x10429230U) == TASK_POINTER;
}
static bool spi_idle(void)
{
    return !(WORD(0xB8000010U) & 0x100100U) && !(WORD(0xB8000020U) & 2U) &&
           !(WORD(0xB8000018U) & 2U);
}
static void stack_get(uint32_t task, uint32_t state[4])
{
    state[0] = WORD(task + 0x24U);
    state[1] = WORD(task + 0x28U);
    state[2] = WORD(task + 0x30U);
    state[3] = WORD(task + 0x34U);
}
static void stack_set(uint32_t task, const uint32_t state[4])
{
    WORD(task + 0x24U) = state[0];
    WORD(task + 0x28U) = state[1];
    WORD(task + 0x30U) = state[2];
    WORD(task + 0x34U) = state[3];
}
static bool guards(const PrivateWriter *writer)
{
    const uint32_t *top =
        writer->stack + writer->stack_bytes / 4U - PRIVATE_WRITER_GUARD_BYTES / 4U;
    for (unsigned i = 0; i < PRIVATE_WRITER_GUARD_BYTES / 4U; ++i)
        if (writer->stack[i] != STACK_GUARD || top[i] != STACK_GUARD)
            return false;
    return true;
}
static void transfer(PrivateWriter *writer)
{
    uint32_t elapsed = writer->slice_start - counter();
    if (elapsed > writer->max_slice_ticks)
        writer->max_slice_ticks = elapsed;
    if (mask() != 0xC0U) {
        ++writer->mask_changes;
        lock();
    }
    if (WORD(TASK_POINTER) != writer->task)
        ++writer->task_changes;
    writer->dispatch_delta = WORD(writer->task + 0x1CU) - writer->dispatch_start;
    stack_get(writer->task, writer->worker_stack);
    if (writer->worker_stack[3] < writer->minimum_stack_remaining)
        writer->minimum_stack_remaining = writer->worker_stack[3];
    writer->worker_errno = *writer->native_errno;
    *writer->native_errno = writer->foreground_errno;
    WORD(writer->spi_device + 112U) = writer->original_callback;
    stack_set(writer->task, writer->original_stack);
    writer->running = false;
    executing = NULL;
    private_context_swap(&writer->worker_sp, writer->foreground_sp);
}
static void callback(void *ioc, uint32_t *request)
{
    PrivateWriter *writer = executing;
    uint32_t sp;
    __asm__ volatile("mov %0,sp" : "=r"(sp));
    /* Never switch a foreign task's stack if native code unexpectedly
     * dispatches despite our masked entry. This path must tail-call the OS
     * callback (checked in the ARM replay/disassembly) so it retains no
     * application return address on a foreign stack. */
    if (!writer || WORD(TASK_POINTER) != writer->task || sp < writer->worker_stack[0] ||
        sp > writer->worker_stack[1]) {
        if (writer)
            ++writer->task_changes;
        storage_mutation_invalidate();
        ((void (*)(void *, uint32_t *))SPI_CALLBACK)(ioc, request);
        return;
    }
    if (mask() != 0xC0U) {
        ++writer->mask_changes;
        storage_mutation_invalidate();
        ((void (*)(void *, uint32_t *))SPI_CALLBACK)(ioc, request);
        return;
    }
    ++writer->callbacks;
    if (request[0] >= 1000U && request[0] < 1008U)
        ++writer->commands[request[0] - 1000U];
    if (WORD(writer->task + 0x38U))
        ++writer->protected_callbacks;
    else if (!spi_idle())
        ++writer->busy_callbacks;
    else if ((uint32_t)(writer->slice_start - counter()) >= writer->budget) {
        ++writer->yields;
        ++writer->spi_yields;
        transfer(writer);
    }
    /* The verified NAND wrappers construct this descriptor on the private
     * stack. Do not chase an unfamiliar pointer just to enable an optimization.
     * Observe after any cooperative yield but before the native command starts. */
    uint32_t descriptor = request[0] >= 1001U && request[0] <= 1004U ? request[4] : 0U;
    if ((descriptor & 3U) || descriptor < writer->worker_stack[0] ||
        descriptor > writer->worker_stack[1] - 20U)
        storage_mutation_observe_spi(request[0], NULL);
    else
        storage_mutation_observe_spi(request[0], (const uint32_t *)(uintptr_t)descriptor);
    uint32_t started = counter();
    ((void (*)(void *, uint32_t *))(uintptr_t)writer->original_callback)(ioc, request);
    uint32_t ticks = started - counter();
    if (request[2])
        storage_mutation_callback_failed();
    if (ticks > writer->max_callback_ticks)
        writer->max_callback_ticks = ticks;
}
void private_writer_cooperate(void)
{
    PrivateWriter *writer = executing;
    if (writer && WORD(TASK_POINTER) == writer->task && mask() == 0xC0U &&
        !WORD(writer->task + 0x38U) && spi_idle()) {
        ++writer->yields;
        transfer(writer);
    }
}
static void entry(void *argument)
{
    PrivateWriter *writer = argument;
    writer->job(writer->argument);
    if (WORD(writer->task + 0x38U) || !spi_idle())
        writer->error = PW_UNSAFE_RETURN;
    writer->done = true;
    writer->pending = false;
    transfer(writer);
    for (;;) {
    } /* A completed fiber must never be resumed. */
}
const char *private_writer_init_stage_name(unsigned stage)
{
    switch (stage) {
    case PW_INIT_NONE:
        return "ok";
    case PW_INIT_ARGUMENT:
        return "argument";
    case PW_INIT_ENTRY_IRQ:
        return "entry_irq_unmasked";
    case PW_INIT_PLATFORM:
        return "platform";
    case PW_INIT_TABLE_ADDRESS:
        return "table_address";
    case PW_INIT_TABLE_ALIAS:
        return "table_alias";
    case PW_INIT_TASK_SPAN:
        return "task_span";
    case PW_INIT_TASK_MAGIC:
        return "task_magic";
    case PW_INIT_TASK_PROTECTED:
        return "task_protected";
    case PW_INIT_STACK_SPAN:
        return "stack_span";
    case PW_INIT_SPI_SPAN:
        return "spi_span";
    case PW_INIT_SPI_DESCRIPTOR:
        return "spi_descriptor";
    case PW_INIT_SPI_BUSY:
        return "spi_busy";
    case PW_INIT_ERRNO_SPAN:
        return "errno_span";
    default:
        return "unknown";
    }
}
unsigned private_writer_init(PrivateWriter *writer, void *stack, size_t bytes)
{
    /* Do not overwrite a live context's diagnostics on an invalid re-init. */
    if (!writer || writer->initialized)
        return PW_BAD_ARGUMENT;
    PrivateWriterInitDiagnostic *diagnostic = &writer->init_diagnostic;
    memset(diagnostic, 0, sizeof(*diagnostic));
    diagnostic->stage = PW_INIT_ARGUMENT;
    diagnostic->stack_address = (uint32_t)(uintptr_t)stack;
    diagnostic->stack_bytes = (uint32_t)bytes;
    diagnostic->valid = PW_INIT_HAVE_STACK_ARGUMENT;
    if (!stack || ((uintptr_t)stack & 7U) || bytes < PRIVATE_WRITER_STACK_BYTES ||
        bytes > 0x80000U || (bytes & 7U) || executing)
        return PW_BAD_ARGUMENT;
    unsigned saved = mask();
    diagnostic->entry_mask = saved;
    diagnostic->valid |= PW_INIT_HAVE_ENTRY_MASK;
    if (!(saved & 0x80U)) {
        diagnostic->stage = PW_INIT_ENTRY_IRQ;
        return PW_BAD_TASK;
    }
    if (!private_writer_supported()) {
        diagnostic->stage = PW_INIT_PLATFORM;
        unlock(saved);
        return PW_UNSUPPORTED;
    }
    lock();
    unsigned error = PW_BAD_TASK;
    uint32_t task = WORD(TASK_POINTER), device = WORD(SPI_POINTER);
    diagnostic->task = task;
    diagnostic->spi_device = device;
    diagnostic->valid |= PW_INIT_HAVE_TASK_ADDRESS | PW_INIT_HAVE_SPI_ADDRESS;
    if (!span(task, 0x44U, diagnostic, PW_INIT_TASK_SPAN))
        goto finish;
    diagnostic->task_magic = WORD(task + 0xCU);
    diagnostic->task_protection = WORD(task + 0x38U);
    diagnostic->valid |= PW_INIT_HAVE_TASK_FIELDS;
    if (diagnostic->task_magic != 0x5441534BU) {
        diagnostic->stage = PW_INIT_TASK_MAGIC;
        goto finish;
    }
    if (diagnostic->task_protection) {
        diagnostic->stage = PW_INIT_TASK_PROTECTED;
        goto finish;
    }
    if (!span((uint32_t)(uintptr_t)stack, (uint32_t)bytes, diagnostic, PW_INIT_STACK_SPAN))
        goto finish;
    error = PW_BAD_SPI;
    if (!span(device, 132U, diagnostic, PW_INIT_SPI_SPAN))
        goto finish;
    if (WORD(device + 116U) || WORD(device + 64U) != device || WORD(device + 72U) != 0x10074FCCU ||
        WORD(device + 100U) != 16U || WORD(device + 112U) != SPI_CALLBACK ||
        WORD(device + 76U) != 0x10C534F4U || WORD(0x10C534F4U) != 0xB8000000U) {
        diagnostic->stage = PW_INIT_SPI_DESCRIPTOR;
        goto finish;
    }
    if (!spi_idle()) {
        diagnostic->stage = PW_INIT_SPI_BUSY;
        goto finish;
    }
    int *native_errno = ((int *(*)(void))0x1048D768U)();
    diagnostic->errno_address = (uint32_t)(uintptr_t)native_errno;
    diagnostic->valid |= PW_INIT_HAVE_ERRNO_ADDRESS;
    if (!span((uint32_t)(uintptr_t)native_errno, 4U, diagnostic, PW_INIT_ERRNO_SPAN)) {
        error = PW_BAD_TASK;
        goto finish;
    }
    diagnostic->stage = PW_INIT_NONE;
    PrivateWriterInitDiagnostic observation = *diagnostic;
    memset(writer, 0, sizeof(*writer));
    writer->init_diagnostic = observation;
    writer->task = task;
    writer->spi_device = device;
    writer->original_callback = SPI_CALLBACK;
    writer->stack = stack;
    writer->stack_bytes = (uint32_t)bytes;
    writer->native_errno = native_errno;
    writer->minimum_stack_remaining = (uint32_t)bytes;
    writer->initialized = true;
    error = PW_OK;
finish:
    unlock(saved);
    return error;
}
unsigned private_writer_submit(PrivateWriter *writer, void (*job)(void *), void *argument)
{
    if (!writer || !writer->initialized || !job)
        return PW_BAD_ARGUMENT;
    if (writer->pending || writer->running || executing)
        return PW_BUSY;
    if (writer->error)
        return writer->error;
    for (unsigned i = 0; i < writer->stack_bytes / 4U; ++i)
        writer->stack[i] = STACK_GUARD;
    uint32_t base = (uint32_t)(uintptr_t)writer->stack + PRIVATE_WRITER_GUARD_BYTES;
    uint32_t top =
        (uint32_t)(uintptr_t)writer->stack + writer->stack_bytes - PRIVATE_WRITER_GUARD_BYTES;
    writer->worker_stack[0] = base;
    writer->worker_stack[1] = top;
    writer->worker_stack[2] = top - base;
    writer->worker_stack[3] = top - base;
    uint32_t *frame = (uint32_t *)(uintptr_t)top - 10;
    memset(frame, 0, 40U);
    frame[0] = (uint32_t)(uintptr_t)writer;
    frame[1] = (uint32_t)(uintptr_t)entry;
    __asm__ volatile("mov %0,r9" : "=r"(frame[5]));
    frame[9] = (uint32_t)(uintptr_t)private_context_entry;
    writer->worker_sp = frame;
    writer->job = job;
    writer->argument = argument;
    writer->worker_errno = 0;
    writer->done = false;
    writer->pending = true;
    writer->dispatch_start = WORD(writer->task + 0x1CU);
    return PW_OK;
}
bool private_writer_step(PrivateWriter *writer, uint32_t budget_ticks)
{
    if (!writer || !writer->initialized || !writer->pending || executing || !budget_ticks)
        return false;
    unsigned saved = lock();
    if (WORD(TASK_POINTER) != writer->task || WORD(writer->task + 0x38U) ||
        WORD(writer->spi_device + 112U) != writer->original_callback || !spi_idle()) {
        writer->error = PW_STATE_CHANGED;
        storage_mutation_invalidate();
        unlock(saved);
        return false;
    }
    stack_get(writer->task, writer->original_stack);
    writer->foreground_errno = *writer->native_errno;
    *writer->native_errno = writer->worker_errno;
    stack_set(writer->task, writer->worker_stack);
    WORD(writer->spi_device + 112U) = (uint32_t)(uintptr_t)callback;
    executing = writer;
    writer->running = true;
    writer->budget = budget_ticks;
    writer->slice_start = counter();
    ++writer->resumes;
    private_context_swap(&writer->foreground_sp, writer->worker_sp);
    if (!guards(writer))
        writer->error = PW_STACK_CORRUPT;
    if (writer->dispatch_delta || writer->mask_changes || writer->task_changes)
        writer->error = PW_STATE_CHANGED;
    if (writer->error)
        storage_mutation_invalidate();
    unlock(saved);
    return true;
}
bool private_writer_restored(const PrivateWriter *writer)
{
    if (!writer || !writer->initialized || writer->pending || writer->running || executing)
        return false;
    uint32_t state[4];
    stack_get(writer->task, state);
    return WORD(TASK_POINTER) == writer->task && !WORD(writer->task + 0x38U) &&
           WORD(writer->spi_device + 112U) == writer->original_callback &&
           memcmp(state, writer->original_stack, 3U * sizeof(uint32_t)) == 0 &&
           state[3] <= writer->original_stack[3] && guards(writer) && spi_idle();
}
