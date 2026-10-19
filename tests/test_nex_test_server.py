"""The local NEX test server's codecs (tools/nex_test_server), on synthetic messages built here."""

import base64
import json
import struct
import unittest

from tools.nex_test_server import prudp, rmc, websocket
from tools.nex_test_server.server import PrudpSession, check_jwt


def jwt(claims: dict, key: bytes) -> str:
    import hashlib
    import hmac

    def b64(b: bytes) -> str:
        return base64.urlsafe_b64encode(b).rstrip(b"=").decode()

    head = b64(json.dumps({"alg": "HS256", "typ": "JWT"}).encode()) + "." + b64(json.dumps(claims).encode())
    return head + "." + b64(hmac.new(key, head.encode(), hashlib.sha256).digest())


def login_request(token: str) -> bytes:
    info = rmc.Writer().string(token).u32(4).u8(2).u32(0).bytes()
    info = rmc.Writer().u8(0).u32(0).u8(0).u32(len(info)).raw(info).bytes()  # Data base + AuthenticationInfo headers
    holder = rmc.Writer().string("AuthenticationInfo").u32(len(info) + 4).u32(len(info)).raw(info).bytes()
    param = rmc.Writer().u32(3).string("1234").raw(holder).u8(0).u32(40605).u32(60).bytes()
    param = rmc.Writer().u8(0).u32(len(param)).raw(param).bytes()
    body = bytes([10 | 0x80]) + struct.pack("<II", 7, 6) + param
    return struct.pack("<I", len(body)) + body


class Codecs(unittest.TestCase):
    def test_websocket_accept_key(self):  # RFC 6455's example
        self.assertEqual(websocket.accept_key("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")

    def test_websocket_masked_frame(self):
        mask, data = b"\x01\x02\x03\x04", b"hello"
        raw = bytes([0x82, 0x80 | len(data)]) + mask + bytes(b ^ mask[i & 3] for i, b in enumerate(data))
        (fin, op, payload), used = websocket.parse_frame(raw + b"x")
        self.assertEqual((fin, op, payload, used), (True, websocket.OP_BINARY, data, len(raw)))

    def test_prudp_lite_roundtrip(self):
        p = prudp.Packet(type=prudp.TYPE_CONNECT, flags=prudp.FLAG_RELIABLE | prudp.FLAG_NEED_ACK, src_port=31,
                         dst_port=1, seq=1, options=[(0, b"\x06\0\0\0"), (0x80, bytes(16))], payload=b"abc")
        (q,), rest = prudp.decode(prudp.encode(p) + b"\x80")
        self.assertEqual((q, rest), (p, b"\x80"))

    def test_login_decoded_and_token_checked(self):
        key = bytes(range(32))
        token = jwt({"sub": "57cdf0b7b8f69c43"}, key)
        req = rmc.parse_request(login_request(token))
        self.assertEqual(req.name, "Authentication.ValidateAndRequestTicketWithParam")
        lines = "\n".join(rmc.decode_login(req))
        self.assertIn("platform type: 3", lines)
        self.assertIn("NEX version: 40605", lines)
        self.assertIn("signature <43 chars, masked>", lines)
        self.assertNotIn(token.split(".")[2], lines)  # the signature is never printed
        self.assertNotIn("left over", lines)
        self.assertEqual(PrudpSession.token_of(req), token)
        self.assertIn("valid", check_jwt(token, key))
        self.assertIn("INVALID", check_jwt(token, bytes(32)))

    def test_handshake_answers(self):
        s = PrudpSession(1, lambda msg: None, None)
        syn = prudp.Packet(type=prudp.TYPE_SYN, flags=prudp.FLAG_NEED_ACK, src_port=31, dst_port=1,
                           options=[(0, b"\x06\0\0\0")])
        (ack,), _ = prudp.decode(s.feed(prudp.encode(syn))[0])
        self.assertEqual((ack.type, ack.flags & prudp.FLAG_ACK, ack.dst_port), (prudp.TYPE_SYN, prudp.FLAG_ACK, 31))
        self.assertEqual(len(ack.option(prudp.OPT_CONNECTION_SIG)), 16)


if __name__ == "__main__":
    unittest.main()
