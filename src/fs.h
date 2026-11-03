#ifndef FS_H
#define FS_H
#include <uv.h>

typedef struct fs_file fs_file_t;

typedef void (*fs_open_cb)(int status, fs_file_t *file, void *userdata);

void fs_open(uv_loop_t *loop, const char *root, const char *target, fs_open_cb cb, void *userdata);

const char *fs_file_path(const fs_file_t *file);

size_t fs_file_size(const fs_file_t *file);

typedef void (*fs_chunk_cb)(const char *data, size_t len, int status, void *userdata);

void fs_file_read(fs_file_t *file, fs_chunk_cb cb, void *userdata);

void fs_file_close(fs_file_t *file);

#endif
