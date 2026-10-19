"""NEX's Kerberos-style tickets (public: kinnay's NintendoClients wiki, "Kerberos Authentication").

Encryption: RC4 with the key, then HMAC-MD5 (same key) of the ciphertext appended.
Client ticket (encrypted with the source key): session key (32 bytes), target PID (u64), buffer: internal ticket.
The internal ticket is the server's own business: here a buffer encrypted with this server's key holding
{u64 issue time, u64 source PID, session key}.
"""

from __future__ import annotations

import hashlib
import hmac
import struct
import time

SESSION_KEY_SIZE = 32


def rc4(key: bytes, data: bytes) -> bytes:
    s = list(range(256))
    j = 0
    for i in range(256):
        j = (j + s[i] + key[i % len(key)]) & 0xFF
        s[i], s[j] = s[j], s[i]
    out = bytearray(len(data))
    i = j = 0
    for n, b in enumerate(data):
        i = (i + 1) & 0xFF
        j = (j + s[i]) & 0xFF
        s[i], s[j] = s[j], s[i]
        out[n] = b ^ s[(s[i] + s[j]) & 0xFF]
    return bytes(out)


def encrypt(key: bytes, data: bytes) -> bytes:
    enc = rc4(key, data)
    return enc + hmac.new(key, enc, hashlib.md5).digest()


def decrypt(key: bytes, data: bytes) -> bytes:
    enc, mac = data[:-16], data[-16:]
    if not hmac.compare_digest(hmac.new(key, enc, hashlib.md5).digest(), mac):
        raise ValueError("Kerberos check failed (wrong key)")
    return rc4(key, enc)


def buffer(b: bytes) -> bytes:
    return struct.pack("<I", len(b)) + b


def client_ticket(source_key: bytes, server_key: bytes, session_key: bytes, source_pid: int, target_pid: int) -> bytes:
    internal = encrypt(server_key, struct.pack("<QQ", int(time.time()), source_pid) + session_key)
    return encrypt(source_key, session_key + struct.pack("<Q", target_pid) + buffer(internal))


def open_internal(server_key: bytes, internal: bytes) -> tuple[int, int, bytes]:
    """(issue time, source PID, session key) from an internal ticket"""
    plain = decrypt(server_key, internal)
    issued, pid = struct.unpack_from("<QQ", plain)
    return issued, pid, plain[16:16 + SESSION_KEY_SIZE]
