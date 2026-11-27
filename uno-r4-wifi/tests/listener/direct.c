/* SPDX-License-Identifier: LGPL-3.0-only */
/* Host-only UDP fixture for the same responder used by the R4 sketch. */
#define _POSIX_C_SOURCE 200809L
#include "hdtiny/listener.h"
#include "hdtiny/publish.h"
#include "../../examples/uno-r4-listener/HdtListen/echo_session.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint8_t packet[HDT_RPC_MAX_DATAGRAM];
static hdt_listener listener;
static hdt_addr relays[HDT_PUBLISH_RELAYS];
static size_t relay_count;
static hdt_addr contacted_peer;
static unsigned blocked;
static int lan_test;

static int rendezvous(const hdt_addr *addr) {
    for (size_t i = 0; i < relay_count; ++i)
        if (hdt_addr_equal(addr, &relays[i])) return 1;
    return 0;
}

static int send_packet(int sock, const hdt_addr *to, size_t size) {
    struct sockaddr_in destination = {0};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(to->port);
    memcpy(&destination.sin_addr, to->ip, 4);
    return sendto(sock, packet, size, 0, (struct sockaddr *)&destination,
                  sizeof(destination)) == (ssize_t)size;
}

static int receive(int sock, hdt_addr *peer) {
    struct sockaddr_in sender;
    socklen_t length = sizeof(sender);
    const ssize_t size = recvfrom(sock, packet, sizeof(packet), MSG_DONTWAIT | MSG_TRUNC,
                                  (struct sockaddr *)&sender, &length);
    if (size < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
    if ((size_t)size > sizeof(packet)) return 0;
    memcpy(peer->ip, &sender.sin_addr, 4);
    peer->port = ntohs(sender.sin_port);
    return (int)size;
}

static uint64_t milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static void pause_poll(void) {
    nanosleep(&(const struct timespec){0, 1000000}, NULL);
}

static int test_address(const hdt_addr *addr) {
    return lan_test ? addr->ip[0] == 192 && addr->ip[1] == 168 && addr->ip[2] == 254
                    : addr->ip[0] == 127;
}

static int find_relays(int sock, const uint8_t key[32], const hdt_addr *seed) {
    hdt_publish_walk walk;
    if (!hdt_publish_walk_begin(&walk, key, seed, 1)) return 0;
    const uint64_t start = milliseconds();
    while (!walk.done && milliseconds() - start < 20000) {
        hdt_addr peer, to;
        const int size = receive(sock, &peer);
        if (size < 0) return 0;
        if (size) hdt_publish_walk_receive(&walk, &peer, packet, (size_t)size, test_address);
        const size_t sent = hdt_publish_walk_poll(&walk, milliseconds(), &to, packet, sizeof(packet));
        if (sent && !send_packet(sock, &to, sent)) return 0;
        pause_poll();
    }
    relay_count = walk.best_count;
    for (size_t i = 0; i < relay_count; ++i) relays[i] = walk.best[i].addr;
    return relay_count != 0;
}

static int publish(int sock, const uint8_t key[32], const uint8_t secret[64], hdt_publish_action action) {
    hdt_publish operation;
    for (size_t i = 0; i < relay_count; ++i) {
        if (!hdt_publish_begin(&operation, &relays[i], key, action)) return 0;
        while (operation.status != HDT_PUBLISH_DONE) {
            hdt_addr peer;
            const int size = receive(sock, &peer);
            if (size < 0) return 0;
            if (size) hdt_publish_receive(&operation, &peer, packet, (size_t)size);
            const size_t sent = hdt_publish_poll(&operation, milliseconds(), secret,
                                                 packet, sizeof(packet));
            if (sent && !send_packet(sock, &relays[i], sent)) return 0;
            if (operation.status == HDT_PUBLISH_FAILED) return 0;
            pause_poll();
        }
        if (action == HDT_PUBLISH_ANNOUNCE)
            hdt_listener_observe(&listener, &relays[i], &operation.observed);
    }
    return 1;
}

static int echo(int sock, unsigned sessions) {
    const uint64_t start = milliseconds();
    hdt_connector *stream = &listener.phase.stream;
    unsigned records = 0;
    while (milliseconds() - start < 18000) {
        hdt_addr peer;
        const int size = receive(sock, &peer);
        if (size < 0) return 0;
        // Model endpoint filtering at the board: DHT relays were already
        // contacted. Direct packets pass only after a send to that tuple.
        if (size && relay_count && !lan_test && !rendezvous(&peer) &&
            !hdt_addr_equal(&peer, &contacted_peer)) {
            ++blocked;
            continue;
        }
        if (size && packet[0] == HDT_RPC_REQUEST) {
            const size_t reply = hdt_listener_handshake(&listener, &peer, packet,
                                                        (size_t)size, sizeof(packet));
            if (reply && !send_packet(sock, &peer, reply)) return 0;
        } else if (size) {
            hdt_listener_receive(&listener, &peer, packet, (size_t)size);
        }
        if (listener.reply_size && stream->status == HDT_CONNECT_FAILED) return 0;
        if (echo_record(stream)) ++records;

        hdt_addr to;
        const size_t count = hdt_listener_poll(&listener, milliseconds(), &to, packet, sizeof(packet));
        if (count) {
            if (!send_packet(sock, &to, count)) return 0;
            contacted_peer = to;
        }
        if (echo_finished(stream, count)) {
            if (relay_count) {
                if (!listener.relayed) return 0;
                printf("DIRECT probe=%u open=%u blocked=%u records=%u lan=%u\n",
                       listener.probe_sent, listener.peer_open, blocked, records, listener.lan_ready);
                fflush(stdout);
            }
            if (--sessions == 0) return records != 0;
            hdt_listener_reset(&listener);
            contacted_peer = (hdt_addr){{0}, 0};
            blocked = records = 0;
        }
        pause_poll();
    }
    return 0;
}

int main(int argc, char **argv) {
    hdt_addr bootstrap = {{127, 0, 0, 1}, 0};
    if (argc > 3) return 1;
    if (argc == 3) {
        if (strcmp(argv[2], "lan")) return 1;
        lan_test = 1;
        bootstrap = (hdt_addr){{192, 168, 254, 1}, 0};
    }
    if (argc >= 2) {
        char *end;
        const long port = strtol(argv[1], &end, 10);
        if (*end || port <= 0 || port > 65535) return 1;
        bootstrap.port = (uint16_t)port;
    }
    if (sodium_init() < 0) return 1;
    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return 1;
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    memcpy(&local.sin_addr, bootstrap.ip, sizeof(bootstrap.ip));
    socklen_t size = sizeof(local);
    if (bind(sock, (struct sockaddr *)&local, size) ||
        getsockname(sock, (struct sockaddr *)&local, &size)) {
        close(sock);
        return 1;
    }

    uint8_t key[32], secret[64], seed[32];
    char hex[65];
    randombytes_buf(seed, sizeof(seed));
    crypto_sign_seed_keypair(key, secret, seed);
    const int located = !bootstrap.port || find_relays(sock, key, &bootstrap);
    hdt_listener_init(&listener, seed, key, relays, relay_count);
    if (lan_test) {
        const hdt_addr local_addr = {{192, 168, 254, 1}, ntohs(local.sin_port)};
        const uint8_t mask[4] = {255, 255, 255, 0};
        hdt_listener_set_lan(&listener, &local_addr, mask);
    }
    sodium_memzero(seed, sizeof(seed));

    int ok = located;
    if (ok) {
        sodium_bin2hex(hex, sizeof(hex), key, sizeof(key));
        printf("%u %s\n", (unsigned)ntohs(local.sin_port), hex);
        fflush(stdout);
    }
    if (ok && relay_count) {
        ok = publish(sock, key, secret, HDT_PUBLISH_ANNOUNCE);
        if (ok) {
            puts("ANNOUNCED");
            fflush(stdout);
            ok = publish(sock, key, secret, HDT_PUBLISH_ANNOUNCE);
        }
        if (ok) {
            puts("REFRESHED");
            fflush(stdout);
        }
    }
    if (ok) ok = echo(sock, relay_count ? 2 : 1);
    if (relay_count) {
        const int withdrawn = publish(sock, key, secret, HDT_PUBLISH_WITHDRAW);
        if (withdrawn) puts("WITHDRAWN");
        ok = ok && withdrawn;
    }
    sodium_memzero(secret, sizeof(secret));
    hdt_listener_clear(&listener);
    close(sock);
    return !ok;
}
