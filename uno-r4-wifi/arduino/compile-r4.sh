#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-only
set -euo pipefail
extras=$(cd "$(dirname "$0")" && pwd)
package=$(dirname "$extras")
test -f "$package/library.properties"
sketch=$(realpath "${1:?Sketch directory required}")
cli=${ARDUINO_CLI:-arduino-cli}
override=()
if [[ -n ${HDT_R4_TOOLCHAIN:-} ]]; then
    override=(--build-property "runtime.tools.arm-none-eabi-gcc-7-2017q4.path=$(realpath "$HDT_R4_TOOLCHAIN")")
fi
out=$(mktemp -d "${TMPDIR:-/tmp}/hdtiny-r4.XXXXXXXX")
echo "Build: $out"
fqbn=arduino:renesas_uno:unor4wifi
"$cli" compile --show-properties=expanded --fqbn "$fqbn" "${override[@]}" "$sketch" \
    > "$out/sdk-properties.txt"
variant=$(sed -n 's/^build.variant.path=//p' "$out/sdk-properties.txt")
"${PYTHON:-python3}" "$extras/r4-stack-overlay.py" "$variant" "$out/UNOWIFIR4"
wifi=$out/WiFiS3
cp -R "$variant/../../libraries/WiFiS3" "$wifi"
chmod -R u+w "$wifi"
patch --batch --forward --fuzz=0 -d "$wifi" -p1 < "$extras/wifis3-1.6.0.patch"
if ! "$cli" compile --fqbn "$fqbn" --verbose "${override[@]}" \
    --library "$package" --library "$wifi" \
    --build-path "$out/build" --build-property "build.variant.path=$out/UNOWIFIR4" \
    "$sketch" > "$out/compile.log" 2>&1; then
    tail -35 "$out/compile.log" >&2
    exit 1
fi
grep -F "Using precompiled library in $package/src/cortex-m4" "$out/compile.log"
grep -F "in folder: $wifi" "$out/compile.log"
grep -F 'Sketch uses ' "$out/compile.log"
