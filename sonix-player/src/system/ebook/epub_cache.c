#include "epub_cache.h"
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const extensions[] = {"mp3", "m4a", "m4b", "mp4", "aac", "ogg", "opus", "flac", "wav", "wv"};
static bool resource_name(const char *name) {
    if (strncmp(name, "narration.", 10)) return false;
    for (size_t i = 0; i < sizeof(extensions) / sizeof(*extensions); i++)
        if (!strcmp(name + 10, extensions[i])) return true;
    return false;
}
static void clear_resources(int directory) {
    for (size_t i = 0; i < sizeof(extensions) / sizeof(*extensions); i++) {
        char name[32]; snprintf(name, sizeof(name), "narration.%s", extensions[i]);
        unlinkat(directory, name, 0);
    }
}
void epub_cache_release(epub_cache_t *cache) {
    if (cache->directory >= 0) clear_resources(cache->directory);
    if (cache->lock >= 0) { flock(cache->lock, LOCK_UN); close(cache->lock); }
    if (cache->directory >= 0) close(cache->directory);
    cache->directory = cache->lock = -1;
}
bool epub_cache_acquire(epub_cache_t *cache, const char *root, char *path, size_t capacity) {
    if (cache->lock >= 0) return true;
    if (!root || !root[0] || snprintf(path, capacity, "%s/.sonix-ebook-audio", root) >= (int)capacity) return false;
    struct stat st;
    if (mkdir(path, 0700) && (lstat(path, &st) || !S_ISDIR(st.st_mode))) return false;
    int dir = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) return false;
    int lock = openat(dir, ".lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock < 0 || fstat(lock, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 || flock(lock, LOCK_EX | LOCK_NB)) {
        if (lock >= 0) close(lock);
        close(dir); return false;
    }
    cache->directory = dir; cache->lock = lock;
    // An OS-released lock identifies an interrupted session. Only our fixed
    // resource names are removed; other card content is never traversed.
    clear_resources(dir);
    return true;
}
void epub_cache_recover_legacy(const char *root, bool (*cancelled)(void *), void *user) {
    DIR *card = opendir(root);
    if (!card) return;
    struct dirent *entry;
    while ((entry = readdir(card)) && (!cancelled || !cancelled(user))) {
        const char *name = entry->d_name;
        if (strncmp(name, ".ebook-audio-", 13) || strlen(name) != 19) continue;
        bool valid = true;
        for (const char *p = name + 13; *p; p++) if (!isalnum((unsigned char)*p)) valid = false;
        if (!valid) continue;
        int fd = openat(dirfd(card), name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) continue;
        DIR *dir = fdopendir(fd);
        if (!dir) { close(fd); continue; }
        char resource[32] = "";
        struct dirent *file;
        while ((file = readdir(dir))) {
            if (!strcmp(file->d_name, ".") || !strcmp(file->d_name, "..")) continue;
            struct stat st;
            if (resource[0] || !resource_name(file->d_name) || fstatat(fd, file->d_name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) || st.st_nlink != 1) { valid = false; break; }
            snprintf(resource, sizeof(resource), "%s", file->d_name);
        }
        if (valid && (!cancelled || !cancelled(user))) {
            if (resource[0]) unlinkat(fd, resource, 0);
            unlinkat(dirfd(card), name, AT_REMOVEDIR);
        }
        closedir(dir);
    }
    closedir(card);
}
