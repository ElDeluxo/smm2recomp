"""RMC (NEX remote method calls) and the NEX types the login uses (public: kinnay's NintendoClients wiki, "RMC
Protocol", "NEX Common Types", "Authentication Protocol").

Request, little-endian:  u32 size, u8 protocol | 0x80, [u16 protocol if 0x7f], u32 call id, u32 method id, params
Response:                u32 size, u8 protocol, u8 1, u32 call id, u32 method id | 0x8000, data
Error response:          u32 size, u8 protocol, u8 0, u32 error code, u32 call id
"""

from __future__ import annotations

import base64
import json
import struct
from dataclasses import dataclass

PROTOCOLS = {10: "Authentication", 11: "SecureConnection", 0x14: "Friends", 0x15: "MatchMaking", 0x6d: "Utility",
             0x73: "DataStore", 0x6e: "Ranking", 0x6f: "MatchmakeExtension", 0x7f: "(extended)", 3: "NAT traversal",
             0x65: "Health", 0x66: "Monitoring", 0x71: "Subscriber"}
AUTH_METHODS = {1: "ValidateAndRequestTicket", 2: "ValidateAndRequestTicketWithCustomData", 3: "RequestTicket",
                4: "GetPID", 5: "GetName", 6: "ValidateAndRequestTicketWithParam"}
SECURE_METHODS = {1: "Register", 2: "RequestConnectionData", 3: "RequestUrls", 4: "RegisterEx", 5: "TestConnectivity",
                  6: "UpdateURLs", 7: "ReplaceURL", 8: "SendReport"}


@dataclass
class Request:
    protocol: int
    call_id: int
    method: int
    params: bytes

    @property
    def name(self) -> str:
        methods = AUTH_METHODS if self.protocol == 10 else SECURE_METHODS if self.protocol == 11 else {}
        return f"{PROTOCOLS.get(self.protocol, hex(self.protocol))}.{methods.get(self.method, self.method)}"


def parse_request(data: bytes) -> Request | None:
    if len(data) < 13:
        return None
    size = struct.unpack_from("<I", data, 0)[0]
    proto = data[4]
    if not proto & 0x80:
        return None
    proto &= 0x7F
    off = 5
    if proto == 0x7F:
        proto = struct.unpack_from("<H", data, off)[0]
        off += 2
    call_id, method = struct.unpack_from("<II", data, off)
    return Request(proto, call_id, method, data[off + 8:4 + size])


def response(protocol: int, call_id: int, method: int, body: bytes) -> bytes:
    head = (bytes([protocol]) if protocol < 0x7F else bytes([0x7F]) + struct.pack("<H", protocol))
    rest = head + b"\x01" + struct.pack("<II", call_id, method | 0x8000) + body
    return struct.pack("<I", len(rest)) + rest


def error(protocol: int, call_id: int, code: int) -> bytes:
    rest = bytes([protocol]) + b"\x00" + struct.pack("<II", code, call_id)
    return struct.pack("<I", len(rest)) + rest


class Reader:
    """NEX types: string = u16 length (with the NUL) + bytes; buffer = u32 length + bytes; qbuffer = u16 + bytes."""

    def __init__(self, data: bytes) -> None:
        self.data, self.pos = data, 0

    def left(self) -> int:
        return len(self.data) - self.pos

    def take(self, n: int) -> bytes:
        if n > self.left():
            raise ValueError(f"wanted {n} bytes, {self.left()} left")
        b = self.data[self.pos:self.pos + n]
        self.pos += n
        return b

    def u8(self) -> int:
        return self.take(1)[0]

    def u16(self) -> int:
        return struct.unpack("<H", self.take(2))[0]

    def u32(self) -> int:
        return struct.unpack("<I", self.take(4))[0]

    def u64(self) -> int:
        return struct.unpack("<Q", self.take(8))[0]

    def string(self) -> str:
        n = self.u16()
        return self.take(n).rstrip(b"\0").decode("utf-8", "replace")

    def buffer(self) -> bytes:
        return self.take(self.u32())

    def struct_header(self) -> tuple[int, int]:
        """A structure's version header (NEX 3.5 and later, as on the Switch): u8 version, u32 content length."""
        return self.u8(), self.u32()


class Writer:
    def __init__(self) -> None:
        self.parts: list[bytes] = []

    def raw(self, b: bytes) -> Writer:
        self.parts.append(b)
        return self

    def u8(self, v: int) -> Writer:
        return self.raw(struct.pack("<B", v))

    def u32(self, v: int) -> Writer:
        return self.raw(struct.pack("<I", v))

    def u64(self, v: int) -> Writer:
        return self.raw(struct.pack("<Q", v))

    def string(self, s: str) -> Writer:
        b = s.encode() + b"\0"
        return self.raw(struct.pack("<H", len(b)) + b)

    def buffer(self, b: bytes) -> Writer:
        return self.raw(struct.pack("<I", len(b)) + b)

    def bytes(self) -> bytes:
        return b"".join(self.parts)


def mask_token(token: str) -> str:
    """A JWT with its signature masked, plus its decoded header and claims (they are not secret)."""
    parts = token.split(".")
    if len(parts) != 3:
        return f"<{len(token)} chars>" if len(token) > 16 else token

    def dec(p: str) -> object:
        try:
            return json.loads(base64.urlsafe_b64decode(p + "=" * (-len(p) % 4)))
        except ValueError:
            return p

    return f"JWT header {json.dumps(dec(parts[0]))} claims {json.dumps(dec(parts[1]))} signature <{len(parts[2])} chars, masked>"


def decode_login(req: Request) -> list[str]:
    """What the game's login carries, field by field (best effort: the layout is from public docs, so every field
    that does not parse is shown raw)."""
    out = [f"{req.name} (protocol {req.protocol}, method {req.method}, call {req.call_id}, {len(req.params)} bytes of parameters)"]
    r = Reader(req.params)
    try:
        if req.protocol == 10 and req.method == 6:  # ValidateAndRequestTicketWithParam(ValidateAndRequestTicketParam)
            ver, length = r.struct_header()
            out.append(f"  ValidateAndRequestTicketParam (structure version {ver}, {length} bytes)")
            out.append(f"    platform type: {r.u32()}")
            out.append(f"    user name: {r.string()!r}")
            name = r.string()
            r.u32()
            inner = Reader(r.take(r.u32()))
            out.append(f"    data: AnyDataHolder {name!r} ({inner.left()} bytes)")
            if name == "AuthenticationInfo":
                iv, il = inner.struct_header()  # AuthenticationInfo derives from Data
                out.append(f"      (Data base: structure version {iv}, {il} bytes)")
                dv, dl = inner.struct_header()
                out.append(f"      AuthenticationInfo (structure version {dv}, {dl} bytes)")
                tok = inner.string()
                out.append(f"      token: {mask_token(tok)}")
                out.append(f"      NGS version: {inner.u32()}")
                out.append(f"      token type: {inner.u8()}")
                out.append(f"      server version: {inner.u32()}")
            elif inner.left():
                out.append(f"      raw: {inner.take(inner.left()).hex()}")
            out.append(f"    skip version check: {bool(r.u8())}")
            out.append(f"    NEX version: {r.u32()}")
            out.append(f"    client version: {r.u32()}")
        elif req.protocol == 10 and req.method in (1, 2):  # ValidateAndRequestTicket(WithCustomData)
            out.append(f"  user name: {r.string()!r}")
            if req.method == 2:
                name = r.string()
                r.u32()
                out.append(f"  custom data: {name!r} {r.buffer().hex()}")
        if r.left():
            out.append(f"  (left over: {r.take(r.left()).hex()})")
    except ValueError as e:
        out.append(f"  (stopped: {e}; the rest raw: {req.params[r.pos:].hex()})")
    return out
