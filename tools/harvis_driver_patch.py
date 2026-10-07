#!/usr/bin/env python3
"""Discover supported PCI devices and submit a generated AOS patch."""

from __future__ import annotations

import argparse
import json
import os
import re
import socket
import sys
import termios
import time
from dataclasses import dataclass
from typing import List, Optional, Tuple
from urllib.request import Request, urlopen


WAIT_MARKER = b"SELF-EVOLVE: waiting for driver payload"
PCI_LINE = re.compile(
    rb"PCI: Found (?P<kind>.+?) at bus (?P<bus>\d+) dev (?P<device>\d+) "
    rb"func (?P<function>\d+) \[vid=(?P<vendor>[0-9a-fA-F]{4}) "
    rb"did=(?P<device_id>[0-9a-fA-F]{4})\]"
)
DRIVER_CATALOG = {
    (0x8086, 0x100E): "e1000",
    (0x10EC, 0x8168): "rtl8169",
    (0x10EC, 0x8169): "rtl8169",
}
DEFAULT_AI_ENDPOINT = os.environ.get("HARVIS_AI_ENDPOINT", "http://127.0.0.1:11434/api/generate")
DEFAULT_SETUP_PORT = 9000


@dataclass(frozen=True)
class PciCandidate:
    kind: str
    bus: int
    device: int
    function: int
    vendor_id: int
    device_id: int
    driver: Optional[str]


def discover_devices(boot_log: bytes) -> List[PciCandidate]:
    devices = []
    seen = set()

    for match in PCI_LINE.finditer(boot_log):
        vendor_id = int(match.group("vendor"), 16)
        device_id = int(match.group("device_id"), 16)
        driver = DRIVER_CATALOG.get((vendor_id, device_id))

        key = (vendor_id, device_id, int(match.group("bus")), int(match.group("device")), int(match.group("function")))
        if key in seen:
            continue
        seen.add(key)
        devices.append(
            PciCandidate(
                kind=match.group("kind").decode("ascii", "replace"),
                bus=key[2],
                device=key[3],
                function=key[4],
                vendor_id=vendor_id,
                device_id=device_id,
                driver=driver,
            )
        )

    return devices


def offline_setup_guidance(devices: List[PciCandidate]) -> str:
    supported = [
        f"{device.driver} {device.vendor_id:04X}:{device.device_id:04X}"
        for device in devices
        if device.driver
    ]
    unsupported = [
        f"{device.vendor_id:04X}:{device.device_id:04X}"
        for device in devices
        if not device.driver
    ]

    if supported:
        guidance = "AOS recognized " + ", ".join(supported[:3]) + ". Connect Ethernet and verify link and DHCP."
    else:
        guidance = "AOS found PCI devices but has no matching built-in driver for this hardware."
    if unsupported:
        guidance += " Unmatched PCI IDs: " + ", ".join(unsupported[:4]) + "."
    return guidance


def sanitize_guidance(text: str) -> str:
    flattened = " ".join(str(text).split())
    ascii_text = "".join(character if 32 <= ord(character) < 127 else "?" for character in flattened)
    return ascii_text[:180].strip()


def generate_setup_guidance(devices: List[PciCandidate], endpoint: str = DEFAULT_AI_ENDPOINT) -> Tuple[str, bool]:
    fallback = offline_setup_guidance(devices)
    inventory = "\n".join(
        f"- {device.kind} {device.vendor_id:04X}:{device.device_id:04X} "
        f"at {device.bus:02X}:{device.device:02X}.{device.function:X}; "
        f"built-in driver: {device.driver or 'none'}"
        for device in devices[:16]
    )
    prompt = (
        "Give concise user-facing setup guidance for this AOS hardware inventory. "
        "Do not output code or commands, do not claim unsupported setup succeeded, "
        "distinguish built-in support from unmatched devices, and keep the response "
        "under 180 ASCII characters.\n"
        f"{inventory}"
    )
    request_body = json.dumps({
        "model": "harvis-local",
        "system": "You are the local AOS hardware setup assistant. Use only the supplied inventory.",
        "prompt": prompt,
        "stream": False,
    }).encode("utf-8")
    request = Request(endpoint, data=request_body, headers={"Content-Type": "application/json"})

    try:
        with urlopen(request, timeout=5) as response:
            result = json.loads(response.read().decode("utf-8", "replace"))
        response_text = str(result.get("response", ""))
        if "Offline local-only fallback." in response_text:
            return fallback, False
        guidance = sanitize_guidance(response_text)
        return (guidance, True) if guidance else (fallback, False)
    except Exception:
        return fallback, False


def escape_c_string(text: str) -> str:
    return text.replace("\\", "\\\\").replace('"', '\\"')


def generate_patch(devices: List[PciCandidate], guidance: str) -> bytes:
    candidate = next((device for device in devices if device.driver), None)
    if candidate:
        function_name = "aos_apply_network_setup"
        argument = (
            f"{candidate.bus},{candidate.device},{candidate.function}|"
            f"{sanitize_guidance(guidance)}"
        )
    else:
        function_name = "print_string"
        argument = "AOS SETUP: " + sanitize_guidance(guidance)

    encoded_argument = escape_c_string(argument)
    source = (
        "int driver_init(void) {\n"
        f'    {function_name}("{encoded_argument}");\n'
        "    return 0;\n"
        "}\n"
    )
    return b"---BEGIN_DRIVER---\n" + source.encode("ascii") + b"---END_DRIVER---\n"


class SerialTransport:
    def __init__(self, serial_path: Optional[str] = None, tcp_address: Optional[Tuple[str, int]] = None):
        self.socket = None
        self.fd = None
        if tcp_address:
            deadline = time.monotonic() + 10
            while True:
                try:
                    self.socket = socket.create_connection(tcp_address, timeout=1)
                    break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(0.1)
            self.socket.settimeout(0.1)
        elif serial_path:
            self.fd = os.open(serial_path, os.O_RDWR | os.O_NOCTTY)
            attrs = termios.tcgetattr(self.fd)
            attrs[0] = 0
            attrs[1] = 0
            attrs[2] = termios.CS8 | termios.CLOCAL | termios.CREAD
            attrs[3] = 0
            attrs[4] = termios.B115200
            attrs[5] = termios.B115200
            attrs[6][termios.VMIN] = 0
            attrs[6][termios.VTIME] = 1
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        else:
            raise ValueError("a serial path or TCP address is required")

    def read(self, size: int) -> bytes:
        if self.socket:
            try:
                return self.socket.recv(size)
            except socket.timeout:
                return b""
        return os.read(self.fd, size)

    def write(self, data: bytes) -> None:
        if self.socket:
            self.socket.sendall(data)
            return
        offset = 0
        while offset < len(data):
            offset += os.write(self.fd, data[offset:])

    def close(self) -> None:
        if self.socket:
            self.socket.close()
        if self.fd is not None:
            os.close(self.fd)


def receive_until(transport: SerialTransport, marker: bytes, timeout: float) -> bytes:
    received = bytearray()
    deadline = time.monotonic() + timeout
    while marker not in received and time.monotonic() < deadline:
        chunk = transport.read(1024)
        if chunk:
            received.extend(chunk)
        else:
            time.sleep(0.01)
    if marker not in received:
        raise TimeoutError(f"timed out waiting for {marker.decode('ascii', 'replace')}")
    return bytes(received)


def receive_one_of(transport: SerialTransport, markers: Tuple[bytes, ...], timeout: float) -> bytes:
    received = bytearray()
    deadline = time.monotonic() + timeout
    while not any(marker in received for marker in markers) and time.monotonic() < deadline:
        chunk = transport.read(1024)
        if chunk:
            received.extend(chunk)
        else:
            time.sleep(0.01)
    if not any(marker in received for marker in markers):
        names = " or ".join(marker.decode("ascii", "replace") for marker in markers)
        raise TimeoutError(f"timed out waiting for {names}")
    return bytes(received)


def parse_tcp_endpoint(value: str) -> Tuple[str, int]:
    if not value:
        raise ValueError("TCP endpoint is required")
    if ":" not in value:
        return value, DEFAULT_SETUP_PORT
    host, port_text = value.rsplit(":", 1)
    if not host:
        raise ValueError("TCP target must include a host before the port")
    try:
        port = int(port_text)
    except ValueError as exc:
        raise ValueError(f"invalid TCP port: {port_text}") from exc
    return host, port


def run(serial_path: Optional[str],
    tcp_address: Optional[Tuple[str, int]],
    timeout: float,
    ai_endpoint: str = DEFAULT_AI_ENDPOINT) -> int:
    transport = SerialTransport(serial_path=serial_path, tcp_address=tcp_address)
    try:
        boot_log = receive_until(transport, WAIT_MARKER, timeout)
        devices = discover_devices(boot_log)
        if not devices:
            raise RuntimeError("boot log contains no PCI device inventory")

        supported_drivers = sorted({device.driver for device in devices if device.driver})
        guidance, used_ai = generate_setup_guidance(devices, ai_endpoint)
        print(f"HOST: discovered {len(devices)} PCI device(s)")
        print(f"HOST: known built-in drivers={', '.join(supported_drivers) if supported_drivers else 'none'}")
        print(f"HOST: setup guidance source={'local-ai' if used_ai else 'offline-rule'}")
        transport.write(generate_patch(devices, guidance))
        result = receive_one_of(
            transport,
            (b"---RESULT: SUCCESS---", b"---RESULT: COMPILE_FAILED---"),
            20,
        )
        sys.stdout.write(result.decode("utf-8", "replace"))
        return 0 if b"---RESULT: SUCCESS---" in result else 1
    finally:
        transport.close()


def main() -> int:
    parser = argparse.ArgumentParser(description="Discover PCI devices and submit a generated AOS driver patch")
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--serial", help="host serial device, for example /dev/ttyUSB0")
    target.add_argument("--tcp", help="QEMU serial TCP endpoint as HOST:PORT")
    parser.add_argument("--timeout", type=float, default=90, help="seconds to wait for the AOS self-evolve prompt")
    parser.add_argument("--ai-endpoint", default=DEFAULT_AI_ENDPOINT, help="hosted local-AI generate endpoint")
    args = parser.parse_args()

    tcp_address = None
    if args.tcp:
        try:
            host, port = parse_tcp_endpoint(args.tcp)
            tcp_address = (host, port)
        except ValueError as exc:
            parser.error(f"--tcp {args.tcp}: {exc}")

    try:
        return run(args.serial, tcp_address, args.timeout, args.ai_endpoint)
    except (OSError, RuntimeError, TimeoutError, ValueError) as exc:
        print(f"HOST: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())