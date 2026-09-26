#ifndef NDVIDEO_JOURNAL_READER_H
#define NDVIDEO_JOURNAL_READER_H
#include "app_task_io.h"
#include <stdbool.h>
#include <stdint.h>
typedef struct JournalReader JournalReader;
/* Allocate and discover exports in the foreground, before starting a job. */
JournalReader *journal_reader_create(void);
void journal_reader_destroy(JournalReader *);
/* The calling app job holds storage_native_begin/end around open, all reads
 * and native close. Its native stream is read-only and remains at position 0. */
bool journal_reader_open(JournalReader *, void *native_stream);
int journal_reader_read(JournalReader *, AppTaskIo *, uint32_t offset, void *destination,
                        uint32_t bytes);
void journal_reader_end(JournalReader *);
#endif
