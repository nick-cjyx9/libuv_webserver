#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

#include "connection.h"

void http_handle_request(conn_t *conn, const char *raw, size_t raw_len);

#endif
