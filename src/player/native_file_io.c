#include "native_file_io.h"
#include "native_interrupts.h"
#include "native_firmware.h"
#include <string.h>
#include <os.h>

typedef struct {
    void *(*file_open)(const char *, const char *);
    unsigned (*file_write)(const void *, unsigned, unsigned, void *);
    int (*file_close)(void *);
    int (*file_remove)(const char *);
    int (*file_stat)(const char *, void *);
    int *(*errno_address)(void);
} NativeFileApi;
static const NativeFileApi native_api = {
    (void *(*)(const char *, const char *))0x10423C64U,
    (unsigned (*)(const void *, unsigned, unsigned, void *))0x1042429CU,
    (int (*)(void *))0x104236E4U,
    (int (*)(const char *))0x10424658U,
    (int (*)(const char *, void *))0x10493C78U,
    (int *(*)(void))0x1048D768U};
static int file_platform(void)
{
#if defined(__arm__)
    static const struct {
        uintptr_t address;
        unsigned word[4];
    } fingerprints[] = {{0x10423C64U, {0xE92D40F0U, 0xE1A07000U, 0xE24DD044U, 0xE1A06001U}},
                        {0x10423E08U, {0xE92D45F8U, 0xE2534000U, 0xE1A06000U, 0xE1A07001U}},
                        {0x104236E4U, {0xE92D4038U, 0xE1A04000U, 0xEBFFFFC4U, 0xE3500000U}},
                        {0x1042429CU, {0xE92D47F0U, 0xE2534000U, 0xE1A05000U, 0xE1A07001U}},
                        {0x10424658U, {0xE92D4030U, 0xE24DDF41U, 0xE1A04000U, 0xEB01AE14U}},
                        {0x10493C78U, {0xE92D4070U, 0xE24DDD09U, 0xE1A05001U, 0xE1A04000U}},
                        {0x1048D768U, {0xE92D4008U, 0xEBFE6EA3U, 0xE3500000U, 0x059F0014U}},
                        {0x10429200U, {0xE59F3028U, 0xE5930000U, 0xE3500000U, 0x1A000001U}}};
    if (!native_firmware_memory())
        return NATIVE_FILE_IO_UNSUPPORTED;
    for (unsigned i = 0; i < sizeof(fingerprints) / sizeof(*fingerprints); ++i)
        for (unsigned j = 0; j < 4; ++j)
            if (((const volatile unsigned *)fingerprints[i].address)[j] != fingerprints[i].word[j])
                return NATIVE_FILE_IO_UNSUPPORTED;
    if (*(const volatile unsigned *)0x10429230U != 0x1148F060U)
        return NATIVE_FILE_IO_UNSUPPORTED;
    uintptr_t address = *(const volatile unsigned *)0x1148F060U;
    if ((address & 3U) || address < 0x10000000U || address > 0x11FFFFB8U)
        return NATIVE_FILE_IO_BAD_TASK;
    const volatile unsigned char *task = (const volatile unsigned char *)address;
    /* Retain the verified launch-state restrictions. These are read-only;
     * the adapter never changes a native task or scheduler configuration. */
    if (*(const volatile unsigned *)(task + 0x0c) != 0x5441534BU || !task[0x1b] ||
        !*(const volatile unsigned *)(task + 0x40) || *(const volatile unsigned *)0x1148F080U != 0U)
        return NATIVE_FILE_IO_BAD_TASK;
    return NATIVE_FILE_IO_OK;
#else
    return NATIVE_FILE_IO_UNSUPPORTED;
#endif
}
int native_file_io_init(NativeFileIo *backend)
{
    if (!backend)
        return NATIVE_FILE_IO_BAD_STATE;
    if (backend->io_phase)
        return NATIVE_FILE_IO_BUSY;
    unsigned saved = native_interrupt_mask();
    memset(backend, 0, sizeof(*backend));
    int result = file_platform();
    native_critical_leave(saved);
    if (result)
        return result;
    if (!(native_interrupt_mask() & 0x80U))
        return NATIVE_FILE_IO_BAD_TASK;
    backend->supported = true;
    return NATIVE_FILE_IO_OK;
}

void *native_file_io_open(NativeFileIo *backend, const char *path)
{
    if (!backend || !backend->supported || !path)
        return NULL;
    backend->io_phase = 4U;
    void *file = native_api.file_open(path, "rb");
    backend->io_phase = 0;
    return file;
}

void *native_file_io_open_write(NativeFileIo *backend, const char *path)
{
    if (!backend || !backend->supported || !path)
        return NULL;
    backend->io_phase = 4U;
    void *file = native_api.file_open(path, "wb");
    backend->io_phase = 0;
    return file;
}

void *native_file_io_open_append(NativeFileIo *backend, const char *path)
{
    if (!backend || !backend->supported || !path)
        return NULL;
    backend->io_phase = 4U;
    void *file = native_api.file_open(path, "ab");
    backend->io_phase = 0;
    return file;
}

size_t native_file_io_write(NativeFileIo *backend, void *file, const void *buffer, size_t bytes)
{
    unsigned requested = (unsigned)bytes;
    if (!backend || !backend->supported || !file || !buffer || (size_t)requested != bytes)
        return 0;
    backend->io_phase = 2U;
    size_t result = (size_t)native_api.file_write(buffer, 1U, requested, file);
    backend->io_phase = 0;
    return result;
}

int native_file_io_close(NativeFileIo *backend, void *file)
{
    if (!backend || !backend->supported || !file)
        return NATIVE_FILE_IO_BAD_STATE;
    backend->io_phase = 8U;
    int result = native_api.file_close(file);
    backend->io_phase = 0;
    return result;
}

int native_file_io_exists(NativeFileIo *backend, const char *path, int *native_error)
{
    /* The verified native stat writes through offset32 (36 bytes total).
     * Keep an aligned opaque buffer so this never depends on newlib's stat ABI. */
    unsigned information[16];
    int *task_errno;
    int result, error;
    if (native_error)
        *native_error = 0;
    if (!backend || !backend->supported || !path) {
        if (native_error)
            *native_error = NATIVE_FILE_IO_BAD_STATE;
        return -1;
    }
    /* errno_addr indexes the current native task's 812-byte runtime record.
     * It is independent of newlib errno and of the other application context. */
    task_errno = native_api.errno_address();
    if (!task_errno) {
        if (native_error)
            *native_error = NATIVE_FILE_IO_BAD_STATE;
        return -1;
    }
    *task_errno = 0;
    backend->io_phase = 16U;
    result = native_api.file_stat(path, information);
    backend->io_phase = 0;
    error = *task_errno;
    if (result == 0)
        return 1;
    if (native_error)
        *native_error = error;
    return error == 2 ? 0 : -1; /* Native ENOENT is explicitly stored as2. */
}

int native_file_io_remove(NativeFileIo *backend, const char *path)
{
    if (!backend || !backend->supported || !path)
        return NATIVE_FILE_IO_BAD_STATE;
    backend->io_phase = 64U;
    int result = native_api.file_remove(path);
    backend->io_phase = 0;
    return result;
}
