#if defined(NDVIDEO_BUILD_MODULE)
#include "module_runtime.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/reent.h>

static void *(*host_malloc)(size_t);
static void *(*host_calloc)(size_t, size_t);
static void *(*host_realloc)(void *, size_t);
static void (*host_free)(void *);
static void *(*host_memcpy)(void *, const void *, size_t);
static void *(*host_memmove)(void *, const void *, size_t);
static void *(*host_memset)(void *, int, size_t);
static int (*host_memcmp)(const void *, const void *, size_t);
static size_t (*host_strlen)(const char *);
static int (*host_strcmp)(const char *, const char *);
static int (*host_strncmp)(const char *, const char *, size_t);
static char *(*host_strchr)(const char *, int);
static char *(*host_strrchr)(const char *, int);
static size_t (*host_strcspn)(const char *, const char *);
static int (*host_vsnprintf)(char *, size_t, const char *, va_list);
static int (*host_vfprintf)(FILE *, const char *, va_list);
static int (*host_vsprintf)(char *, const char *, va_list);
static int (*host_strerror_r)(int, char *, size_t);
static char *(*host_getenv)(const char *);
static unsigned long (*host_strtoul)(const char *, char **, int);
static int (*host_fputs)(const char *, FILE *);
static int (*host_fflush)(FILE *);
static size_t (*host_fwrite)(const void *, size_t, size_t, FILE *);
static int (*host_fputc)(int, FILE *);
static int (*host_puts)(const char *);
static void (*host_abort)(void);
static int *(*host_errno)(void);
#if defined(NDVIDEO_BUILD_MPEG4_MODULE)
static int (*host_rand)(void);
static void (*host_srand)(unsigned);
static double (*host_log)(double);
static double (*host_sqrt)(double);
static int (*host_vsscanf)(const char *, const char *, va_list);
#endif
struct _reent *_impure_ptr;

#define BIND(field, name)                                                                          \
    do {                                                                                           \
        *(void **)(&(field)) = host->resolve(name);                                                \
        if (!(field))                                                                              \
            return false;                                                                          \
    } while (0)
bool module_runtime_bind(const CodecHostApi *host)
{
    if (!host || host->abi_version != CODEC_MODULE_ABI_VERSION ||
        host->struct_size < sizeof(*host) || !host->resolve)
        return false;
    BIND(host_malloc, "malloc");
    BIND(host_calloc, "calloc");
    BIND(host_realloc, "realloc");
    BIND(host_free, "free");
    BIND(host_memcpy, "memcpy");
    BIND(host_memmove, "memmove");
    BIND(host_memset, "memset");
    BIND(host_memcmp, "memcmp");
    BIND(host_strlen, "strlen");
    BIND(host_strcmp, "strcmp");
    BIND(host_strncmp, "strncmp");
    BIND(host_strchr, "strchr");
    BIND(host_strrchr, "strrchr");
    BIND(host_strcspn, "strcspn");
    BIND(host_vsnprintf, "vsnprintf");
    BIND(host_vfprintf, "vfprintf");
    BIND(host_fflush, "fflush");
    BIND(host_vsprintf, "vsprintf");
    BIND(host_strerror_r, "__xpg_strerror_r");
    BIND(host_getenv, "getenv");
    BIND(host_strtoul, "strtoul");
    BIND(host_fputs, "fputs");
    BIND(host_fwrite, "fwrite");
    BIND(host_fputc, "fputc");
    BIND(host_puts, "puts");
    BIND(host_abort, "abort");
    BIND(host_errno, "__errno");
#if defined(NDVIDEO_BUILD_MPEG4_MODULE)
    BIND(host_rand, "rand");
    BIND(host_srand, "srand");
    BIND(host_log, "log");
    BIND(host_sqrt, "sqrt");
    BIND(host_vsscanf, "vsscanf");
#endif
    struct _reent **impure = (struct _reent **)host->resolve("_impure_ptr");
    if (!impure || !*impure)
        return false;
    _impure_ptr = *impure;
    return true;
}
#undef BIND

/* Module-local libc thunks deliberately share the host allocator and stdio
 * objects. Constructors/destructors and atexit are managed by each module. */
void *malloc(size_t n)
{
    return host_malloc(n);
}

void *calloc(size_t n, size_t s)
{
    return host_calloc(n, s);
}

void *realloc(void *p, size_t n)
{
    return host_realloc(p, n);
}

void free(void *p)
{
    host_free(p);
}

void *memcpy(void *d, const void *s, size_t n)
{
    return host_memcpy(d, s, n);
}

void *memmove(void *d, const void *s, size_t n)
{
    return host_memmove(d, s, n);
}

void *memset(void *d, int v, size_t n)
{
    return host_memset(d, v, n);
}

int memcmp(const void *a, const void *b, size_t n)
{
    return host_memcmp(a, b, n);
}

size_t strlen(const char *s)
{
    return host_strlen(s);
}

int strcmp(const char *a, const char *b)
{
    return host_strcmp(a, b);
}

int strncmp(const char *a, const char *b, size_t n)
{
    return host_strncmp(a, b, n);
}

char *strchr(const char *s, int c)
{
    return host_strchr(s, c);
}

char *strrchr(const char *s, int c)
{
    return host_strrchr(s, c);
}

size_t strcspn(const char *s, const char *set)
{
    return host_strcspn(s, set);
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

int *__errno(void)
{
    return host_errno();
}

int vsnprintf(char *s, size_t n, const char *f, va_list a)
{
    return host_vsnprintf(s, n, f, a);
}

int snprintf(char *s, size_t n, const char *f, ...)
{
    va_list a;
    va_start(a, f);
    int r = vsnprintf(s, n, f, a);
    va_end(a);
    return r;
}

int vfprintf(FILE *s, const char *f, va_list a)
{
    return host_vfprintf(s, f, a);
}

int fprintf(FILE *s, const char *f, ...)
{
    va_list a;
    va_start(a, f);
    int r = vfprintf(s, f, a);
    va_end(a);
    return r;
}

int printf(const char *f, ...)
{
    va_list a;
    va_start(a, f);
    int r = vfprintf(stdout, f, a);
    va_end(a);
    return r;
}

int vprintf(const char *f, va_list a)
{
    return vfprintf(stdout, f, a);
}

int sprintf(char *s, const char *f, ...)
{
    va_list a;
    va_start(a, f);
    int r = host_vsprintf(s, f, a);
    va_end(a);
    return r;
}

int vsprintf(char *s, const char *f, va_list a)
{
    return host_vsprintf(s, f, a);
}

int __xpg_strerror_r(int e, char *s, size_t n)
{
    return host_strerror_r(e, s, n);
}

char *getenv(const char *n)
{
    return host_getenv(n);
}

unsigned long strtoul(const char *s, char **e, int b)
{
    return host_strtoul(s, e, b);
}

int fputs(const char *s, FILE *f)
{
    return host_fputs(s, f);
}

int fflush(FILE *f)
{
    return host_fflush(f);
}

size_t fwrite(const void *p, size_t s, size_t n, FILE *f)
{
    return host_fwrite(p, s, n, f);
}

int fputc(int c, FILE *f)
{
    return host_fputc(c, f);
}

int puts(const char *s)
{
    return host_puts(s);
}

void abort(void)
{
    host_abort();
    for (;;) {
    }
}
#if defined(NDVIDEO_BUILD_MPEG4_MODULE)
int rand(void)
{
    return host_rand();
}

void srand(unsigned value)
{
    host_srand(value);
}

double log(double value)
{
    return host_log(value);
}

double sqrt(double value)
{
    return host_sqrt(value);
}

int sscanf(const char *s, const char *f, ...)
{
    va_list a;
    va_start(a, f);
    int r = host_vsscanf(s, f, a);
    va_end(a);
    return r;
}
#endif
void module_runtime_shutdown(void)
{
    _impure_ptr = NULL;
}
#endif
