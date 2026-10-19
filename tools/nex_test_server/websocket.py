"""The server side of WebSocket (RFC 6455): the HTTP/1.1 upgrade and frames."""

from __future__ import annotations

import base64
import hashlib
import struct

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
OP_CONT, OP_TEXT, OP_BINARY, OP_CLOSE, OP_PING, OP_PONG = 0, 1, 2, 8, 9, 10


def parse_request(head: bytes) -> tuple[str, str, dict[str, str]]:
    """'GET /path HTTP/1.1' + headers -> (method, path, headers with lower-case names)"""
    lines = head.decode("latin-1").split("\r\n")
    method, path = (lines[0].split(" ") + ["", ""])[:2]
    headers = {}
    for ln in lines[1:]:
        if ":" in ln:
            k, v = ln.split(":", 1)
            headers[k.strip().lower()] = v.strip()
    return method, path, headers


def accept_key(key: str) -> str:
    return base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()


def upgrade_response(headers: dict[str, str]) -> bytes:
    lines = ["HTTP/1.1 101 Switching Protocols", "Upgrade: websocket", "Connection: Upgrade",
             f"Sec-WebSocket-Accept: {accept_key(headers.get('sec-websocket-key', ''))}"]
    if "sec-websocket-protocol" in headers:  # (the first protocol the client offers)
        lines.append(f"Sec-WebSocket-Protocol: {headers['sec-websocket-protocol'].split(',')[0].strip()}")
    return ("\r\n".join(lines) + "\r\n\r\n").encode()


def parse_frame(buf: bytes) -> tuple[tuple[bool, int, bytes] | None, int]:
    """One frame from buf: ((fin, opcode, payload) or None if incomplete, bytes used)"""
    if len(buf) < 2:
        return None, 0
    fin, op = bool(buf[0] & 0x80), buf[0] & 0x0F
    masked, n = bool(buf[1] & 0x80), buf[1] & 0x7F
    off = 2
    if n == 126:
        if len(buf) < 4:
            return None, 0
        n = struct.unpack_from(">H", buf, 2)[0]
        off = 4
    elif n == 127:
        if len(buf) < 10:
            return None, 0
        n = struct.unpack_from(">Q", buf, 2)[0]
        off = 10
    mask = b""
    if masked:
        if len(buf) < off + 4:
            return None, 0
        mask = buf[off:off + 4]
        off += 4
    if len(buf) < off + n:
        return None, 0
    data = buf[off:off + n]
    if masked:
        data = bytes(b ^ mask[i & 3] for i, b in enumerate(data))
    return (fin, op, data), off + n


def frame(op: int, payload: bytes) -> bytes:
    n = len(payload)
    if n < 126:
        head = struct.pack(">BB", 0x80 | op, n)
    elif n < 65536:
        head = struct.pack(">BBH", 0x80 | op, 126, n)
    else:
        head = struct.pack(">BBQ", 0x80 | op, 127, n)
    return head + payload
