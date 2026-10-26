#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum { HTTP_HEADER_LIMIT = 2048 };
typedef enum { HTTP_MORE, HTTP_READY, HTTP_BAD, HTTP_TOO_LARGE } http_result_t;
typedef struct {
    char bytes[HTTP_HEADER_LIMIT + 1];
    size_t used;
    bool complete, head;
    unsigned status;
    enum { ROUTE_PAGE, ROUTE_EMPTY } route;
} http_request_t;
http_result_t http_feed(http_request_t *request, const uint8_t *data, size_t len);
int http_header(char *out, size_t capacity, unsigned status, const char *type, size_t length);
