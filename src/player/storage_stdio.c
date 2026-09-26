/* Relinquish storage before entering stdio, including its buffer allocation
 * and flush/close machinery. Read-only refills use the independent reader and
 * do not invalidate its warm filesystem view. */
#include "raw_player_io.h"
#include <dirent.h>

extern FILE *__real_fopen(const char *, const char *);
extern size_t __real_fread(void *, size_t, size_t, FILE *);
extern size_t __real_fwrite(const void *, size_t, size_t, FILE *);
extern int __real_fseek(FILE *, long, int);
extern int __real_fclose(FILE *);
extern int __real_fflush(FILE *);

FILE *__wrap_fopen(const char *path, const char *mode)
{
    raw_player_before_native();
    return __real_fopen(path, mode);
}
size_t __wrap_fread(void *data, size_t size, size_t count, FILE *file)
{
    raw_player_before_read();
    return __real_fread(data, size, count, file);
}
size_t __wrap_fwrite(const void *data, size_t size, size_t count, FILE *file)
{
    raw_player_before_native();
    return __real_fwrite(data, size, count, file);
}
int __wrap_fseek(FILE *file, long offset, int whence)
{
    raw_player_before_read();
    return __real_fseek(file, offset, whence);
}
int __wrap_fclose(FILE *file)
{
    raw_player_before_native();
    return __real_fclose(file);
}
int __wrap_fflush(FILE *file)
{
    raw_player_before_native();
    return __real_fflush(file);
}

extern int __real_remove(const char *);
extern int __real_rename(const char *, const char *);
extern DIR *__real_nuc_opendir(const char *);
extern struct dirent *__real_nuc_readdir(DIR *);
extern int __real_nuc_closedir(DIR *);

int __wrap_remove(const char *path)
{
    raw_player_before_native();
    return __real_remove(path);
}
int __wrap_rename(const char *from, const char *to)
{
    raw_player_before_native();
    return __real_rename(from, to);
}
DIR *__wrap_nuc_opendir(const char *path)
{
    raw_player_before_native();
    return __real_nuc_opendir(path);
}
struct dirent *__wrap_nuc_readdir(DIR *directory)
{
    raw_player_before_native();
    return __real_nuc_readdir(directory);
}
int __wrap_nuc_closedir(DIR *directory)
{
    raw_player_before_native();
    return __real_nuc_closedir(directory);
}
