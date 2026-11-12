#ifndef EPUB_CACHE_H
#define EPUB_CACHE_H
#include <stdbool.h>
#include <stddef.h>

typedef struct { int directory, lock; } epub_cache_t;
#define EPUB_CACHE_INIT { -1, -1 }
bool epub_cache_acquire(epub_cache_t *cache, const char *root, char *path, size_t capacity);
void epub_cache_release(epub_cache_t *cache);
void epub_cache_recover_legacy(const char *root, bool (*cancelled)(void *), void *user);
#endif
