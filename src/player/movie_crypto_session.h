#ifndef NDVIDEO_MOVIE_CRYPTO_SESSION_H
#define NDVIDEO_MOVIE_CRYPTO_SESSION_H
#include "movie/nve_crypto.h"

/* One open movie owns the session. Readers borrow it until their handles
 * close; the main loop clears it before opening another video or exiting. */
const NveKeys *movie_crypto_keys(const char *path);
bool movie_crypto_install(const char *path, NveKeys *keys);
void movie_crypto_clear(void);
#endif
