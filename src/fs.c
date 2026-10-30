#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fs.h"

#define FS_CHUNK (64 * 1024)

typedef enum
{
  TRY_PLAIN = 0, /* <root>/rel            */
  TRY_HTML,      /* <root>/rel.html       */
  TRY_INDEX,     /* <root>/rel/index.html */
} fs_try_t;

/* An opened file. One uv_fs request at a time, reused: read, read, ... close.
 * The chunk buffer is deliberately handed out as is, see fs_file_read(). */
struct fs_file
{
  uv_loop_t *loop;
  uv_fs_t req;
  uv_file fd;
  size_t size;
  size_t off;
  char path[PATH_MAX];
  char *chunk;
  fs_chunk_cb cb;
  void *cb_ud;
};

/* One fs_open() call in flight. The fallback state lives here, so the module
 * keeps nothing between requests. */
typedef struct
{
  uv_fs_t req;
  uv_loop_t *loop;
  const char *root;
  fs_open_cb cb;
  void *ud;
  fs_try_t attempt;
  size_t size;
  char rel[PATH_MAX];
  char path[PATH_MAX];
} fs_open_t;

void fs_open_try(fs_open_t *job);
void on_stat(uv_fs_t *req);
void on_open(uv_fs_t *req);

/* ---------------------------------------------------------------- target */

int hex_val(int c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

/* Decodes one path segment, refusing anything that could change its meaning:
 * NUL, separators, drive letters and control characters. */
int urldecode(const char *src, size_t n, char *dst, size_t dst_size)
{
  size_t i = 0;
  size_t k = 0;

  while (i < n)
  {
    int c = (unsigned char)src[i++];
    if (c == '%')
    {
      int hi = i + 1 < n ? hex_val((unsigned char)src[i]) : -1;
      int lo = i + 1 < n ? hex_val((unsigned char)src[i + 1]) : -1;
      if (hi < 0 || lo < 0)
        return -1; /* malformed escape: refuse, do not guess */
      c = hi * 16 + lo;
      i += 2;
    }
    if (c == 0 || c == '/' || c == '\\' || c == ':' || c < 0x20 || c == 0x7f)
      return -1;
    if (k + 1 >= dst_size)
      return -1;
    dst[k++] = (char)c;
  }

  return (int)k;
}

int has_extension(const char *rel)
{
  const char *base = rel;
  const char *p;

  for (p = rel; *p; p++)
    if (*p == '/')
      base = p + 1;

  return strchr(base, '.') != NULL;
}

/* "/a/../b/%69ndex.html?q=1" -> "b/index.html". Every segment is decoded
 * before it is classified, so "%2e%2e" is a parent hop and not a file name,
 * and "%2f" is refused outright: the segment stack never pops past the root,
 * which makes traversal structurally impossible, symlinks aside. */
int fs_normalize(const char *target, char *out, size_t out_size)
{
  size_t n = 0;
  size_t i = 0;
  size_t len = strcspn(target, "?#"); /* drop query and fragment */

  out[0] = '\0';

  while (i < len)
  {
    char seg[PATH_MAX];
    size_t start;
    int decoded;

    while (i < len && target[i] == '/')
      i++;
    start = i;
    while (i < len && target[i] != '/')
      i++;
    if (i == start)
      break;

    decoded = urldecode(target + start, i - start, seg, sizeof(seg));
    if (decoded < 0)
      return -1;

    if (decoded == 0 || (decoded == 1 && seg[0] == '.'))
      continue;
    if (decoded == 2 && seg[0] == '.' && seg[1] == '.')
    {
      while (n > 0 && out[n - 1] != '/')
        n--;
      if (n > 0)
        n--;
      out[n] = '\0';
      continue;
    }

    if (n > 0)
    {
      if (n + 1 >= out_size)
        return -1;
      out[n++] = '/';
    }
    if (n + (size_t)decoded >= out_size)
      return -1;
    memcpy(out + n, seg, (size_t)decoded);
    n += (size_t)decoded;
    out[n] = '\0';
  }

  if (n == 0)
    snprintf(out, out_size, "index.html");

  return 0;
}

void on_fs_read(uv_fs_t *req)
{
  fs_file_t *file = req->data;
  fs_chunk_cb cb = file->cb;
  void *ud = file->cb_ud;
  ssize_t n = req->result;

  uv_fs_req_cleanup(req);
  file->cb = NULL;
  file->cb_ud = NULL;

  if (n < 0)
    return cb(NULL, 0, (int)n, ud);

  file->off += (size_t)n;
  cb(n > 0 ? file->chunk : NULL, (size_t)n, 0, ud);
}

void fs_file_read(fs_file_t *file, fs_chunk_cb cb, void *userdata)
{
  uv_buf_t buf;
  size_t left;
  int err;

  left = file->size - file->off;
  if (left == 0)
    return cb(NULL, 0, 0, userdata); /* EOF, no syscall needed */

  if (file->chunk == NULL && (file->chunk = malloc(FS_CHUNK)) == NULL)
    return cb(NULL, 0, UV_ENOMEM, userdata);

  buf = uv_buf_init(file->chunk, (unsigned int)(left < FS_CHUNK ? left : FS_CHUNK));

  file->cb = cb;
  file->cb_ud = userdata;
  file->req.data = file;

  err = uv_fs_read(file->loop, &file->req, file->fd, &buf, 1, (int64_t)file->off, on_fs_read);
  if (err < 0)
    cb(NULL, 0, err, userdata);
}

void on_closed(uv_fs_t *req)
{
  fs_file_t *file = req->data;

  uv_fs_req_cleanup(req);
  free(file->chunk);
  free(file);
}

void fs_file_close(fs_file_t *file)
{
  int err;

  if (file == NULL)
    return;

  file->req.data = file;
  err = uv_fs_close(file->loop, &file->req, file->fd, on_closed);
  if (err < 0)
  {
    /* the loop could not allocate a request, nothing else to do */
    free(file->chunk);
    free(file);
  }
}

const char *fs_file_path(const fs_file_t *file) { return file->path; }

size_t fs_file_size(const fs_file_t *file) { return file->size; }

void fs_open_finish(fs_open_t *job, int status, fs_file_t *file)
{
  fs_open_cb cb = job->cb;
  void *ud = job->ud;

  free(job);
  cb(status, file, ud);
}

void on_open(uv_fs_t *req)
{
  fs_open_t *job = req->data;
  fs_file_t *file;
  int fd = (int)req->result;

  uv_fs_req_cleanup(req);

  if (fd < 0)
    return fs_open_finish(job, fd, NULL);

  file = calloc(1, sizeof(*file));
  if (file == NULL)
  {
    uv_fs_close(job->loop, &job->req, fd, NULL); /* blocking, but out of memory */
    uv_fs_req_cleanup(&job->req);
    return fs_open_finish(job, UV_ENOMEM, NULL);
  }

  file->loop = job->loop;
  file->fd = fd;
  file->size = job->size;
  snprintf(file->path, sizeof(file->path), "%s", job->path);

  fs_open_finish(job, 0, file);
}

void on_stat(uv_fs_t *req)
{
  fs_open_t *job = req->data;
  int is_dir = 0;
  int is_reg = 0;
  int err = (int)req->result;

  if (err == 0)
  {
    job->size = (size_t)req->statbuf.st_size;
    is_dir = S_ISDIR(req->statbuf.st_mode) != 0;
    is_reg = S_ISREG(req->statbuf.st_mode) != 0;
  }
  uv_fs_req_cleanup(req);

  if (err != 0)
  {
    /* a missing extensionless name may still be a .html file */
    if (err == UV_ENOENT && job->attempt == TRY_PLAIN && !has_extension(job->rel))
    {
      job->attempt = TRY_HTML;
      return fs_open_try(job);
    }
    return fs_open_finish(job, err, NULL);
  }

  if (is_dir)
  {
    if (job->attempt != TRY_INDEX)
    {
      job->attempt = TRY_INDEX;
      return fs_open_try(job);
    }
    return fs_open_finish(job, UV_ENOENT, NULL); /* a directory is not a file */
  }

  if (!is_reg)
    return fs_open_finish(job, UV_ENOENT, NULL);

  job->req.data = job;
  err = uv_fs_open(job->loop, &job->req, job->path, O_RDONLY, 0, on_open);
  if (err < 0)
    fs_open_finish(job, err, NULL);
}

void fs_open_try(fs_open_t *job)
{
  const char *suffix;
  int err;
  int n;

  suffix = job->attempt == TRY_HTML ? ".html" : job->attempt == TRY_INDEX ? "/index.html"
                                                                          : "";
  n = snprintf(job->path, sizeof(job->path), "%s/%s%s", job->root, job->rel, suffix);
  if (n < 0 || (size_t)n >= sizeof(job->path))
    return fs_open_finish(job, UV_ENAMETOOLONG, NULL);

  job->req.data = job;
  err = uv_fs_stat(job->loop, &job->req, job->path, on_stat);
  if (err < 0)
    fs_open_finish(job, err, NULL);
}

void fs_open(uv_loop_t *loop, const char *root, const char *target, fs_open_cb cb, void *userdata)
{
  fs_open_t *job = calloc(1, sizeof(*job));

  if (job == NULL)
    return cb(UV_ENOMEM, NULL, userdata);

  job->loop = loop;
  job->root = root;
  job->cb = cb;
  job->ud = userdata;
  job->attempt = TRY_PLAIN;

  if (fs_normalize(target, job->rel, sizeof(job->rel)) < 0)
    return fs_open_finish(job, UV_EACCES, NULL);

  fs_open_try(job);
}
