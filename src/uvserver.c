#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <limits.h>
#include <ctype.h>
#include "argparse.h"
#include "picohttpparser.h"

#define DEFAULT_BACKLOG 6767
#define DEFAULT_TIMEOUT_S 67
// https://zh.cppreference.net/c/preprocessor/replace.html
#define UV_CHECK(expr)                                                \
  do                                                                  \
  {                                                                   \
    int uv_err = (expr);                                              \
    if (uv_err < 0)                                                   \
    {                                                                 \
      fprintf(stderr, "error: %s: %s\n", #expr, uv_strerror(uv_err)); \
      _ret = uv_err;                                                  \
      goto cleanup;                                                   \
    }                                                                 \
  } while (0)

#undef DELETE
#define DELETE(x) \
  do              \
  {               \
    free(x);      \
    x = NULL;     \
  } while (0)

int _ret = 0;
char *g_directory = NULL;

int parse_args(int argc, const char **argv, struct sockaddr_in *addr)
{
  char *arg_bind = NULL;
  char *arg_directory = NULL;
  // sentinel-terminated array, NULL as end marker is required by argparse_usage() to know when to stop iterating
  static const char *const usages[] = {"uvserver.exe [-h] [-b ADDRESS] [-d DIRECTORY] [port]", NULL};
  struct argparse_option options[] = {
      OPT_HELP(),
      OPT_GROUP("options:"),
      OPT_STRING('b', "bind", &arg_bind, "bind to this address (default: 127.0.0.1)", NULL, 0, 0),
      OPT_STRING('d', "directory", &arg_directory, "serve this directory (default: current directory)", NULL, 0, 0),
      OPT_END(),
  };
  struct argparse argparse;
  argparse_init(&argparse, options, usages, 0);
  argparse_describe(&argparse, "\nA brief description of what the program does and how it works.", "");
  argc = argparse_parse(&argparse, argc, argv);
  // argparse_help_cb will exit app, so no need to handle that
  if (argc < 1)
  {
    fprintf(stderr, "argument error: missing required argument 'port'\n");
    argparse_usage(&argparse);
    return 1;
  }
  else
  {
    int port = atoi(argv[0]);
    if (port <= 0 || port > 65535)
    {
      fprintf(stderr, "argument error: invalid port number\n");
      return 1;
    }
    if (arg_bind)
      UV_CHECK(uv_ip4_addr(arg_bind, port, addr));
    else
      UV_CHECK(uv_ip4_addr("127.0.0.1", port, addr));
    if (arg_directory)
      g_directory = arg_directory;
    else
      g_directory = ".";
  }
  return 0;
cleanup:
  return 1;
}

void fs_cb(uv_fs_t *req)
{
  printf("fs_cb called\n");
}

// returns a heap allocated response the caller must free, NULL on failure
char *serve_http_response(const char *raw_request, size_t raw_len)
{
  const char *method, *path;
  int minor_version;
  size_t method_len, path_len;
  struct phr_header headers[16];
  size_t num_headers = sizeof(headers) / sizeof(headers[0]);
  size_t capacity;
  char *response;
  if (phr_parse_request(
          raw_request, raw_len, &method, &method_len,
          &path, &path_len, &minor_version, headers,
          &num_headers, 0) < 0)
  {
    fprintf(stderr, "Failed to parse HTTP request\n");
    return NULL;
  }
  // status line and headers always fit in 256 bytes, the body is body_prefix + path
  capacity = 65536;
  response = malloc(capacity);
  if (response == NULL)
  {
    fprintf(stderr, "out of memory\n");
    return NULL;
  }
  if (strncmp(method, "GET", method_len) != 0)
  {
    snprintf(response, capacity,
             "HTTP/1.1 405 Method Not Allowed\r\n"
             "Content-Length: 0\r\n"
             "Connection: close\r\n"
             "\r\n");
    return response;
  }
  if (minor_version != 1)
  {
    snprintf(response, capacity,
             "HTTP/1.1 505 HTTP Version Not Supported\r\n"
             "Content-Length: 0\r\n"
             "Connection: close\r\n"
             "\r\n");
    return response;
  }

  // TODO: use uv_fs_access
  // path process rule:
  // ends with a slash, append index.html
  // does not end with a slash or ext, try access barely, then +.html, if exists, serve it, else 404
  char full_path[PATH_MAX];
  // ! abslotely not safe lfmao
  // TODO: sanitize path, check symlinks
  snprintf(full_path, sizeof(full_path), "%s/%.*s", g_directory, (int)path_len, path);
  if (full_path[strlen(full_path) - 1] == '/')
    strncat(full_path, "index.html", sizeof(full_path) - strlen(full_path) - 1);
  if (access(full_path, F_OK) != 0)
  {
    // TODO: should be more strict
    strncat(full_path, ".html", sizeof(full_path) - strlen(full_path) - 1);
    if (access(full_path, F_OK) != 0)
    {
      snprintf(response, capacity,
               "HTTP/1.1 404 Not Found\r\n"
               "Content-Length: 0\r\n"
               "Connection: close\r\n"
               "\r\n");
      return response;
    }
  }
  uv_fs_t *req = malloc(sizeof(uv_fs_t));
  uv_file fd = uv_fs_open(uv_default_loop(),
                          req,
                          full_path,
                          O_RDONLY,
                          S_IRUSR, NULL);
  // fuck async
  // fuck async
  // fuck async
  const uv_buf_t bufs[] = {uv_buf_init(NULL, 0)};
  uv_fs_read(uv_default_loop(), req, fd, bufs, 1, 0, fs_cb);
  printf("buf: %s\n", bufs[0].base);
  snprintf(response, capacity,
           "HTTP/1.1 200 OK\r\n"
           "Content-Length: 4\r\n"
           "Connection: close\r\n"
           "\r\n"
           "6767");
  return response;

  // snprintf(response, capacity,
  //          "HTTP/1.1 500 Internal Server Error\r\n"
  //          "Content-Length: 0\r\n"
  //          "Connection: close\r\n"
  //          "\r\n");
  // return response;
}

void onClientClosed(uv_handle_t *handle)
{
  free(handle);
}

void onAlloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
  buf->base = malloc(suggested_size);
  buf->len = buf->base ? suggested_size : 0;
  handle = handle; // used lol
}

// on heap 67676767676767676767676767676767
typedef struct
{
  uv_write_t req;
  char *response;
} write_req_t;

void onWrite(uv_write_t *req, int status)
{
  write_req_t *write_req = (write_req_t *)req;
  uv_stream_t *stream = req->handle;
  uv_timer_t *timer = stream->data;

  if (status < 0)
    fprintf(stderr, "write error: %s\n", uv_strerror(status));

  free(write_req->response);

  if (timer != NULL && !uv_is_closing((uv_handle_t *)timer))
    uv_close((uv_handle_t *)timer, onClientClosed);
  uv_close((uv_handle_t *)stream, onClientClosed);
  free(write_req);
}

void onRead(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
  uv_timer_t *timer = stream->data;
  char *response = NULL;

  if (nread < 0)
  {
    if (nread != UV_EOF)
      fprintf(stderr, "read error: %s\n", uv_strerror(nread));
  }
  else if (nread > 0)
  {
    uv_timer_stop(timer);
    response = serve_http_response(buf->base, (size_t)nread);
  }

  // onAlloc() handed this buffer to us, so it is ours to release
  free(buf->base);

  if (response != NULL)
  {
    write_req_t *write_req = malloc(sizeof(write_req_t));
    if (write_req != NULL)
    {
      write_req->response = response;
      const uv_buf_t bufs[] = {uv_buf_init(response, strlen(response))};
      uv_read_stop(stream);
      int err = uv_write(&write_req->req, stream, bufs, 1, onWrite);
      if (err < 0)
      {
        fprintf(stderr, "write error: %s\n", uv_strerror(err));
        free(write_req->response);
        free(write_req);
      }
      else
        return;
    }
    else
    {
      fprintf(stderr, "out of memory\n");
      free(response);
    }
  }

  uv_read_stop(stream);
  if (timer != NULL && !uv_is_closing((uv_handle_t *)timer))
    uv_close((uv_handle_t *)timer, onClientClosed);
  uv_close((uv_handle_t *)stream, onClientClosed);
}

void onTimedout(uv_timer_t *timer)
{
  uv_tcp_t *client = (uv_tcp_t *)timer->data;

  // static: uv_write() does not copy, the buffer must outlive this call
  static const char response[] =
      "HTTP/1.1 408 Request Timeout\r\n"
      "Content-Length: 0\r\n"
      "Connection: close\r\n"
      "\r\n";

  if (!uv_is_closing((uv_handle_t *)timer))
    uv_close((uv_handle_t *)timer, onClientClosed);

  if (client == NULL || uv_is_closing((uv_handle_t *)client))
    return;

  // do not answer the timeout twice if more data shows up in the meantime
  uv_read_stop((uv_stream_t *)client);

  write_req_t *write_req = malloc(sizeof(write_req_t));

  if (write_req == NULL)
  {
    uv_close((uv_handle_t *)client, onClientClosed);
    return;
  }

  write_req->response = NULL;
  uv_buf_t buf = uv_buf_init((char *)response, sizeof(response) - 1);

  int err = uv_write(&write_req->req, (uv_stream_t *)client, &buf, 1, onWrite);

  if (err < 0)
  {
    fprintf(stderr, "write error: %s\n", uv_strerror(err));
    free(write_req);
    uv_close((uv_handle_t *)client, onClientClosed);
  }
}

void onConnect(uv_stream_t *server, int status)
{
  if (status < 0)
  {
    fprintf(stderr, "connection error: %s\n", uv_strerror(status));
    return;
  }

  uv_tcp_t *client = malloc(sizeof(uv_tcp_t));
  uv_timer_t *timer = NULL;

  if (client == NULL)
  {
    fprintf(stderr, "out of memory\n");
    return;
  }

  UV_CHECK(uv_tcp_init(server->loop, client));
  client->data = NULL; // onWrite() looks the timer up here, keep it valid
  UV_CHECK(uv_accept(server, (uv_stream_t *)client));

  timer = malloc(sizeof(uv_timer_t));
  if (!timer)
  {
    uv_close((uv_handle_t *)client, onClientClosed);
    return;
  }

  if (uv_timer_init(server->loop, timer) < 0)
  {
    fprintf(stderr, "error: %s\n", "uv_timer_init");
    free(timer);
    uv_close((uv_handle_t *)client, onClientClosed);
    return;
  }

  timer->data = client;
  client->data = timer;

  UV_CHECK(uv_timer_start(timer, onTimedout, DEFAULT_TIMEOUT_S * 1000, 0));
  UV_CHECK(uv_read_start((uv_stream_t *)client, onAlloc, onRead));

  return;

cleanup:
  uv_close((uv_handle_t *)client, onClientClosed);
  if (timer)
    uv_close((uv_handle_t *)timer, onClientClosed);
}

int main(int argc, const char **argv)
{
  // parse config form command line arguments
  struct sockaddr_in addr = {0};
  if (parse_args(argc, argv, &addr) != 0)
    return 1;
  // init uvloop
  uv_loop_t *loop = uv_default_loop();
  if (loop == NULL)
  {
    fprintf(stderr, "loop error: `uv_default_loop()` failed to allocate memory for the default loop\n");
    return 1;
  }
  // init server
  uv_tcp_t server;
  UV_CHECK(uv_tcp_init(loop, &server));
  UV_CHECK(uv_tcp_bind(&server, (const struct sockaddr *)&addr, 0));
  UV_CHECK(uv_listen((uv_stream_t *)&server, DEFAULT_BACKLOG, onConnect));
  // enter loop
  printf("Listening on %s:%d\n", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
  return uv_run(loop, UV_RUN_DEFAULT);
cleanup:
  return _ret;
}
