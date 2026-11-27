#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""Save local Wi-Fi credentials once and configure the listener over USB serial."""
import argparse
import getpass
import os
from pathlib import Path
import re
import select
import stat
import sys
import tempfile
import termios
import time
import tty

STATE = Path(os.environ.get("XDG_STATE_HOME") or Path.home() / ".local/state")
CREDENTIALS = STATE / "hyperdht-tiny" / "uno-r4-wifi"


def encode_wifi(ssid, password):
    for field, value, limit in (("SSID", ssid, 32), ("password", password, 63)):
        if (field == "SSID" and not value) or len(value) > limit or any(
            ch < " " or ch > "~" or ch == "," for ch in value
        ):
            raise ValueError(f"Use at most {limit} printable ASCII characters without commas for {field}")
    return f"{ssid}\n{password}\n".encode("ascii")


def save_wifi(config):
    CREDENTIALS.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    # Replace atomically so a write failure preserves the previous credentials.
    with tempfile.NamedTemporaryFile(dir=CREDENTIALS.parent, delete=False) as output:
        temporary = Path(output.name)
        try:
            output.write(config)
            output.close()
            os.replace(temporary, CREDENTIALS)
        finally:
            temporary.unlink(missing_ok=True)


def load_wifi():
    fd = os.open(CREDENTIALS, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as saved:
        info = os.fstat(saved.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
            raise ValueError("The Wi-Fi file must be a private regular file owned by you (mode 0600)")
        lines = saved.read(98).decode("ascii").split("\n")
    if len(lines) != 3 or lines[2]:
        raise ValueError("Invalid Wi-Fi file. Run save again")
    return encode_wifi(*lines[:2])


def send_wifi(fd, config):
    if os.write(fd, config) != len(config):
        raise RuntimeError("Incomplete USB serial write. Reset the Uno and retry")
    print("Sent saved Wi-Fi credentials over USB.", flush=True)


def configure(port, config, after_reset=False):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    original = None
    try:
        original = termios.tcgetattr(fd)
        tty.setraw(fd)
        attrs = termios.tcgetattr(fd)
        attrs[2] &= ~termios.HUPCL
        attrs[4] = attrs[5] = termios.B115200
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        if after_reset:
            send_wifi(fd, config)
        else:
            print("Listening. Press RESET if the Uno has already finished booting.", flush=True)
        deadline = time.monotonic() + (105 if after_reset else 60)
        sent, published = after_reset, False
        pending = b""
        while True:
            timeout = None if published else max(0, deadline - time.monotonic())
            if not select.select([fd], [], [], timeout)[0]:
                break
            try:
                data = os.read(fd, 1024)
            except BlockingIOError:
                continue
            if not data:
                raise RuntimeError("USB serial disconnected")
            pending += data
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                line = line.rstrip(b"\r")
                if line == b"R4_LISTENER_CONFIG" and not sent:
                    send_wifi(fd, config)
                    sent = True
                    deadline = time.monotonic() + 105
                elif re.fullmatch(rb"PUBLIC_KEY=[0-9a-f]{64}", line):
                    published = True
                    print(line.decode("ascii"), flush=True)
                elif re.fullmatch(rb"R4_LISTENER_FAIL (profile|config|entropy|lookup|announce|storage)", line):
                    raise RuntimeError(line.decode("ascii"))
                elif re.fullmatch(rb"R4_LISTENER_(RELAYED|DIRECT|LAN|PUNCH|UDX|SETTINGS (saved|loaded)|MATRIX failed|REFRESH (ok|failed)|SESSION echo=(ok|failed) records=\d+ stack_used=\d+)", line):
                    print(line.decode("ascii"), flush=True)
                elif re.fullmatch(rb"R4_LISTENER_STOPPED withdraw=(ok|failed)", line):
                    print(line.decode("ascii"), flush=True)
                    return published and line.endswith(b"withdraw=ok")
                elif re.fullmatch(rb"R4_LISTENER_DONE echo=(ok|failed) withdraw=(ok|failed) stack_used=\d+", line):
                    print(line.decode("ascii"), flush=True)
                    return published and line.startswith(b"R4_LISTENER_DONE echo=ok withdraw=ok ")
            if len(pending) > 2048:
                raise RuntimeError("Invalid serial output")
        raise TimeoutError("Timed out waiting for the listener")
    finally:
        try:
            if original is not None:
                termios.tcsetattr(fd, termios.TCSANOW, original)
        finally:
            os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("save", "send"))
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--after-reset", action="store_true",
                        help="send immediately after pressing RESET during first setup if the startup prompt was missed")
    args = parser.parse_args()
    if args.action == "save":
        if args.after_reset:
            parser.error("--after-reset applies to send")
        if not sys.stdin.isatty():
            parser.error("Run save in a local interactive terminal")
        ssid = input("Wi-Fi SSID: ")
        password = getpass.getpass("Wi-Fi password (empty for an open network): ")
        save_wifi(encode_wifi(ssid, password))
        print(f"Saved Wi-Fi details to {CREDENTIALS}")
        return 0
    if not CREDENTIALS.exists():
        parser.error("Run save once before send")
    return 0 if configure(args.port, load_wifi(), args.after_reset) else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
    except (ValueError, RuntimeError, OSError) as error:
        sys.exit(str(error))
