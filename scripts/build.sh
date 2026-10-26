#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
: "${IDF_PATH:?Enter the HyperDHT esp32 Nix shell}"
: "${HYPERDHT_DIR:?Set HYPERDHT_DIR to the private embedded checkout}"
: "${HOLESAIL_DIR:?Set HOLESAIL_DIR to the separate Holesail checkout}"
[[ $# -le 1 ]] || { echo 'Usage: scripts/build.sh [n4r2|n16r8]' >&2; exit 2; }
board=${1:-n4r2}
case "$board" in
    n4r2) build=build; config=sdkconfig ;;
    n16r8) build='build-n16r8'; config=sdkconfig.n16r8 ;;
    *) echo 'Usage: scripts/build.sh [n4r2|n16r8]' >&2; exit 2 ;;
esac
umask 077
for dir in "$HYPERDHT_DIR" "$HOLESAIL_DIR"; do
    [[ "$(git -C "$dir" rev-parse --show-toplevel)" == "$(realpath "$dir")" ]] || exit 1
done
[[ "$(git -C "$HYPERDHT_DIR" rev-parse HEAD)" == bb16c0b0fab5c4dccfde50120955ea0a1730b903 ]] || exit 1
case "$(git -C "$HOLESAIL_DIR" rev-parse HEAD)" in
    8e1efbe4b79dc7d4695eaf2dcfae39bd4cb7769e|f57d423b2d65a660468cbb8029536ec99b9e79c2) ;;
    *) echo 'Use the pinned Holesail codec revision from README.md' >&2; exit 1 ;;
esac
git -C "$HYPERDHT_DIR" apply --reverse --check "$PWD/embedded-tls.patch"
cd examples/PublicWebServer
defaults=sdkconfig.defaults
[[ ! -f sdkconfig.local ]] || defaults+=';sdkconfig.local'
if [[ "$board" == n16r8 ]]; then
    defaults+=';sdkconfig.defaults.n16r8'
    # Copy only Wi-Fi settings on first use; never carry the old memory profile.
    if [[ ! -f "$config" && -f sdkconfig ]]; then
        python - <<'PY'
from pathlib import Path
lines = Path('sdkconfig').read_text().splitlines()
keys = ('CONFIG_P2P_WIFI_SSID=', 'CONFIG_P2P_WIFI_PASSWORD=')
Path('sdkconfig.n16r8').write_text(''.join(line + '\n' for line in lines if line.startswith(keys)))
PY
    fi
fi
idf.py -B "$build" -DIDF_TARGET=esp32s3 -DP2P_BOARD="$board" \
    -DSDKCONFIG="$PWD/$config" -DSDKCONFIG_DEFAULTS="$defaults" reconfigure
chmod 600 "$config"
ninja -C "$build" -j2
python ../../scripts/check-tls.py "$build/arduino-p2p.elf"
symbols=$(xtensa-esp32s3-elf-nm -C "$build/arduino-p2p.elf")
for symbol in 'setup()' 'loopTask' 'HolesailWebServer::begin' hyperdht_server_listen hyperdht_mutable_put 'SecretStreamDuplex::process_incoming_bytes'; do
    [[ "$symbols" == *"$symbol"* ]] || { echo "Missing $symbol; build must retain the complete server" >&2; exit 1; }
done
