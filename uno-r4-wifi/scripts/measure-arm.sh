#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build-arm
sodium_headers=$(pkg-config --variable=includedir libsodium)
for unit in rpc discovery udx_rx record; do
  arm-none-eabi-gcc -std=c99 -Os -g -mcpu=cortex-m4 -mthumb \
    -Wall -Wextra -Werror -pedantic -ffunction-sections -fdata-sections \
    -fstack-usage -Iinclude -isystem "$sodium_headers" \
    -c "src/$unit.c" -o "build-arm/$unit.o"
done
cat > build-arm/layout.c <<'EOF'
#include "hdtiny/discovery.h"
#include "hdtiny/udx_rx.h"
#include "hdtiny/record.h"
unsigned char hdt_size_discovery[sizeof(hdt_discovery)];
unsigned char hdt_size_udx_rx[sizeof(hdt_udx_rx_t)];
unsigned char hdt_size_record[sizeof(hdt_record)];
EOF
arm-none-eabi-gcc -std=c99 -Os -mcpu=cortex-m4 -mthumb -Iinclude \
  -isystem "$sodium_headers" -c build-arm/layout.c -o build-arm/layout.o
arm-none-eabi-gcc --version | head -n 1
arm-none-eabi-size build-arm/rpc.o build-arm/discovery.o build-arm/udx_rx.o build-arm/record.o
arm-none-eabi-nm --print-size --size-sort --radix=d build-arm/layout.o
printf '%s\n' 'Object-only ARM measurement. Crypto/libc/drivers/startup are NOT linked.'
printf '%s\n' 'Compiler .su files report local frames only, NOT whole call-stack peaks.'
