"""The local NEX test server's codecs (tools/nex_test_server), on synthetic messages built here."""

import base64
import json
import struct
import unittest

from tools.nex_test_server import kerberos, nex, prudp, rmc, websocket
from tools.nex_test_server.nex import check_jwt, token_of


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
        token = jwt({"sub": "57cdf0b7b8f69c43", "nnex": "nx2.c2VjcmV0.c2ln"}, key)
        req = rmc.parse_request(login_request(token))
        self.assertEqual(req.name, "Authentication.ValidateAndRequestTicketWithParam")
        lines = "\n".join(rmc.decode_login(req))
        self.assertIn("platform type: 3", lines)
        self.assertIn("NEX version: 40605", lines)
        self.assertIn("signature <43 chars, masked>", lines)
        self.assertNotIn(token.split(".")[2], lines)  # the signature is never printed
        self.assertNotIn("nx2.", lines)  # nor a NEX login token
        self.assertNotIn("left over", lines)
        self.assertEqual(token_of(req), token)
        self.assertIn("valid", check_jwt(token, key))
        self.assertIn("INVALID", check_jwt(token, bytes(32)))

    def test_handshake_answers(self):
        s = nex.Session(nex.Server(lambda msg: None, None), 1)
        syn = prudp.Packet(type=prudp.TYPE_SYN, flags=prudp.FLAG_NEED_ACK, src_port=31, dst_port=1,
                           options=[(0, b"\x06\0\0\0")])
        (ack,), _ = prudp.decode(s.feed(prudp.encode(syn))[0])
        self.assertEqual((ack.type, ack.flags & prudp.FLAG_ACK, ack.dst_port), (prudp.TYPE_SYN, prudp.FLAG_ACK, 31))
        self.assertEqual(len(ack.option(prudp.OPT_CONNECTION_SIG)), 16)

    def test_rc4_known_answer(self):  # (the classic "Key" / "Plaintext" vector)
        self.assertEqual(kerberos.rc4(b"Key", b"Plaintext").hex(), "bbf316e8d940af0ad3")

    def test_ticket_then_secure_connect(self):
        server = nex.Server(lambda msg: None, None)
        s = nex.Session(server, 1)
        conn = nex.Connection(server, 1, 31, 1)
        req = rmc.parse_request(login_request(jwt({"sub": "1"}, bytes(32))).replace(b"1234", b"4321"))
        r = rmc.Reader(server.validate_and_request_ticket_with_param(conn, req))
        r.struct_header()
        pid, ticket, url = r.u64(), r.buffer(), r.string()
        r.u64(), r.string()
        source_key = bytes.fromhex(r.string())
        self.assertEqual(pid, 4321)
        self.assertIn("sid=2", url)
        plain = kerberos.decrypt(source_key, ticket)
        session_key, target = plain[:32], int.from_bytes(plain[32:40], "little")
        self.assertEqual(target, nex.SECURE_PID)
        internal = rmc.Reader(plain[40:]).buffer()
        request = kerberos.encrypt(session_key, struct.pack("<QII", pid, 5, 0x1234))
        payload = kerberos.buffer(internal) + kerberos.buffer(request)
        connect = prudp.Packet(type=prudp.TYPE_CONNECT, flags=prudp.FLAG_RELIABLE | prudp.FLAG_NEED_ACK,
                               src_port=30, dst_port=2, seq=1, payload=payload)
        s.feed(prudp.encode(prudp.Packet(type=prudp.TYPE_SYN, flags=4, src_port=30, dst_port=2)))
        (ack,), _ = prudp.decode(s.feed(prudp.encode(connect))[0])
        self.assertEqual(ack.payload, struct.pack("<II", 4, 0x1235))
        self.assertEqual(s.conns[(30, 2)].pid, 4321)

    def test_resent_request_answered_once(self):
        server = nex.Server(lambda msg: None, None)
        s = nex.Session(server, 1)
        body = bytes([0x73 | 0x80]) + struct.pack("<II", 19, 125)
        data = prudp.encode(prudp.Packet(type=prudp.TYPE_DATA, flags=prudp.FLAG_RELIABLE | prudp.FLAG_NEED_ACK,
                                         src_port=30, dst_port=2, seq=8, payload=struct.pack("<I", len(body)) + body))
        self.assertEqual(len(s.feed(data)), 2)  # the ack and the answer
        self.assertEqual(len(s.feed(data)), 1)  # the resend: the ack only

    def test_big_answers_are_fragmented(self):
        server = nex.Server(lambda msg: None, None)
        server.handlers[(0x73, 99)] = lambda conn, req: bytes(3000)
        s = nex.Session(server, 1)
        body = bytes([0x73 | 0x80]) + struct.pack("<II", 1, 99)
        data = prudp.Packet(type=prudp.TYPE_DATA, flags=prudp.FLAG_RELIABLE | prudp.FLAG_NEED_ACK, src_port=30,
                            dst_port=2, seq=2, payload=struct.pack("<I", len(body)) + body)
        out = [prudp.decode(b)[0][0] for b in s.feed(prudp.encode(data))]
        self.assertEqual([p.fragment for p in out[1:]], [1, 2, 0])
        self.assertEqual(sum(len(p.payload) for p in out[1:]), 3000 + 14)


if __name__ == "__main__":
    unittest.main()
