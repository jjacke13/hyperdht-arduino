#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""Test-only framed UART <-> isolated loopback UDP bridge; no target NIC."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import select
import signal
import socket
import struct
import subprocess
import tempfile
import time
import unittest
from unittest.mock import patch


MAX_BODY = 2063


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def exact(stream, size, deadline):
    data = bytearray()
    while len(data) < size:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("UART frame deadline")
        stream.settimeout(remaining)
        chunk = stream.recv(size - len(data))
        if not chunk:
            raise EOFError("truncated UART frame")
        data.extend(chunk)
    return bytes(data)


def receive(stream):
    deadline = time.monotonic() + 5
    size, = struct.unpack("<H", exact(stream, 2, deadline))
    require(1 <= size <= MAX_BODY, "invalid UART body length")
    return exact(stream, size, deadline)


def send(stream, body):
    require(1 <= len(body) <= MAX_BODY, "invalid outbound UART length")
    stream.settimeout(5)
    stream.sendall(struct.pack("<H", len(body)) + body)


def ready(line):
    match = re.fullmatch(rb"READY ([0-9]{1,5}) ([0-9a-f]{64})\n", line)
    require(match is not None, "invalid native peer READY line")
    port = int(match[1])
    require(0 < port <= 65535, "invalid native peer port")
    return port, bytes.fromhex(match[2].decode("ascii"))


def output(body, port):
    require(7 <= len(body) <= 2055 and body[0] == 3, "invalid OUTPUT")
    address, target_port = body[1:5], struct.unpack_from("<H", body, 5)[0]
    if len(body) == 7:
        require(address == bytes(4) and target_port == 0, "nonzero empty OUTPUT")
    else:
        require(address == socket.inet_aton("127.0.0.1") and target_port == port,
                "guest requested destination outside selected loopback peer")
    return body[7:]


def result(body):
    require(len(body) == 13 and body[0] == 4, "invalid RESULT")
    status, observed, minimum = struct.unpack_from("<III", body, 1)
    require(status == 0, f"guest failure status {status}")
    require(0 < observed <= 8192 and 0x20006000 <= minimum < 0x20008000,
            "invalid guest stack metrics")
    require(observed >= 0x20008000 - minimum, "stack watermark understates sampled MSP")
    return {"observed_stack_bytes": observed, "minimum_msp": hex(minimum)}


def ready_line(process, deadline):
    line = bytearray()
    while time.monotonic() < deadline:
        if select.select([process.stdout], [], [], 0.1)[0]:
            # One byte avoids swallowing any following native-peer output.
            byte = os.read(process.stdout.fileno(), 1)
            require(bool(byte), "native peer exited before READY")
            line.extend(byte)
            require(len(line) <= 128, "oversized READY line")
            if byte == b"\n":
                return ready(bytes(line))
    raise TimeoutError("native peer READY deadline")


def run(args):
    require({name for _, name in socket.if_nameindex()} == {"lo"},
            "run through tests/run-loopback-only.sh; namespace must contain only lo")
    elf = Path(args.elf).resolve(strict=True)
    parent = Path(args.output).resolve()
    parent.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix="run-", dir=parent))
    report = {"status": "INCOMPLETE", "elf_sha256": hashlib.sha256(elf.read_bytes()).hexdigest(),
              "native_peer_sha256": hashlib.sha256(Path(args.peer).read_bytes()).hexdigest()}
    (directory / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    processes = []
    started = time.monotonic()
    deadline = started + 40
    try:
        with tempfile.TemporaryDirectory(prefix="hdt-m4-", dir="/tmp") as temporary, \
                open(directory / "peer.stderr", "wb") as peer_errors, \
                open(directory / "qemu.stderr", "wb") as qemu_errors, \
                open(directory / "qemu.stdout", "wb") as qemu_output, \
                socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp, \
                socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as uart:
            peer = subprocess.Popen([args.peer, "--serve"], stdout=subprocess.PIPE, stderr=peer_errors)
            processes.append(peer)
            port, key = ready_line(peer, min(deadline, time.monotonic() + 10))
            udp.bind(("127.0.0.1", 0))
            path = str(Path(temporary) / "uart")
            command = [args.qemu, "-M", "mps2-an386", "-nographic", "-monitor", "none",
                       "-serial", f"unix:{path},server=on,wait=on", "-nic", "none",
                       "-kernel", str(elf)]
            report["qemu_command"] = command
            report["qemu_version"] = subprocess.check_output([args.qemu, "--version"], text=True, timeout=5).splitlines()[0]
            guest = subprocess.Popen(command, stdout=qemu_output, stderr=qemu_errors)
            processes.append(guest)
            while True:
                require(guest.poll() is None, "QEMU exited before UART connection")
                try:
                    uart.connect(path)
                    break
                except (FileNotFoundError, ConnectionRefusedError):
                    if time.monotonic() >= deadline:
                        raise TimeoutError("UART startup deadline")
                    time.sleep(0.01)
            uart.settimeout(5)
            send(uart, b"\x01" + socket.inet_aton("127.0.0.1") + struct.pack("<H", port) + key)
            require(output(receive(uart), port) == b"", "CONFIG produced UDP data")
            tx_count = rx_count = steps = 0
            while time.monotonic() < deadline:
                packet, source = b"", ("0.0.0.0", 0)
                if select.select([udp], [], [], 0.002)[0]:
                    packet, source = udp.recvfrom(2049)
                    require(len(packet) <= 2048 and source == ("127.0.0.1", port),
                            "unexpected UDP source or oversized packet")
                    rx_count += 1
                now = int((time.monotonic() - started) * 1000)
                send(uart, b"\x02" + struct.pack("<Q", now) + socket.inet_aton(source[0])
                     + struct.pack("<H", source[1]) + packet)
                body = receive(uart)
                steps += 1
                if body[0] == 4:
                    report.update(result(body))
                    break
                packet = output(body, port)
                if packet:
                    require(udp.sendto(packet, ("127.0.0.1", port)) == len(packet), "short UDP send")
                    tx_count += 1
            else:
                raise TimeoutError("emulated session deadline")
            require(peer.wait(timeout=5) == 0, "native peer did not close cleanly")
            # Embedded guest deliberately halts after RESULT; shutdown is host-owned.
            require(guest.poll() is None, "QEMU exited unexpectedly")
            guest.terminate()
            require(guest.wait(timeout=5) in (0, -signal.SIGTERM), "QEMU shutdown failed")
            report["guest_shutdown"] = "host SIGTERM after valid RESULT and clean peer exit"
            require(hashlib.sha256(elf.read_bytes()).hexdigest() == report["elf_sha256"],
                    "ELF changed during execution")
            report.update(status="PASS", steps=steps, guest_udp_packets=tx_count, peer_udp_packets=rx_count)
    except BaseException as error:
        report.update(status="FAIL", error=f"{type(error).__name__}: {error}")
        raise
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            if process.stdout:
                process.stdout.close()
        report["child_exit_codes"] = [process.returncode for process in processes]
        report["elapsed_seconds"] = round(time.monotonic() - started, 3)
        (directory / "result.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps({"artifacts": str(directory), **report}, indent=2), flush=True)


class BridgeTests(unittest.TestCase):
    def test_frames(self):
        left, right = socket.socketpair()
        with left, right:
            send(left, b"\x03" + bytes(6))
            self.assertEqual(receive(right), b"\x03" + bytes(6))
            left.sendall(b"\x03\x00x")
            left.shutdown(socket.SHUT_WR)
            with self.assertRaises(EOFError):
                receive(right)

    def test_reject_length(self):
        for size in (0, MAX_BODY + 1):
            left, right = socket.socketpair()
            with left, right:
                left.sendall(struct.pack("<H", size))
                with self.assertRaises(ValueError):
                    receive(right)

    def test_frame_deadline_includes_header_and_body(self):
        left, right = socket.socketpair()
        with left, right:
            left.sendall(b"\x02\x00x")
            with patch.object(time, "monotonic", side_effect=[0, 0, 6]):
                with self.assertRaises(TimeoutError):
                    receive(right)

    def test_boundaries(self):
        self.assertEqual(ready(b"READY 1234 " + b"ab" * 32 + b"\n"), (1234, bytes.fromhex("ab" * 32)))
        self.assertEqual(output(b"\x03" + bytes(6), 1234), b"")
        self.assertEqual(output(b"\x03\x7f\x00\x00\x01" + struct.pack("<H", 1234) + b"x", 1234), b"x")
        for bad in (b"READY 0 " + b"ab" * 32 + b"\n", b"READY 1234 secret\n"):
            with self.assertRaises(ValueError):
                ready(bad)
        with self.assertRaises(ValueError):
            output(b"\x03\x08\x08\x08\x08" + struct.pack("<H", 1234) + b"x", 1234)
        self.assertEqual(result(b"\x04" + struct.pack("<III", 0, 4096, 0x20007000))["observed_stack_bytes"], 4096)
        for fields in ((1, 4096, 0x20007000), (0, 1024, 0x20007000), (0, 8193, 0x20006000)):
            with self.assertRaises(ValueError):
                result(b"\x04" + struct.pack("<III", *fields))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    for option in ("elf", "peer", "qemu", "output"):
        parser.add_argument("--" + option)
    arguments = parser.parse_args()
    if arguments.self_test:
        unittest.main(argv=[__file__])
    else:
        require(all((arguments.elf, arguments.peer, arguments.qemu, arguments.output)), "missing bridge arguments")
        run(arguments)
