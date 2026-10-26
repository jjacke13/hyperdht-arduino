#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
: "${HYPERDHT_DIR:?}" "${HOLESAIL_DIR:?}"
mkdir -p build-tests
cc -std=c11 -g -fsanitize=address,undefined -I src/detail tests/http_test.c src/detail/http.c -o build-tests/http
build-tests/http
cc -std=c11 -g -fsanitize=address,undefined -I src/detail -c src/detail/http.c -o build-tests/http.o
c++ -std=c++20 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I tests/stubs -I src -I "$HYPERDHT_DIR/include" -I "$HYPERDHT_DIR/components/libuv-esp32/include" \
    -I "$HOLESAIL_DIR/include" $(pkg-config --cflags libsodium) \
    tests/wrapper_test.cpp src/HyperDHTNode.cpp src/HolesailWebServer.cpp \
    "$HOLESAIL_DIR/src/keys.cpp" "$HOLESAIL_DIR/src/record.cpp" build-tests/http.o \
    $(pkg-config --libs libsodium) -o build-tests/wrapper
build-tests/wrapper
