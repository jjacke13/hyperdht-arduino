# Architecture and interoperability

Both packages let a sketch listen on a HyperDHT public key or connect to one.
They share wire protocols with the upstream implementation and have
board-specific APIs and resource budgets. [Upstream HyperDHT](https://github.com/holepunchto/hyperdht)

## Connection path

1. The listener announces its public key through the DHT. The client starts
   with that key, looks up rendezvous information, and starts a peer handshake.
2. Noise IK authenticates the peer and establishes session keys. Rendezvous
   nodes can carry handshake messages. On a compatible network, the peers
   establish a direct UDP path by using a reachable address, taking the
   authenticated LAN shortcut where available, or coordinating UDP hole
   punching. A successful lookup alone does not prove UDP connectivity.
3. UDX provides the reliable transport for encrypted SecretStream records.
   The receiving application gets authenticated plaintext. A connection
   encrypted to the right public key can still be public at the application
   level. The example listeners accept anyone with the key and a viable path.

## Board implementations

| | ESP32-S3 | Arduino Uno R4 WiFi |
|---|---|---|
| Protocol code | Native `hyperdht-cpp`, libuv ESP32 shim, libudx and libsodium | Bounded HyperDHT Tiny, UDX and libsodium on the RA4M1 |
| UDP path | ESP32-S3 Wi-Fi and lwIP | Stock ESP32-S3 bridge firmware **0.6.0** provides Wi-Fi and UDP. The RA4M1 holds the secret key and runs Noise, stream encryption and the application. |
| Sketch API | `HyperDHTNode` owns the event loop. Use `nativeHandle()` with the bundled C client/server and stream API. | `HyperDHTClient` and `HyperDHTServer` expose a poll/read/write interface. Select one role per firmware build. |
| Build profile | Arduino-ESP32 **3.3.6**, stock dual-core SDK, 32 KiB loop-task stack in the example sketches | Arduino Renesas UNO **1.6.0**, GCC 7-2017q4 hard-float ABI, 8 KiB main stack overlay and bounded WiFiS3 receive patch |
| Application limits | The echo example caps the total echoed per session at 1 KiB. The separate Holesail example serves one static page. | One active session, up to 1 KiB plaintext **per record**, bounded IPv4 discovery and direct data paths. |

Tiny publishes its own listener to up to three rendezvous nodes and refreshes
its announcements. It does not serve DHT storage or maintain a routing table.
Its transport has a fixed receive pool and one outstanding transmit packet.
It supports eligible LAN paths and a bounded direct punch profile. Data
relay, arbitrary NAT compatibility and combined client/server sizing are
outside this profile. See the [current Uno API](../uno-r4-wifi/ARDUINO-CLI.md),
[transport details](../uno-r4-wifi/docs/TRANSPORT.md) and
[listener wire tests](../uno-r4-wifi/tests/listener/README.md).

On the ESP32-S3, open a server stream inside the connection callback while
its socket reference is valid. The client convenience call
`hyperdht_connect_and_open_stream` does this internally. Drive either
implementation from the sketch's owning task and keep polling.

The Holesail wrapper adds public link publication and a small HTTP handler
on top of the encrypted stream. It answers GET and HEAD at `/` and provides
four application connection slots. A Holesail client exposes a local browser
address. The raw echo sketches use HyperDHT keys and do not speak Holesail
or HTTP. [Holesail wrapper API and limits](../README.md#api-and-limits)

## Verification

| Evidence | Result | Applies to |
|---|---|---|
| ESP32-S3 Arduino CLI | All three sketches compile and link from the installed v0.2.0 ZIP on generic N4R2 and N16R8 profiles. | Package and firmware build. The new raw echo sketches still need a physical run. |
| ESP32-S3 earlier firmware | Arduino IDE upload and working webpage reported by the operator. Matched N16R8 firmware reached `ready=1`. | Earlier Holesail example. See the [hardware record](../BUILD-RESULTS.md#n16r8-hardware-check--2026-10-26). |
| Uno R4 WiFi Arduino CLI | Installed client and server sketches linked at 184,588 and 206,332 bytes of program storage. Static global usage was 22,204 and 21,848 bytes respectively, with an 8,192-byte stack reservation in each ELF. | Packaged sketches. These figures do not measure peak stack or heap. |
| Uno R4 WiFi standalone builds | A stock JavaScript HyperDHT peer exchanged an authenticated 1 KiB echo and EOF over remote and LAN paths. The standalone listener retained its public key through a power cycle. | Earlier standalone [client](../uno-r4-wifi/examples/uno-r4-echo/README.md) and [listener](../uno-r4-wifi/examples/uno-r4-listener/README.md) firmware. |
| Host and Cortex-M4 checks | Tiny has protocol-boundary and upstream interoperability fixtures. A Cortex-M4 guest ran target crypto and exchanged an authenticated echo with a native HyperDHT server over a host bridge. ESP32 wrapper and HTTP host tests passed under ASan/UBSan. | [Tiny host checks](../uno-r4-wifi/README.md#host-checks-with-nix), [emulation](../uno-r4-wifi/docs/EMULATED-M4.md), and [ESP32 host tests](../README.md#checks). |

The desktop examples pin JavaScript HyperDHT **6.29.1**. See the
[quickstart](QUICKSTART.md#connect-from-your-computer) to run the same
1 KiB check. An echo on one network establishes that path's behavior.
It does not establish a NAT compatibility matrix or cross-board testing.

## Rebuild and source pins

- **ESP32-S3:** `hyperdht-cpp` at `bb16c0b` with the repo's embedded TLS
  patch, libudx at `759bf764`, libsodium **1.0.21** and Holesail codecs.
  The build guide lists full revisions and archives the matching sources
  and SDK flag hashes in the ZIP. [ESP32-S3 build guide](../ARDUINO-CLI.md#build-the-package-with-nix)
- **Uno R4 WiFi:** Tiny imported at `fbe4ef1`, plus the Arduino server
  adapter. The package uses libsodium **1.0.20** and an isolated copy of
  the stock SDK linker variant and WiFiS3 library.
  [Uno build guide](../uno-r4-wifi/ARDUINO-CLI.md#build-the-zip-from-source) ·
  [Source provenance](../uno-r4-wifi/PROVENANCE.md)
