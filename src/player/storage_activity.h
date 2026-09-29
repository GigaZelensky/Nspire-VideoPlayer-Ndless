#ifndef NDVIDEO_STORAGE_ACTIVITY_H
#define NDVIDEO_STORAGE_ACTIVITY_H
#include <stdbool.h>
#include <stdint.h>
/* Transaction ownership for the app's cooperative file-writing contexts,
 * including open/close. This is storage serialization, not an OS scheduler:
 * another writer or a physical read waits until the transaction ends. Crypto
 * on private RAM buffers does not access the shared storage device. */
void storage_native_begin(void);
void storage_native_end(void);
bool storage_native_active(void);
/* Changes at every native transaction, including RAM-only filesystem work. */
uint32_t storage_native_revision(void);
#endif
