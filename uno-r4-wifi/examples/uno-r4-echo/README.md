# Uno R4 WiFi HyperDHT echo

The RA4M1 looks up a peer by public key, authenticates it with Noise IK,
and sends a 1 KiB challenge over UDX and SecretStream. It checks the exact
echo and waits for EOF. The stock ESP32-S3 bridge provides Wi-Fi and UDP.
On the same subnet the connector can choose an authenticated LAN address.
Its other paths include direct UDP and bounded stable-mapping hole punching.

## Build

Use Arduino CLI 1.3.1 with `arduino:renesas_uno` 1.6.0, Arm GCC 7-2017q4,
Python 3, and a static hard-float libsodium 1.0.20 build. From this repo:

```sh
./examples/uno-r4-echo/build.sh /path/to/gcc-arm-none-eabi /path/to/libsodium
```

To prepare libsodium, set `CC` to that toolchain's `arm-none-eabi-gcc`.
Configure with `--host=arm-none-eabi --disable-shared --enable-static
--disable-asm --disable-ssp --without-pthreads` and the following flags:

```sh
export CFLAGS='-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -Os -ffunction-sections -fdata-sections -DRANDOMBYTES_CUSTOM_IMPLEMENTATION -DRANDOMBYTES_DEFAULT_IMPLEMENTATION=NULL'
export LDFLAGS='-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard --specs=nosys.specs --specs=nano.specs'
```

The build writes the ELF, map and binary under `build-r4-sdk/echo.*/build/`.
Arduino's backtrace metadata stamps the build path and date into the binary.
The script applies the example's patches to private copies of the protocol
sources and WiFiS3. It uses the existing 8 KiB SDK stack overlay and
the RA4M1 SCE5 entropy provider.

## Run the echo

On a Node.js host, copy `hyperdht-echo.cjs` into a scratch directory and run
`npm install --no-save hyperdht@6.29.1`, then `node hyperdht-echo.cjs`.
The server prints its public key and uses an OS-assigned UDP port by default.

After uploading the sketch to an Uno R4 WiFi with bridge firmware 0.6.0,
run `python3 connect.py SERIAL_PORT PUBLIC_KEY` from this directory, then
reset the board if it has passed its serial startup prompt. The helper
prompts for Wi-Fi credentials and sends them over USB. On success it reports
`R4_FOUND` and `R4_DONE 0`; `R4_LAN` identifies the LAN route.

## Evidence and scope

The real Uno completed a public-key lookup and encrypted 1 KiB echo plus
EOF with a remote JS peer. That run used 6,996 B of observed stack.
It also completed the same exchange via the LAN shortcut. That run used
7,044 B of observed stack. Both reported 136 B of heap growth and zero
failed allocations. The LAN test image occupied 185,800 B of flash and
reserved 21,864 B of static SRAM, 2,456 B of heap, 8,192 B of stack and
256 B for RAM vectors. Stack watermarks cover the exercised paths.
The example supports one outbound connection and the echo peer's 1 KiB
record profile. Listener and broader NAT compatibility remain open.

The connector patch adapts discovery, LAN selection and hole punching from
hyperdht-cpp at `35514620b5e050817fe15255333f6ab4427f469c` (LGPL-3.0),
principally `src/query.cpp`, `src/connect.cpp`, `src/peer_connect.cpp`
and `src/holepunch.cpp`. The WiFiS3 patch targets the Arduino Renesas
UNO 1.6.0 library and keeps its upstream license notices. It receives
UDP straight into caller-owned storage to avoid a transient SDK allocation.
The UART format comes from the stock bridge 0.6.0 at
`ac27b7b1d9c6c00a341c196ad2185816fe6e589d` (`UNOR4USBBridge/cmds_wifi_udp.h`
and `UNOR4USBBridge/server.cpp`).
