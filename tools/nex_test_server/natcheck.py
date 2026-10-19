"""The NAT check server (nncs1 / nncs2), for the game's peer-to-peer setup (public: kinnay's wiki, "NAT Check Server").

UDP on port 10025 (primary) and 10125 (secondary); big-endian {u32 type, u32 external port, u32 external address,
u32 local address}. The server fills in the port and address it saw and its own address, and replies:
  1, 4, 5, 101, 103: from the port it was asked on;
  3, 102: from another port on the same address;
  2: "from another address and port" - on a single loopback address only another port (ALT_PORT).
"""

from __future__ import annotations

import asyncio
import socket
import struct
from collections.abc import Callable

PORTS = (10025, 10125)
OTHER_PORT = 10026  # (3, 102: same address, another port)
ALT_PORT = 10027    # (2: "another address and port")


class _Proto(asyncio.DatagramProtocol):
    def __init__(self, nat: NatCheck, port: int) -> None:
        self.nat, self.port = nat, port
        self.transport: asyncio.DatagramTransport | None = None

    def connection_made(self, transport) -> None:
        self.transport = transport

    def datagram_received(self, data: bytes, addr) -> None:
        self.nat.received(self.port, data, addr)


def answer(data: bytes, addr: tuple[str, int], server_ip: str) -> bytes | None:
    if len(data) < 16:
        return None
    kind = struct.unpack_from(">I", data)[0]
    return (struct.pack(">IIII", kind, addr[1], int.from_bytes(socket.inet_aton(addr[0]), "big"),
                        int.from_bytes(socket.inet_aton(server_ip), "big")) + data[16:])


class NatCheck:
    def __init__(self, addr: str, log: Callable[[str], None]) -> None:
        self.addr, self.log = addr, log
        self.protos: dict[int, _Proto] = {}

    async def start(self) -> None:
        loop = asyncio.get_running_loop()
        for port in PORTS + (OTHER_PORT, ALT_PORT):
            _, proto = await loop.create_datagram_endpoint(lambda p=port: _Proto(self, p), local_addr=(self.addr, port))
            self.protos[port] = proto
        self.log(f"NAT check server on {self.addr} UDP {PORTS[0]} / {PORTS[1]} (replies also from {OTHER_PORT}, {ALT_PORT})")

    def received(self, port: int, data: bytes, addr) -> None:
        out = answer(data, addr, self.addr)
        if out is None:
            self.log(f"natcheck: {len(data)} bytes from {addr[0]}:{addr[1]} on {port}: too short")
            return
        kind = struct.unpack_from(">I", data)[0]
        via = OTHER_PORT if kind in (3, 102) else ALT_PORT if kind == 2 else port
        self.protos[via].transport.sendto(out, addr)
        self.log(f"natcheck: type {kind} from {addr[0]}:{addr[1]} on {port}, answered from {via}")
