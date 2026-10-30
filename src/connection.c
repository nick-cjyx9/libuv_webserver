#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "connection.h"
#include "http.h"

#define REQUEST_MAX (8 * 1024)
#define TIMEOUT_MS (67 * 1000)

#define MSG_408 "HTTP/1.1 408 Request Timeout\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
#define MSG_431 "HTTP/1.1 431 Request Header Fields Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"

struct conn
{
  uv_tcp_t tcp;
  uv_timer_t timer;
  uv_write_t write;

  const char *root;

  char request[REQUEST_MAX]; /* filled in place by on_alloc(), no copying */
  size_t request_len;

  char *head;      /* header block, freed once it is on the wire */
  fs_file_t *file; /* body being streamed, NULL for an empty response */

  int refs;    /* 1 for the handles, +1 per request in flight, +1 per stream */
  int closing; /* pending uv_close() callbacks, two of them */
  int responded;
};

void conn_close(conn_t *conn);
void on_file_chunk(const char *data, size_t len, int status, void *userdata);

void conn_ref(conn_t *conn) { conn->refs++; }

void conn_unref(conn_t *conn)
{
  if (--conn->refs == 0)
    free(conn);
}

uv_loop_t *conn_loop(conn_t *conn) { return conn->tcp.loop; }

const char *conn_root(conn_t *conn) { return conn->root; }

/* Small responses (408, 431) are compile time strings, but conn_respond()
 * owns whatever we hand it, so it gets a copy. */
void conn_send_simple(conn_t *conn, const char *response)
{
  size_t len = strlen(response);
  char *head = malloc(len);

  if (head == NULL)
    return conn_respond(conn, NULL, 0);

  memcpy(head, response, len);
  conn_respond(conn, head, len);
}

/* One exit for every response: drop whatever is still owned, let the handles
 * close and give up the streaming ref, if one was taken. */
void conn_finish(conn_t *conn)
{
  fs_file_t *file = conn->file;

  conn->file = NULL;
  free(conn->head);
  conn->head = NULL;

  conn_close(conn);

  if (file != NULL)
  {
    fs_file_close(file);
    conn_unref(conn); /* pairs with conn_send_file() */
  }
}

/* Stops reading and disarms the timer: from here on this connection only
 * writes. Returns 0 while it is still usable. */
int conn_begin(conn_t *conn)
{
  if (uv_is_closing((uv_handle_t *)&conn->tcp))
    return -1;

  conn->responded = 1;
  uv_timer_stop(&conn->timer);
  uv_read_stop((uv_stream_t *)&conn->tcp);

  return 0;
}

/* Shared by the header block and every chunk: whatever was written is done
 * with, either the body starts or the next chunk is pulled. */
void on_written(uv_write_t *req, int status)
{
  conn_t *conn = req->data;

  if (status < 0)
    fprintf(stderr, "write error: %s\n", uv_strerror(status));

  if (status < 0 || uv_is_closing((uv_handle_t *)&conn->tcp))
    return conn_finish(conn);

  free(conn->head);
  conn->head = NULL;

  if (conn->file == NULL)
    return conn_finish(conn); /* the header block was the whole response */

  fs_file_read(conn->file, on_file_chunk, conn);
}

int conn_write(conn_t *conn, const char *data, size_t len)
{
  uv_buf_t buf = uv_buf_init((char *)data, (unsigned int)len);

  conn->write.data = conn;
  return uv_write(&conn->write, (uv_stream_t *)&conn->tcp, &buf, 1, on_written);
}

/* The next chunk, len == 0 at end of file, status < 0 on a read error. The
 * chunk belongs to the file and must not be read again before this write is
 * done, which is exactly the order the two callbacks enforce. */
void on_file_chunk(const char *data, size_t len, int status, void *userdata)
{
  conn_t *conn = userdata;
  int err;

  if (status < 0)
    fprintf(stderr, "read error: %s\n", uv_strerror(status));

  if (status < 0 || len == 0 || uv_is_closing((uv_handle_t *)&conn->tcp))
    return conn_finish(conn);

  err = conn_write(conn, data, len);
  if (err < 0)
  {
    fprintf(stderr, "write error: %s\n", uv_strerror(err));
    conn_finish(conn);
  }
}

void conn_respond(conn_t *conn, char *head, size_t head_len)
{
  int err;

  if (conn_begin(conn) < 0 || head_len == 0)
  {
    free(head);
    return conn_finish(conn);
  }

  conn->head = head;

  err = conn_write(conn, head, head_len);
  if (err < 0)
  {
    fprintf(stderr, "write error: %s\n", uv_strerror(err));
    conn_finish(conn);
  }
}

void conn_send_file(conn_t *conn, char *head, size_t head_len, fs_file_t *file)
{
  int err;

  if (conn_begin(conn) < 0)
  {
    free(head);
    fs_file_close(file);
    return;
  }

  conn->file = file;
  conn_ref(conn);

  if (head_len == 0 || head == NULL)
  {
    free(head);
    return fs_file_read(file, on_file_chunk, conn);
  }

  conn->head = head;

  err = conn_write(conn, head, head_len);
  if (err < 0)
  {
    fprintf(stderr, "write error: %s\n", uv_strerror(err));
    conn_finish(conn);
  }
}

void on_handle_closed(uv_handle_t *handle)
{
  conn_t *conn = handle->data;

  if (--conn->closing == 0)
    conn_unref(conn);
}

void conn_close(conn_t *conn)
{
  if (uv_is_closing((uv_handle_t *)&conn->tcp))
    return;

  uv_read_stop((uv_stream_t *)&conn->tcp);
  uv_timer_stop(&conn->timer);

  conn->closing = 2;
  uv_close((uv_handle_t *)&conn->tcp, on_handle_closed);
  uv_close((uv_handle_t *)&conn->timer, on_handle_closed);
}

void on_timeout(uv_timer_t *timer)
{
  conn_t *conn = timer->data;

  /* the response is already on the wire, cut the connection instead */
  if (conn->responded)
    return conn_close(conn);

  conn_send_simple(conn, MSG_408);
}

void on_alloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
  conn_t *conn = handle->data;
  size_t space = sizeof(conn->request) - conn->request_len;

  (void)suggested_size;

  buf->base = space > 0 ? conn->request + conn->request_len : NULL;
  buf->len = space;
}

/* Offset just past the header block, 0 while the request is still partial. */
size_t header_end(const char *s, size_t n)
{
  size_t i;

  for (i = 0; i + 1 < n; i++)
  {
    if (s[i] == '\n' && s[i + 1] == '\n')
      return i + 2;
    if (i + 3 < n && s[i] == '\r' && s[i + 1] == '\n' && s[i + 2] == '\r' && s[i + 3] == '\n')
      return i + 4;
  }

  return 0;
}

void on_con_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
  conn_t *conn = stream->data;
  size_t end;

  (void)buf; /* the buffer is conn->request, there is nothing to free */

  if (nread < 0)
  {
    if (nread != UV_EOF)
      fprintf(stderr, "read error: %s\n", uv_strerror((int)nread));
    return conn_close(conn);
  }

  if (nread == 0)
    return;

  conn->request_len += (size_t)nread;
  end = header_end(conn->request, conn->request_len);

  if (end == 0)
  {
    if (conn->request_len < sizeof(conn->request))
      return; /* wait for the rest */
    return conn_send_simple(conn, MSG_431);
  }

  uv_timer_stop(&conn->timer);
  uv_read_stop(stream);

  http_handle_request(conn, conn->request, end);
}

conn_t *conn_new(uv_loop_t *loop, const char *root)
{
  conn_t *conn = calloc(1, sizeof(*conn));

  if (conn == NULL)
    return NULL;

  conn->refs = 1;
  conn->root = root;

  if (uv_tcp_init(loop, &conn->tcp) < 0)
  {
    free(conn);
    return NULL;
  }

  if (uv_timer_init(loop, &conn->timer) < 0)
  {
    /* uv_timer_init() does not allocate, this cannot realistically happen */
    free(conn);
    return NULL;
  }

  conn->tcp.data = conn;
  conn->timer.data = conn;

  return conn;
}

void connection_on_accept(uv_stream_t *server, int status)
{
  conn_t *conn;

  if (status < 0)
  {
    fprintf(stderr, "connection error: %s\n", uv_strerror(status));
    return;
  }

  conn = conn_new(server->loop, server->data);
  if (conn == NULL)
  {
    fprintf(stderr, "out of memory\n");
    return;
  }

  if (uv_accept(server, (uv_stream_t *)&conn->tcp) < 0)
    return conn_close(conn);
  if (uv_timer_start(&conn->timer, on_timeout, TIMEOUT_MS, 0) < 0)
    return conn_close(conn);
  if (uv_read_start((uv_stream_t *)&conn->tcp, on_alloc, on_con_read) < 0)
    return conn_close(conn);
}
