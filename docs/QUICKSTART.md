# Run your first HyperDHT echo

You need one supported board, a USB data cable, Wi-Fi with internet access,
and a computer with Node.js and npm. The board prints a public key that you
can copy to the computer to connect. It is normal for the first lookup to
take a few seconds while the board announces its key.

Use the installer ZIP for your board or build it using the linked package
guide. The GitHub source archive contains build inputs. An installer ZIP
contains the matching compiled library and example sketches.

## ESP32-S3 with Arduino IDE

1. Open **Tools → Board → Boards Manager** and install **esp32 by Espressif
   Systems**, version **3.3.6**. Install `HyperDHT_ESP32-0.2.0.zip` with
   **Sketch → Include Library → Add .ZIP Library**.
2. Select your board and USB port under **Tools**. For **ESP32S3 Dev
   Module**, use the settings below and choose **Huge APP** as the partition
   scheme. With native USB, enable **USB CDC On Boot** and select
   **Hardware CDC and JTAG** for **USB Mode** where those options are shown.

   | Module | Flash Size | PSRAM |
   |---|---|---|
   | ESP32-S3 N4R2 | 4 MB | QSPI PSRAM |
   | ESP32-S3 N16R8 | 16 MB | OPI PSRAM |

3. Open **File → Examples → HyperDHT ESP32 → HyperDHTEchoServer**. Save a
   local copy. Replace the Wi-Fi placeholders in that copy or use its local
   `wifi_config.h` option. Click **Upload**.
4. Open the serial monitor at **115200**. Press **Reset** if you missed the
   startup message. Copy the 64-character value after `Server public key:`.
   The echo sketch generates a fresh key after each reboot.

On NixOS, launch the IDE with `python3` on its PATH. See the
[ESP32-S3 package guide](../ARDUINO-CLI.md) for the build-from-source and
Arduino CLI instructions.

## Arduino Uno R4 WiFi with Arduino CLI

Use Arduino Renesas UNO **1.6.0**, bridge firmware **0.6.0**, and the compile
helper from `HyperDHT_Tiny_R4-0.1.0.zip`. The helper sets up the required
stack reservation and WiFiS3 patch for each build.

```sh
arduino-cli core update-index
arduino-cli core install arduino:renesas_uno@1.6.0
```

1. [Install the ZIP and prepare the helper](../uno-r4-wifi/ARDUINO-CLI.md#install-and-compile).
   Copy `HyperDHTListenEcho` out of the installed library, edit the Wi-Fi
   placeholders in the copy, then compile that directory with the helper.
2. Find the board port using `arduino-cli board list`. Substitute your port
   and the path printed after `Build:` below. Append `/build` to that path.

   ```sh
   arduino-cli upload /path/to/sketches/HyperDHTListenEcho \
     --fqbn arduino:renesas_uno:unor4wifi \
     --port /dev/ttyACM0 --build-path /path/from/helper/build
   arduino-cli monitor --port /dev/ttyACM0 --config baudrate=115200
   ```

3. Copy the value after `PUBLIC_KEY=`. Send `q` over serial when you want
   the listener to withdraw its announcement and stop. The packaged echo
   sketch generates a fresh key on boot. The
   [standalone listener demo](../uno-r4-wifi/examples/uno-r4-listener/README.md)
   adds persistent Wi-Fi settings and identity.

## Connect from your computer

From the repository root, put the supplied desktop client and its Node.js
dependency in the same scratch directory.

```sh
demo_dir=$(mktemp -d)
cp uno-r4-wifi/examples/uno-r4-listener/hyperdht-client.cjs "$demo_dir/"
cd "$demo_dir"
npm install --no-save hyperdht@6.29.1
node hyperdht-client.cjs PASTE_SERVER_PUBLIC_KEY_HERE
```

Success looks like `Authenticated 1 KiB echo and EOF verified`. If lookup
fails, check that the key is from the current boot and retry after the
listener has had time to announce. If discovery succeeds but the stream
times out, the peers' UDP paths may be blocked by the network or NAT.
The [verification table](ARCHITECTURE.md#verification) shows which
firmware and network paths have been exercised.

## Try the board as a client

- **ESP32-S3:** Use `HyperDHTEchoClient` and an echo server such as a second
  board running `HyperDHTEchoServer`. Paste the server key into the serial
  monitor with newline enabled. Success prints `Authenticated echo verified`.
- **Uno R4 WiFi:** Build and upload `HyperDHTEcho` with the same helper.
  From the repository root, run the supplied desktop server in a new
  scratch directory. Paste its `PUBLIC_KEY=` value into the board's serial
  monitor when prompted. Success prints `Encrypted 1 KiB echo verified`.

  ```sh
  demo_dir=$(mktemp -d)
  cp uno-r4-wifi/examples/uno-r4-echo/hyperdht-echo.cjs "$demo_dir/"
  cd "$demo_dir"
  npm install --no-save hyperdht@6.29.1
  node hyperdht-echo.cjs
  ```

## Open the ESP32-S3 webpage through Holesail

Upload `PublicWebServer` from the ESP32 examples with your Wi-Fi settings.
Wait for `ready=1` in the serial monitor at 115200 and copy its `hs://` link
into Holesail on your phone or computer. Open the local browser address
shown by Holesail. The webpage comes from the ESP32. The link stays the same
across ordinary reboots because the sketch keeps its identity in NVS.
Anyone with this public link can connect.

On a computer with the Holesail CLI, run `holesail 'YOUR_HS_LINK'` and
open the address printed by the client. [Holesail connection guide](https://docs.holesail.io/usage-guide/connect-to-holesail-server)
