#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <uv.h>

#include "argparse.h"

#include "connection.h"

#define DEFAULT_BACKLOG 6767

#define UV_CHECK(expr)                                                 \
  do                                                                   \
  {                                                                    \
    int uv_err_ = (expr);                                              \
    if (uv_err_ < 0)                                                   \
    {                                                                  \
      fprintf(stderr, "error: %s: %s\n", #expr, uv_strerror(uv_err_)); \
      _ret = uv_err_;                                                  \
      goto cleanup;                                                    \
    }                                                                  \
  } while (0)

int _ret = 0;

int parse_args(int argc, const char **argv, struct sockaddr_in *addr, const char **directory)
{
  char *arg_bind = NULL;
  char *arg_directory = NULL;
  /* sentinel terminated, argparse_usage() walks it until it hits NULL */
  static const char *const usages[] = {"uvserver.exe [-h] [-b ADDRESS] [-d DIRECTORY] [port]", NULL};
  struct argparse_option options[] = {
      OPT_HELP(),
      OPT_GROUP("options:"),
      OPT_STRING('b', "bind", &arg_bind, "bind to this address (default: 127.0.0.1)", NULL, 0, 0),
      OPT_STRING('d', "directory", &arg_directory, "serve this directory (default: current directory)", NULL, 0, 0),
      OPT_END(),
  };
  struct argparse argparse;
  int port;

  argparse_init(&argparse, options, usages, 0);
  argparse_describe(&argparse, "\nA tiny async static file server built on libuv.", "");
  argc = argparse_parse(&argparse, argc, argv);

  /* argparse_help_cb() exits the process, that one needs no handling */
  if (argc < 1)
  {
    fprintf(stderr, "argument error: missing required argument 'port'\n");
    argparse_usage(&argparse);
    return 1;
  }

  port = atoi(argv[0]);
  if (port <= 0 || port > 65535)
  {
    fprintf(stderr, "argument error: invalid port number\n");
    return 1;
  }

  if (arg_bind != NULL)
  {
    int err = uv_ip4_addr(arg_bind, port, addr);
    if (err < 0)
    {
      fprintf(stderr, "argument error: invalid bind address: %s\n", uv_strerror(err));
      return 1;
    }
  }
  else
    uv_ip4_addr("127.0.0.1", port, addr);

  *directory = arg_directory != NULL ? arg_directory : ".";

  return 0;
}

void check_directory(uv_loop_t *loop, const char *directory)
{
  uv_fs_t req;
  int err = uv_fs_stat(loop, &req, directory, NULL);

  if (err < 0)
    fprintf(stderr, "warning: cannot serve '%s': %s\n", directory, uv_strerror(err));
  else if (!S_ISDIR(req.statbuf.st_mode))
    fprintf(stderr, "warning: '%s' is not a directory\n", directory);

  uv_fs_req_cleanup(&req);
}

int main(int argc, const char **argv)
{
  struct sockaddr_in addr = {0};
  const char *directory = NULL;
  char ip[64];
  uv_loop_t *loop;
  uv_tcp_t server;

  if (parse_args(argc, argv, &addr, &directory) != 0)
    return 1;

  loop = uv_default_loop();
  if (loop == NULL)
  {
    fprintf(stderr, "loop error: uv_default_loop() failed to allocate memory\n");
    return 1;
  }

  check_directory(loop, directory);

  UV_CHECK(uv_tcp_init(loop, &server));
  server.data = (void *)directory;
  UV_CHECK(uv_tcp_bind(&server, (const struct sockaddr *)&addr, 0));
  UV_CHECK(uv_listen((uv_stream_t *)&server, DEFAULT_BACKLOG, connection_on_accept));

  uv_ip4_name(&addr, ip, sizeof(ip));
  printf("serving %s on http://%s:%d\n", directory, ip, ntohs(addr.sin_port));
  return uv_run(loop, UV_RUN_DEFAULT);
cleanup:
  return _ret;
}
