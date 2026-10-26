#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
: "${IDF_PATH:?Enter the HyperDHT esp32 Nix shell}"
: "${HYPERDHT_DIR:?Set HYPERDHT_DIR to the private embedded checkout}"
: "${HOLESAIL_DIR:?Set HOLESAIL_DIR to the separate Holesail checkout}"
for dir in "$HYPERDHT_DIR" "$HOLESAIL_DIR"; do
    [[ "$(git -C "$dir" rev-parse --show-toplevel)" == "$(realpath "$dir")" ]] || exit 1
done
[[ "$(git -C "$HYPERDHT_DIR" rev-parse HEAD)" == bb16c0b0fab5c4dccfde50120955ea0a1730b903 ]] || exit 1
[[ "$(git -C "$HOLESAIL_DIR" rev-parse HEAD)" == f57d423b2d65a660468cbb8029536ec99b9e79c2 ]] || exit 1
git -C "$HYPERDHT_DIR" apply --reverse --check "$PWD/embedded-tls.patch"
cd examples/PublicWebServer
defaults=sdkconfig.defaults
[[ ! -f sdkconfig.local ]] || defaults+=';sdkconfig.local'
idf.py -DIDF_TARGET=esp32s3 -DSDKCONFIG_DEFAULTS="$defaults" reconfigure
ninja -C build -j2
python ../../scripts/check-tls.py build/arduino-p2p.elf
symbols=$(xtensa-esp32s3-elf-nm -C build/arduino-p2p.elf)
for symbol in 'setup()' 'loopTask' 'HolesailWebServer::begin' hyperdht_server_listen hyperdht_mutable_put 'SecretStreamDuplex::process_incoming_bytes'; do
    [[ "$symbols" == *"$symbol"* ]] || { echo "Missing $symbol; build must retain the complete server" >&2; exit 1; }
done
