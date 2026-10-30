#include <stdio.h>
#include <uv.h>
#include <stdlib.h>
#include "argparse.h"

#define DEFAULT_BACKLOG 6767
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

struct config_s
{
  const char *bind_address;
  const char *directory;
  int port;
};
typedef struct config_s config_t;

int _ret = 0;

int parse_args(int argc, const char **argv, config_t *config)
{
  char *arg_bind = NULL;
  char *arg_directory = NULL;
  // sentinel-terminated array, NULL as end marker is required by argparse_usage() to know when to stop iterating
  static const char *const usages[] = {"uvecho.exe [-h] [-b ADDRESS] [-d DIRECTORY] [port]", NULL};
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
    fprintf(stderr, "error: missing required argument 'port'\n");
    argparse_usage(&argparse);
    return 1;
  }
  else
  {
    config->port = atoi(argv[0]);
    if (config->port <= 0 || config->port > 65535)
    {
      fprintf(stderr, "error: invalid port number\n");
      return 1;
    }
    if (arg_bind)
      config->bind_address = arg_bind;
    else
      config->bind_address = "127.0.0.1";
    if (arg_directory)
      config->directory = arg_directory;
    else
      config->directory = ".";
  }
  return 0;
}

void onWrite(uv_write_t *req, int status)
{
  if (status < 0)
    fprintf(stderr, "error: %s\n", uv_strerror(status));
  DELETE(req);
}

void onAlloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
  handle = handle; // unused
  buf->base = malloc(suggested_size);
  buf->len = suggested_size;
}

void onRead(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
  if (nread < 0)
  {
    if (nread != UV_EOF)
      fprintf(stderr, "error: %s\n", uv_strerror(nread));
    goto cleanup;
  }
  uv_write_t *req = malloc(sizeof(uv_write_t));
  const uv_buf_t bufs[] = {uv_buf_init(buf->base, nread)};
  UV_CHECK(uv_write(req, stream, bufs, 1, onWrite));
cleanup:
  free(buf->base);
  uv_read_stop(stream);
  uv_close((uv_handle_t *)stream, NULL);
  return;
}

void onConnect(uv_stream_t *server, int status)
{
  if (status < 0)
  {
    fprintf(stderr, "error: %s\n", uv_strerror(status));
    return;
  }
  uv_tcp_t *client = malloc(sizeof(uv_tcp_t));
  UV_CHECK(uv_tcp_init(server->loop, client));
  if (uv_accept(server, (uv_stream_t *)client) == 0)
  {
    // start receiving data
    UV_CHECK(uv_read_start((uv_stream_t *)client, onAlloc, onRead));
  }
cleanup:
  DELETE(client);
  return;
}

int main(int argc, const char **argv)
{
  // parse config form command line arguments
  config_t config = {0};
  if (parse_args(argc, argv, &config) != 0)
    return 1;
  struct sockaddr_in addr = {0};
  UV_CHECK(uv_ip4_addr(config.bind_address, config.port, &addr));
  // init uvloop
  uv_loop_t *loop = uv_default_loop();
  if (loop == NULL)
  {
    fprintf(stderr, "error: `uv_default_loop()` failed to allocate memory for the default loop\n");
    return 1;
  }
  // init server
  uv_tcp_t server;
  UV_CHECK(uv_tcp_init(loop, &server));
  UV_CHECK(uv_tcp_bind(&server, (const struct sockaddr *)&addr, 0));
  UV_CHECK(uv_listen((uv_stream_t *)&server, DEFAULT_BACKLOG, onConnect));
  // enter loop
  printf("Listening on %s:%d\n", config.bind_address, config.port);
  return uv_run(loop, UV_RUN_DEFAULT);
cleanup:
  return _ret;
}
