#ifndef NDVIDEO_RELIANCE_READER_H
#define NDVIDEO_RELIANCE_READER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Standalone core: no allocation, filesystem calls, interrupts or polling.
 * One immutable filesystem view per instance. Reinitialize after the OS can
 * change that view, after the provider has drained outstanding operations.
 * Drain before disposing the workspace too. All API calls belong to one
 * foreground owner; provider completion must be delivered to that owner. */
#define REL_CACHE_SLOTS 7U
typedef enum { REL_IDLE, REL_PROGRESS, REL_NEED_BLOCK, REL_DONE, REL_ERROR } RelStatus;
typedef enum {
    REL_OK,
    REL_BAD_ARGUMENT,
    REL_BAD_FORMAT,
    REL_UNSUPPORTED,
    REL_BAD_RANGE,
    REL_IO_ERROR
} RelError;
typedef struct {
    uint32_t block, bytes;
    uint64_t token;
} RelRequest;
typedef struct {
    uint32_t block, stamp;
    bool valid;
} RelCacheSlot;
typedef struct {
    uint8_t *workspace, *destination;
    uint32_t block_bytes, block_count, index_block, inode_id, shift, fanout;
    RelCacheSlot cache[REL_CACHE_SLOTS];
    uint32_t clock, pending_slot, file_block, file_mode, index_mode;
    uint32_t requested, copied, mapped_page, mapped_block;
    uint64_t serial, offset, index_length, file_length;
    RelRequest request;
    RelError error;
    bool initialized, active, pending, index_ready, file_ready, range_ready, mapped;
    bool pending_metadata;
} RelReader;

bool rel_reader_init(RelReader *, uint32_t block_bytes, uint32_t block_count,
                     uint32_t index_inode_block, uint32_t inode_id, void *workspace,
                     size_t workspace_bytes);
/* begin cancels any older job, retaining valid cache blocks in the same view. */
bool rel_reader_begin(RelReader *, uint64_t offset, void *destination, uint32_t bytes);
void rel_reader_cancel(RelReader *);
/* Copies at most min(copy_budget, one block) bytes. Repeated calls while a
 * request is pending return immediately with that same request, without I/O. */
RelStatus rel_reader_step(RelReader *, uint32_t copy_budget);
bool rel_reader_request(const RelReader *, RelRequest *);
/* Provider owns its input buffer. No pointer into internal cache storage is
 * exposed, so a canceled operation cannot overwrite a later job's storage.
 * A stale completion returns false without changing state. Do not DMA into
 * the workspace or overlap a supply source with its destination cache slot. */
bool rel_reader_supply(RelReader *, uint64_t token, const void *data, uint32_t bytes);
bool rel_reader_fail(RelReader *, uint64_t token);
#endif
