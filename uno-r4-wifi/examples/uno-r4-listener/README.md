# Uno R4 WiFi listener test

The sketch starts from the standard HyperDHT bootstrap hosts and publishes
its key to up to three responders. It refreshes those announcements every
30 seconds, serves one active client at a time, and accepts subsequent
clients under the same public key. The RA4M1 runs the listener and SCE5
entropy provider. The ESP32-S3 bridge supplies Wi-Fi and UDP.

The listener binds a local UDP socket on port 49738 and uses it for
publication, authenticated hole punching and the direct encrypted stream.
The JS client uses an OS-assigned port. The supported paths are a reachable
client or stable endpoint mappings for both peers. The tested flow uses
ordinary HyperDHT rendezvous and leaves router and firewall settings alone.
The direct data path requires a NAT/filter combination that admits punches.
If both peers share an external IPv4 address and the client advertises a
private address on the Uno's subnet, the encrypted reply offers the Uno's
LAN address. The stock client can then ping and open UDX over the LAN.

## Build and configure

Use the same Arduino Renesas UNO 1.6.0, Arm GCC 7-2017q4 and hard-float
libsodium build as the [R4 echo bench](../uno-r4-echo/README.md).

```sh
./examples/uno-r4-listener/build.sh /path/to/gcc-arm-none-eabi /path/to/libsodium
```

The build applies the 8 KiB stack overlay and bounded WiFiS3 receive patch
in an isolated build directory. The output is `build/HdtListen.ino.bin`
under the printed directory. The build performs no upload.

Save the Wi-Fi credentials once in a local terminal from the repo root:

```sh
python3 examples/uno-r4-listener/serial-wifi.py save
```

The password prompt hides typing. The helper stores the two values in an
owner-only plaintext file under `$XDG_STATE_HOME/hyperdht-tiny/uno-r4-wifi`
(default `~/.local/state/hyperdht-tiny/uno-r4-wifi`). Run `save` again to
change networks. After uploading, start the helper and press RESET if needed:

```sh
python3 examples/uno-r4-listener/serial-wifi.py send --port /dev/ttyACM0
```

The helper responds to `R4_LISTENER_CONFIG` over USB and displays the public
key after a DHT node acknowledges the announcement. It reports each
completed session and the stack watermark observed since boot. When direct
UDP is established, the LED matrix shows **P2P!** for three seconds on
both LAN and hole-punched connections. The display timer does not pause
the network poll loop.

On the first successful setup, the Uno saves the private identity seed
and Wi-Fi credentials in its RA4M1 data flash. On later boots it rejoins
Wi-Fi and republishes the same key without USB configuration. If the saved
Wi-Fi network cannot be joined, the Uno prompts for replacement credentials
and keeps its identity. The helper can reuse your saved local credentials.
On first setup, if the startup prompt is missed, press RESET and run
`send --after-reset` right away.

This sketch reserves the last two 1 KiB data-flash blocks at EEPROM offsets
`0x1800` and `0x1c00`. It verifies each new record before use and keeps the
previous valid slot during updates. The on-board credentials and seed are
stored in plaintext. A corrupt record with no valid backup stops startup
rather than silently changing identities. Erasing the reserved storage
removes the saved identity.

Closing the serial monitor leaves the listener running. To stop the sketch
and withdraw its key, send `q` over USB serial.

## Remote client

Copy `hyperdht-client.cjs` into a directory on the remote machine. Install
the pinned dependency and run it with the key printed by the board.

```sh
npm install --no-save hyperdht@6.29.1
node hyperdht-client.cjs PUBLIC_KEY
```

The stock JS client discovers the listener from its public key. A passing
run prints `Authenticated 1 KiB echo and EOF verified`. Run it again with
the same key for another session. Other clients can send multiple records
per connection, from 1 to 1,024 bytes of plaintext per record, then end the
stream to receive a clean EOF. An inactive session releases the slot after
60 seconds.

## Verification

The listener core and this JS client passed against an isolated stock JS
DHT. The fixture drops initial direct datagrams until the listener has sent
to that exact peer. The filtered stable-mapping test observed the drop, a
listener punch, multiple exact encrypted echoes, EOF, a second client using
the same public key, and clean withdrawal. The open-client path also passed.
Matrix visibility, publication refresh timing and repeated connections on the
physical board remain hardware checks.
