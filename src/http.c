#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "picohttpparser.h"

#include "connection.h"
#include "fs.h"
#include "http.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define HTTP_MAX_HEADERS 16
#define HEAD_MAX 512

typedef struct
{
  const char *ext;
  const char *type;
} mime_t;

static const mime_t g_mimes[] = {
    {".html", "text/html; charset=utf-8"},
    {".htm", "text/html; charset=utf-8"},
    {".css", "text/css; charset=utf-8"},
    {".js", "text/javascript; charset=utf-8"},
    {".json", "application/json"},
    {".txt", "text/plain; charset=utf-8"},
    {".png", "image/png"},
    {".gif", "image/gif"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".svg", "image/svg+xml"},
    {".ico", "image/x-icon"},
};

const char *mime_type(const char *path)
{
  const char *base = path;
  const char *dot;
  const char *p;
  size_t i;

  for (p = path; *p; p++)
    if (*p == '/' || *p == '\\')
      base = p + 1;

  dot = strrchr(base, '.');
  if (dot == NULL)
    return "application/octet-stream";

  for (i = 0; i < ARRAY_LEN(g_mimes); i++)
    if (strcmp(dot, g_mimes[i].ext) == 0)
      return g_mimes[i].type;

  return "application/octet-stream";
}

/* Builds the header block, the connection owns it from here on. NULL means the
 * response cannot be built at all. */
char *http_head(size_t *len, int status, const char *reason, const char *ctype, size_t body_len)
{
  char *head = malloc(HEAD_MAX);
  int n;

  if (head == NULL)
    return NULL;

  n = snprintf(head, HEAD_MAX,
               "HTTP/1.1 %d %s\r\n"
               "Content-Length: %zu\r\n"
               "Content-Type: %s\r\n"
               "Connection: close\r\n"
               "\r\n",
               status, reason, body_len, ctype);
  if (n < 0 || (size_t)n >= HEAD_MAX)
  {
    free(head);
    return NULL;
  }

  *len = (size_t)n;
  return head;
}

void http_error(conn_t *conn, int status, const char *reason)
{
  size_t len = 0;
  char *head = http_head(&len, status, reason, "text/plain; charset=utf-8", 0);

  conn_respond(conn, head, head == NULL ? 0 : len);
}

void on_fs_open(int status, fs_file_t *file, void *userdata)
{
  conn_t *conn = userdata;
  size_t len = 0;
  char *head;

  if (status == 0)
  {
    head = http_head(&len, 200, "OK", mime_type(fs_file_path(file)), fs_file_size(file));
    if (head != NULL)
    {
      conn_send_file(conn, head, len, file);
      conn_unref(conn);
      return;
    }

    fs_file_close(file);
    status = UV_ENOMEM;
  }

  if (status == UV_ENOENT || status == UV_ENOTDIR)
    http_error(conn, 404, "Not Found");
  else if (status == UV_EACCES)
    http_error(conn, 403, "Forbidden");
  else
    http_error(conn, 500, "Internal Server Error");

  conn_unref(conn);
}

void http_handle_request(conn_t *conn, const char *raw, size_t raw_len)
{
  const char *method;
  const char *path;
  size_t method_len;
  size_t path_len;
  int minor_version;
  struct phr_header headers[HTTP_MAX_HEADERS];
  size_t num_headers = ARRAY_LEN(headers);
  char target[PATH_MAX + 1];
  int n;

  n = phr_parse_request(raw, raw_len, &method, &method_len, &path, &path_len, &minor_version, headers, &num_headers, 0);
  if (n < 0)
    return http_error(conn, 400, "Bad Request");

  if (method_len != 3 || memcmp(method, "GET", 3) != 0)
    return http_error(conn, 405, "Method Not Allowed");

  if (minor_version != 1)
    return http_error(conn, 505, "HTTP Version Not Supported");

  if (path_len > PATH_MAX)
    return http_error(conn, 414, "URI Too Long");

  memcpy(target, path, path_len);
  target[path_len] = '\0';

  conn_ref(conn);
  fs_open(conn_loop(conn), conn_root(conn), target, on_fs_open, conn);
}
