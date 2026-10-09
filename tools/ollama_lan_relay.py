#!/usr/bin/env python3
"""Expose local Ollama to AOS only on the private Ethernet link."""

from __future__ import annotations

import argparse
import ipaddress
import select
import socket
import socketserver
import sys

LINK_NETWORK = ipaddress.ip_network("192.168.77.0/24")


class RelayHandler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        try:
            peer = ipaddress.ip_address(self.client_address[0])
        except ValueError:
            return
        if peer not in LINK_NETWORK:
            return

        try:
            upstream = socket.create_connection(("127.0.0.1", self.server.upstream_port), timeout=5)
        except OSError as exc:
            print(f"Ollama relay: upstream connection failed: {exc}", file=sys.stderr, flush=True)
            return

        with upstream:
            client = self.request
            sockets = [client, upstream]
            while sockets:
                readable, _, _ = select.select(sockets, [], [])
                for source in readable:
                    destination = upstream if source is client else client
                    try:
                        data = source.recv(65536)
                    except OSError:
                        return
                    if not data:
                        sockets.remove(source)
                        try:
                            destination.shutdown(socket.SHUT_WR)
                        except OSError:
                            pass
                        continue
                    try:
                        destination.sendall(data)
                    except OSError:
                        return


class RelayServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address: tuple[str, int], upstream_port: int):
        self.upstream_port = upstream_port
        super().__init__(address, RelayHandler)


def main() -> int:
    parser = argparse.ArgumentParser(description="Forward direct-link Ollama requests to the local Ollama service")
    parser.add_argument("--bind-address", default="192.168.77.1", help="LOQ direct-link address to bind")
    parser.add_argument("--port", type=int, default=11434, help="AOS-facing Ollama port")
    parser.add_argument("--upstream-port", type=int, default=11434, help="localhost Ollama port")
    args = parser.parse_args()

    try:
        bind_address = ipaddress.ip_address(args.bind_address)
    except ValueError:
        parser.error("--bind-address must be a valid IPv4 address")
    if bind_address not in LINK_NETWORK or bind_address == LINK_NETWORK.network_address:
        parser.error("--bind-address must belong to 192.168.77.0/24")

    try:
        with RelayServer((str(bind_address), args.port), args.upstream_port) as server:
            print(f"Ollama relay listening on {bind_address}:{args.port} -> 127.0.0.1:{args.upstream_port}", flush=True)
            server.serve_forever()
    except OSError as exc:
        print(f"Ollama relay failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())