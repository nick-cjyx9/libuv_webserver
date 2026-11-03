#ifndef CONNECTION_H
#define CONNECTION_H
#include <uv.h>

#include "fs.h"

typedef struct conn conn_t;

void connection_on_accept(uv_stream_t *server, int status);

uv_loop_t *conn_loop(conn_t *conn);

/* The directory this connection serves, taken from server->data at accept. */
const char *conn_root(conn_t *conn);

/* A connection is ref'd once for its handles plus once per request that is
 * still in flight, so the http layer can keep it across async fs work. */
void conn_ref(conn_t *conn);
void conn_unref(conn_t *conn);

/* Takes ownership of `head` (which may be NULL), writes it and closes the
 * connection. Called on a connection that is already going away it just frees
 * the buffer. */
void conn_respond(conn_t *conn, char *head, size_t head_len);

/* Takes ownership of `head` and `file`: writes the header block, then streams
 * the file, then closes the connection. The connection keeps itself alive
 * until the last chunk is out or the first error comes back. */
void conn_send_file(conn_t *conn, char *head, size_t head_len, fs_file_t *file);
#endif
