"""PRUDP "Lite": the packet format NEX uses over WebSocket on the Switch (public: kinnay's NintendoClients wiki,
"PRUDP Protocol", lite mode).

Header, little-endian:
    u8  magic 0x80
    u8  option data length
    u16 payload length
    u8  stream types: source << 4 | destination
    u8  source port
    u8  destination port
    u8  fragment id
    u16 packet type | flags << 4
    u16 sequence id
then the options (id u8, length u8, value) and the payload.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

MAGIC = 0x80

TYPE_SYN, TYPE_CONNECT, TYPE_DATA, TYPE_DISCONNECT, TYPE_PING, TYPE_USER = range(6)
TYPE_NAMES = {0: "SYN", 1: "CONNECT", 2: "DATA", 3: "DISCONNECT", 4: "PING", 5: "USER"}

FLAG_ACK, FLAG_RELIABLE, FLAG_NEED_ACK, FLAG_HAS_SIZE, FLAG_MULTI_ACK = 0x1, 0x2, 0x4, 0x8, 0x200
FLAG_NAMES = [(FLAG_ACK, "ACK"), (FLAG_RELIABLE, "RELIABLE"), (FLAG_NEED_ACK, "NEED_ACK"), (FLAG_HAS_SIZE, "HAS_SIZE"),
              (FLAG_MULTI_ACK, "MULTI_ACK")]

OPT_SUPPORT, OPT_CONNECTION_SIG, OPT_FRAGMENT_ID, OPT_INITIAL_SEQ, OPT_MAX_SUBSTREAM = 0, 1, 2, 3, 4
OPT_CONNECTION_SIG_LITE = 0x80
OPTION_NAMES = {0: "supported functions", 1: "connection signature", 2: "fragment id", 3: "initial sequence id",
                4: "max substream id", 0x80: "lite connection signature"}

STREAM_NAMES = {1: "DO", 2: "RV", 3: "OldRVSec", 4: "SBMGMT", 5: "NAT", 6: "SessionDiscovery", 7: "NATEcho",
                8: "Routing", 9: "Game", 10: "RVSecure", 11: "Relay"}


@dataclass
class Packet:
    type: int
    flags: int = 0
    src_type: int = 10
    dst_type: int = 10
    src_port: int = 0
    dst_port: int = 0
    fragment: int = 0
    seq: int = 0
    options: list[tuple[int, bytes]] = field(default_factory=list)
    payload: bytes = b""

    def option(self, oid: int) -> bytes | None:
        for i, v in self.options:
            if i == oid:
                return v
        return None

    def describe(self) -> str:
        flags = "|".join(n for f, n in FLAG_NAMES if self.flags & f) or "-"
        opts = ", ".join(f"{OPTION_NAMES.get(i, hex(i))}={v.hex()}" for i, v in self.options)
        return (f"{TYPE_NAMES.get(self.type, self.type)} flags {flags} "
                f"{STREAM_NAMES.get(self.src_type, self.src_type)}:{self.src_port} -> "
                f"{STREAM_NAMES.get(self.dst_type, self.dst_type)}:{self.dst_port} seq {self.seq} frag {self.fragment}"
                f"{' options {' + opts + '}' if opts else ''} payload {len(self.payload)} bytes")


def decode_options(data: bytes) -> list[tuple[int, bytes]]:
    out, i = [], 0
    while i + 2 <= len(data):
        oid, n = data[i], data[i + 1]
        out.append((oid, data[i + 2:i + 2 + n]))
        i += 2 + n
    return out


def encode_options(options: list[tuple[int, bytes]]) -> bytes:
    return b"".join(bytes([oid, len(v)]) + v for oid, v in options)


def decode(data: bytes) -> tuple[list[Packet], bytes]:
    """All whole packets in data, and what is left over."""
    packets = []
    while len(data) >= 12:
        if data[0] != MAGIC:
            raise ValueError(f"not a PRUDP lite packet (first byte {data[0]:#x})")
        optlen, paylen = data[1], struct.unpack_from("<H", data, 2)[0]
        end = 12 + optlen + paylen
        if len(data) < end:
            break
        streams, sport, dport, frag, tf, seq = struct.unpack_from("<BBBBHH", data, 4)
        packets.append(Packet(type=tf & 0xF, flags=tf >> 4, src_type=streams >> 4, dst_type=streams & 0xF,
                              src_port=sport, dst_port=dport, fragment=frag, seq=seq,
                              options=decode_options(data[12:12 + optlen]), payload=data[12 + optlen:end]))
        data = data[end:]
    return packets, data


def encode(p: Packet) -> bytes:
    opts = encode_options(p.options)
    head = struct.pack("<BBHBBBBHH", MAGIC, len(opts), len(p.payload), p.src_type << 4 | p.dst_type, p.src_port,
                       p.dst_port, p.fragment, p.type | p.flags << 4, p.seq)
    return head + opts + p.payload
