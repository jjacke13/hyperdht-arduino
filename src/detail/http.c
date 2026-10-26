#include "http.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

http_result_t http_feed(http_request_t *r, const uint8_t *data, size_t len) {
    if (r->complete || !data || !len) return HTTP_BAD;
    if (len > HTTP_HEADER_LIMIT - r->used) return HTTP_TOO_LARGE;
    for (size_t i = 0; i < len; ++i)
        if (!data[i] || (data[i] < 32 && data[i] != '\r' && data[i] != '\n' && data[i] != '\t'))
            return HTTP_BAD;
    memcpy(r->bytes + r->used, data, len);
    r->used += len;
    r->bytes[r->used] = 0;
    char *end = strstr(r->bytes, "\r\n\r\n");
    if (!end) return HTTP_MORE;
    /* This service accepts bodyless requests only; reject pipelining and body smuggling. */
    if (end + 4 != r->bytes + r->used) return HTTP_BAD;
    char *line = strstr(r->bytes, "\r\n");
    if (!line) return HTTP_BAD;
    *line = 0;
    char method[8], target[96], version[16], extra;
    if (sscanf(r->bytes, "%7s %95s %15s %c", method, target, version, &extra) != 3 ||
        (strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0"))) return HTTP_BAD;
    bool content_length = false;
    for (char *p = line + 2; p < end; ) {
        char *next = strstr(p, "\r\n");
        if (!next || p == next || *p == ' ' || *p == '\t') return HTTP_BAD;
        for (char *k = p; k < next; ++k) if (*k == '\r' || *k == '\n') return HTTP_BAD;
        *next = 0;
        char *colon = strchr(p, ':');
        if (!colon || colon == p) return HTTP_BAD;
        for (char *k = p; k < colon; ++k)
            if (!((*k >= 'a' && *k <= 'z') || (*k >= 'A' && *k <= 'Z') ||
                  (*k >= '0' && *k <= '9') || *k == '-')) return HTTP_BAD;
        *colon = 0;
        char *value = colon + 1;
        while (*value == ' ' || *value == '\t') ++value;
        char *tail = value + strlen(value);
        while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t')) *--tail = 0;
        if (!strcasecmp(p, "Transfer-Encoding") || !strcasecmp(p, "Upgrade")) return HTTP_BAD;
        if (!strcasecmp(p, "Content-Length")) {
            if (content_length || strcmp(value, "0")) return HTTP_BAD;
            content_length = true;
        }
        p = next + 2;
    }
    r->complete = true;
    r->head = !strcmp(method, "HEAD");
    r->status = 200;
    r->route = ROUTE_EMPTY;
    if (strcmp(method, "GET") && !r->head) r->status = 405;
    else if (!strcmp(target, "/")) r->route = ROUTE_PAGE;
    else if (!strcmp(target, "/favicon.ico")) r->status = 204;
    else r->status = 404;
    return HTTP_READY;
}

int http_header(char *out, size_t capacity, unsigned status, const char *type, size_t length) {
    const char *reason = status == 200 ? "OK" : status == 204 ? "No Content" :
                         status == 400 ? "Bad Request" : status == 404 ? "Not Found" :
                         status == 405 ? "Method Not Allowed" : "Request Header Fields Too Large";
    const int n = snprintf(out, capacity,
        "HTTP/1.1 %u %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Connection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "Content-Security-Policy: default-src 'self'; script-src 'unsafe-inline'; "
        "style-src 'unsafe-inline'; img-src 'self' data:; frame-ancestors 'none'\r\n"
        "Allow: GET, HEAD\r\n\r\n", status, reason, type, length);
    return n >= 0 && (size_t)n < capacity ? n : -1;
}
