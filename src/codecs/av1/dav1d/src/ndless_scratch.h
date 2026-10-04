#ifndef DAV1D_NDLESS_SCRATCH_H
#define DAV1D_NDLESS_SCRATCH_H

#include <stdint.h>

/* Bound by the single-thread adapter only while stepping a decoder. Motion
 * filtering and inverse transforms run in sequence. Compound predictions need
 * their own region while both references are filtered and blended. No pointer
 * to this workspace survives a complete decoder step. */
typedef struct Dav1dNdlessScratch {
    union {
        int16_t motion[128 * 135];
        int32_t transform[64 * 64];
    } temporary;
    int16_t compound[2][128 * 128];
} Dav1dNdlessScratch;
#define DAV1D_NDLESS_SCRATCH_BYTES sizeof(Dav1dNdlessScratch)
extern Dav1dNdlessScratch *dav1d_ndless_scratch;

#endif
