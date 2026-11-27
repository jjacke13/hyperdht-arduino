#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""Supply Wi-Fi credentials and the destination public key over USB serial."""
import argparse
import getpass
import os
import re
import select
import sys
import termios
import time
import tty


def credential(value, maximum, empty_ok=False):
    if (not value and not empty_ok) or len(value) > maximum or any(
        not 32 <= ord(char) <= 126 or char == "," for char in value
    ):
        raise ValueError(f"Use at most {maximum} printable ASCII bytes without commas")
    return value.encode("ascii") + b"\n"


def run(port, config, key):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    original = termios.tcgetattr(fd)
    try:
        tty.setraw(fd, termios.TCSANOW)
        attrs = termios.tcgetattr(fd)
        attrs[2] &= ~termios.HUPCL
        attrs[4] = attrs[5] = termios.B115200
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        sent_config = sent_key = found = False
        pending = b""
        deadline = time.monotonic() + 140
        while time.monotonic() < deadline:
            if not select.select([fd], [], [], max(0, deadline - time.monotonic()))[0]:
                break
            data = os.read(fd, 1024)
            if not data:
                raise RuntimeError("USB serial disconnected")
            pending += data
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                line = line.rstrip(b"\r")
                if line == b"R4_READY" and not sent_config:
                    if os.write(fd, config) != len(config):
                        raise RuntimeError("Incomplete Wi-Fi configuration")
                    sent_config = True
                    config = b""
                elif line == b"R4_KEY_READY" and sent_config and not sent_key:
                    if os.write(fd, key) != len(key):
                        raise RuntimeError("Incomplete public key")
                    sent_key = True
                elif line.startswith(b"R4_DONE "):
                    print(line.decode("ascii"), flush=True)
                    match = re.fullmatch(rb"R4_DONE 0 heap_high=(\d+) oom=0 stack_used=(\d+)", line)
                    if not found or not match or not 0 < int(match[2]) < 8192:
                        raise RuntimeError("Board did not complete lookup and authenticated echo")
                    return
                elif line.startswith((b"R4_FAIL ", b"R4_READ_ABORT ")):
                    raise RuntimeError(line.decode("ascii"))
                elif line.startswith((b"R4_STARTUP ", b"R4_WIFI ", b"R4_MODEM ", b"R4_JOIN ",
                                      b"R4_LOOKUP ", b"R4_FOUND ", b"R4_LAN ")):
                    print(line.decode("ascii"), flush=True)
                    if line.startswith(b"R4_FOUND ") and sent_key:
                        found = True
            if len(pending) > 2048:
                raise RuntimeError("Invalid serial output")
        raise TimeoutError("No completed board test within 140 seconds")
    finally:
        termios.tcsetattr(fd, termios.TCSANOW, original)
        os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("public_key", help="JavaScript server's 32-byte public key in hex")
    parser.add_argument("--ssid")
    args = parser.parse_args()
    if not sys.stdin.isatty() or not re.fullmatch(r"[0-9a-fA-F]{64}", args.public_key):
        parser.error("Use a terminal and a 64-character hexadecimal public key")
    config = credential(args.ssid or input("Test Wi-Fi name: "), 32)
    config += credential(getpass.getpass("Test Wi-Fi password (empty for open network): "),
                         63, empty_ok=True)
    run(args.port, config, bytes.fromhex(args.public_key))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, RuntimeError, OSError) as error:
        sys.exit(str(error))
