# Axiometa integration assessment — 2026-10-26

The sketch API fits Axiometa's Arduino workflow. Distribution is not ready:
the current native stack builds as ESP-IDF components, while Axiometa's
published CLI builds sketches with Arduino CLI. This assessment changes no
firmware and does not claim a Studio build has passed.

Update after implementation on 2026-10-26: the native distribution gap below
now has an evaluation implementation. [ARDUINO-CLI.md](ARDUINO-CLI.md) documents
a ZIP installed and compiled with stock Arduino-ESP32 3.3.6 for generic N4R2
and N16R8 profiles, including full server linkage and zero GCC TLS. The original
assessment below records the plan and source research. Exact Axiometa
FQBN/CLI 1.5.1, Studio acceptance, hardware checks and catalog publication are
still outstanding; this is not a Studio-ready claim.

## Their organization

| Layer | Observed organization | Implication for this library |
|---|---|---|
| Boards | Custom Arduino board definitions and variants, named AX22 port pins, FQBN and build-option profiles | Validate against their Genesis Mini target and memory/USB options |
| Modules | Board knowledge contains module `metadata.json` with `libraries`, pins, initialization and coding rules | Supply library name/version plus a short usage guide; Axiometa must decide how a software-only capability enters its catalog |
| Libraries | CLI installs declared names using `arduino-cli lib install`, into its isolated sketchbook | A normal Arduino library is the natural integration point; their published installer does not install our ESP-IDF component |
| Examples | Product-page `.ino` snippets and forkable Studio community projects | Offer a minimal sketch first, with a display example as a separate follow-up |

The PyPI release currently served is `axiometa-cli` 2.0.2. Its published source
pins Arduino CLI 1.5.1 and Arduino-ESP32 3.3.6. The latter matches this
prototype's Arduino core version. Its comments describe parity with Studio's
backend installer; that backend itself was not inspected.

Published source paths inspected within that release:

```text
axiometa_cli/core.py       libraries_for(), fqbn_with_options(), stage(), arduino_deploy()
axiometa_cli/toolchain.py  ESP32_CORE_VERSION, REQUIRED_LIBRARIES, install_library()
axiometa_cli/provision.py  _fragments(), _inject_board_packages()
```

The board-knowledge layout inferred directly from those readers is:

```text
knowledge/boards/<board>/board.json
knowledge/boards/<board>/arduino/firmware/board-package/axiometa/boards.append.txt
knowledge/boards/<board>/arduino/firmware/board-package/axiometa/variants/<variant>/...
```

Module metadata carries a `libraries` list; its exact current Genesis Mini
catalog was not available. Library installation is driven by declared parts,
not inferred from `#include` statements. A software-only networking library
therefore needs explicit catalog/backend integration or another install route.
Matching the Arduino core version alone does not prove SDK/config/ABI parity.

The public GitHub organization currently lists four repositories: three
BrødBoost hardware projects and Binary-Clock. No central public Genesis library
repository was found in that bounded search. The CLI package metadata says its
source repository is private, but the source distribution is public on PyPI.

## Smallest proposed delivery

Keep one library package exposing `HyperDHTNode` and `HolesailWebServer` so
applications can choose their level of abstraction. First target ESP32-S3 only.
Keep the display independent of the networking library.

Use the standard Arduino layout: root `library.properties`, public headers and
implementation under `src/`, and
`examples/PublicWebServer/PublicWebServer.ino`. Preserve the existing ESP-IDF
component build as a supported path. No separate library per AX22 module is
needed for networking.

Before distributing that package:

1. Make HyperDHT, libuv-esp32, libudx, libsodium and the Holesail codecs resolve
   through the Arduino build. Our CMake component graph and external checkout
   paths are not processed by Arduino CLI. Include the upstream TLS correction
   and preserve licenses and reproducible source revisions.
2. Make the sketch configure Wi-Fi without our ESP-IDF-only
   `CONFIG_P2P_WIFI_*` symbols. Use explicit placeholders; never distribute a
   locally configured image or an existing device identity.
3. Preserve the validated 32 KiB loop stack. Core 3.3.6 provides
   `SET_LOOP_TASK_STACK_SIZE(...)`; the ordinary core fallback is 8 KiB. Verify
   PSRAM allocation, USB console, partition size and link flags in the target
   core rather than copying sdkconfig assumptions.
4. Compile with the exact Axiometa FQBN/profile and test the Genesis Mini N4R2:
   startup, zero GCC TLS, image fit, persistent identity and an official
   Holesail client loading the page. Existing N16R8 component-build success
   does not prove this Arduino-CLI package works.

Arduino supports both source libraries and mixed source/precompiled archive
libraries. First check whether the native dependencies can be compiled as
ordinary Arduino library sources. If build-specific flags/configuration make
that unsuitable, evaluate a Nix-produced ESP32-S3 archive plus the small
Arduino wrapper. Such an archive must match the destination core/SDK ABI;
do not reuse the current ESP-IDF archives without that check. A binary package
does not inherently reduce linked firmware size. Neither route was built here.

For initial evaluation, Axiometa could install a library ZIP directly into its
build environment. Its published automatic installer currently expects Library
Manager names; a private ZIP is not automatically available through that path.
Library Manager registration would be a later publishing step, requiring an
accessible repository and release. No publication is authorized by this review.

## Sources and limits

- [Official module sketch: AX22-0034](https://www.axiometa.io/products/ips-lcd-0-96)
- [Official Arduino setup and named port pins](https://www.axiometa.io/pages/how-to-use-axiometa-genesis-with-arduino)
- [Official Studio community examples](https://www.axiometa.io/blogs/news-latest-releases/build-ideas-rotary-encoder)
- [Official CLI documentation and source download](https://pypi.org/project/axiometa-cli/2.0.2/)
- [Official GitHub organization](https://github.com/axiometa)
- [Arduino library specification](https://docs.arduino.cc/arduino-cli/library-specification/)
- [Arduino Library Manager requirements](https://github.com/arduino/library-registry/blob/main/FAQ.md)
- [Arduino 3.3.6 build recipes](https://github.com/espressif/arduino-esp32/blob/3.3.6/platform.txt)

The PyPI source archive SHA-256 was checked against current PyPI metadata:
`b724cf8636ec853c86ba95554f3cb539e9a2fa9ca5408919b568af844679dd1b`.

No browser surface was available. The knowledge service returned HTTP 401 to
an unauthenticated read; no account/key was used. The live module catalog,
signed-in Studio export/import and custom-library acceptance remain unverified.
No toolchain provision, code execution from Axiometa, build, flash or outreach
was performed during this source review.
