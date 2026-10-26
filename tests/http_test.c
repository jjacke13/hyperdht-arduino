#include "http.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static http_request_t parse(const char *text, http_result_t expected) {
    http_request_t r = {0};
    assert(http_feed(&r, (const uint8_t *)text, strlen(text)) == expected);
    return r;
}

int main(void) {
    const char *request = "GET / HTTP/1.1\r\nHost: localhost:8080\r\n\r\n";
    for (size_t split = 1; split < strlen(request); ++split) {
        http_request_t r = {0};
        assert(http_feed(&r, (const uint8_t *)request, split) == HTTP_MORE);
        assert(http_feed(&r, (const uint8_t *)request + split, strlen(request) - split) == HTTP_READY);
        assert(r.status == 200 && r.route == ROUTE_PAGE && !r.head);
    }
    http_request_t r = parse("HEAD / HTTP/1.1\r\n\r\n", HTTP_READY);
    assert(r.head && r.route == ROUTE_PAGE);
    r = parse("POST /api/pulse HTTP/1.1\r\ncontent-length: 0\r\n\r\n", HTTP_READY);
    assert(r.status == 405 && r.route == ROUTE_EMPTY);
    r = parse("GET /api/status HTTP/1.1\r\n\r\n", HTTP_READY); assert(r.status == 404);
    r = parse("GET /favicon.ico HTTP/1.0\r\n\r\n", HTTP_READY);
    assert(r.status == 204);
    r = parse("GET /missing HTTP/1.1\r\n\r\n", HTTP_READY); assert(r.status == 404);
    r = parse("DELETE / HTTP/1.1\r\n\r\n", HTTP_READY); assert(r.status == 405);
    parse("GET / HTTP/1.1\r\nContent-Length: 4\r\n\r\n", HTTP_BAD);
    parse("GET / HTTP/1.1\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n", HTTP_BAD);
    parse("POST /api/pulse HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", HTTP_BAD);
    parse("GET / HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\n\r\n", HTTP_BAD);
    parse("GET / HTTP/1.1\r\nContent-Length : 0\r\n\r\n", HTTP_BAD);
    parse("GET / HTTP/1.1\r\n Folded: header\r\n\r\n", HTTP_BAD);
    parse("GET / HTTP/1.1\r\nX: bad\nInjected: yes\r\n\r\n", HTTP_BAD);
    const uint8_t nul[] = "GET / HTTP/1.1\r\nX: \0\r\n\r\n";
    r = (http_request_t){0}; assert(http_feed(&r, nul, sizeof(nul)-1) == HTTP_BAD);
    uint8_t long_header[HTTP_HEADER_LIMIT + 1]; memset(long_header, 'x', sizeof(long_header));
    r = (http_request_t){0}; assert(http_feed(&r,long_header,sizeof(long_header)) == HTTP_TOO_LARGE);
    char header[512];
    assert(http_header(header,sizeof(header),200,"text/html",1234)>0);
    assert(strstr(header,"Content-Length: 1234\r\n"));
    assert(strstr(header,"Connection: close\r\n"));
    assert(http_header(header,8,200,"text/html",1234)<0);
    puts("HTTP: every split, routing, HEAD, framing and bounds pass");
}
