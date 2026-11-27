#!/usr/bin/env python3
"""Execute real ARM CONFIG/framing/retry paths without a peer or guest NIC."""
import argparse
import json
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time


def receive(sock, size):
    result = bytearray()
    while len(result) < size:
        part = sock.recv(size - len(result))
        if not part:
            raise RuntimeError("UART ended before a complete frame")
        result.extend(part)
    return bytes(result)


def exchange(sock, body):
    sock.sendall(struct.pack("<H", len(body)) + body)
    size, = struct.unpack("<H", receive(sock, 2))
    assert 1 <= size <= 2055, size
    return receive(sock, size)


def step(now):
    return b"\x02" + struct.pack("<Q", now) + bytes(6)


def run(args, name, operation, expected):
    with tempfile.TemporaryDirectory(prefix="tiny-m4-") as directory:
        path = Path(directory) / "uart"
        with (Path(directory) / "qemu.stderr").open("wb") as error:
            process = subprocess.Popen([
                args.qemu, "-M", "mps2-an386", "-nographic", "-monitor", "none",
                "-serial", f"unix:{path},server=on,wait=on", "-nic", "none",
                "-kernel", args.elf,
            ], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=error)
            try:
                with socket.socket(socket.AF_UNIX) as sock:
                    deadline = time.monotonic() + 5
                    while True:
                        try:
                            sock.connect(str(path))
                            break
                        except (FileNotFoundError, ConnectionRefusedError):
                            assert process.poll() is None and time.monotonic() < deadline
                            time.sleep(.01)
                    sock.settimeout(10)
                    response = operation(sock)
                    assert len(response) == 13 and response[0] == 4, response.hex()
                    status, stack, minimum = struct.unpack("<III", response[1:])
                    assert status == expected, (name, status, expected)
                    assert 0 < stack <= 8192 and 0x20006000 <= minimum <= 0x20008000
                    print(json.dumps(dict(case=name, status=status, observed_stack=stack,
                                          min_msp=hex(minimum))))
            finally:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


def configure(sock):
    # RFC 8032 public test vector, never a production identity.
    key = bytes.fromhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")
    config = b"\x01\x7f\0\0\x01" + struct.pack("<H", 12345) + key
    assert exchange(sock, config) == b"\x03" + bytes(6)


def timeout_session(sock):
    configure(sock)
    for now in (0, 1000, 2000):
        out = exchange(sock, step(now))
        assert len(out) > 7 and out[:7] == b"\x03\x7f\0\0\x01" + struct.pack("<H",12345)
    return exchange(sock, step(3000))


def backward_clock(sock):
    configure(sock)
    assert exchange(sock, step(1))[0] == 3
    return exchange(sock, step(0))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", required=True)
    parser.add_argument("--qemu", default="qemu-system-arm")
    args = parser.parse_args()
    run(args, "unknown-type", lambda s: exchange(s, b"\x09"), 1)
    run(args, "empty-frame", lambda s: exchange(s, b""), 1)
    run(args, "oversized-frame", lambda s: exchange(s, bytes(2064)), 1)
    run(args, "step-before-config", lambda s: exchange(s, step(0)), 2)
    run(args, "short-config", lambda s: exchange(s, b"\x01"), 2)
    run(args, "backward-clock", backward_clock, 6)
    run(args, "noise-request-retry-timeout", timeout_session, 4)
