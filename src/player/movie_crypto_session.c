#include "movie_crypto_session.h"
#include <string.h>

static NveKeys active_keys;
static char active_path[1024];

const NveKeys *movie_crypto_keys(const char *path)
{
    return active_keys.valid && path && !strcmp(path, active_path) ? &active_keys : NULL;
}
void movie_crypto_clear(void)
{
    nve_wipe(&active_keys, sizeof(active_keys));
    nve_wipe(active_path, sizeof(active_path));
}
bool movie_crypto_install(const char *path, NveKeys *keys)
{
    if (!path || !keys || !keys->valid || strlen(path) >= sizeof(active_path)) return false;
    movie_crypto_clear();
    memcpy(active_path, path, strlen(path) + 1U);
    active_keys = *keys;
    nve_wipe(keys, sizeof(*keys));
    return true;
}
