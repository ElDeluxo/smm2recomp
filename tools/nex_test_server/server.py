"""The local NEX test server (see __init__.py): TLS -> HTTP/1.1 WebSocket upgrade -> PRUDP Lite -> RMC.

What it answers: the PRUDP handshake (SYN, CONNECT), acknowledgements, pings and disconnects. The game's first
request (its login to the authentication server) is decoded and logged; it is answered with an RMC error for now
(the login flow itself is a later step: see recomp/docs/mods.md "The game's own network stack").
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import hashlib
import hmac
import os
import ssl
import sys
import time
from pathlib import Path

from . import certs, prudp, rmc, websocket

# Core::NotImplemented [inf: NintendoClients' error code list]; the game shows a communication error
ERROR_NOT_IMPLEMENTED = 0x80010002


class Log:
    def __init__(self, path: Path | None, capture: Path | None) -> None:
        self.file = path.open("a") if path else None
        self.capture = capture.open("a") if capture else None
        self.t0 = time.time()

    def __call__(self, msg: str) -> None:
        line = f"[{time.time() - self.t0:8.3f}] {msg}"
        print(line, flush=True)
        if self.file:
            self.file.write(line + "\n")
            self.file.flush()

    def raw(self, conn: int, direction: str, what: str, data: bytes) -> None:
        if self.capture:
            self.capture.write(f"{time.time() - self.t0:.3f} conn {conn} {direction} {what} {data.hex()}\n")
            self.capture.flush()


def account_key(folder: Path) -> bytes | None:
    """The recomp's local account key (runtime/net/token.c writes net/account.txt), to check its token's signature"""
    p = folder / "account.txt"
    if not p.exists():
        return None
    for ln in p.read_text().splitlines():
        if ln.startswith("key "):
            return bytes.fromhex(ln[4:].strip())
    return None


def check_jwt(token: str, key: bytes | None) -> str:
    parts = token.split(".")
    if len(parts) != 3:
        return "not a JWT"
    if key is None:
        return "signature not checked (no net/account.txt here)"
    mac = hmac.new(key, f"{parts[0]}.{parts[1]}".encode(), hashlib.sha256).digest()
    sig = base64.urlsafe_b64decode(parts[2] + "=" * (-len(parts[2]) % 4))
    return "signature valid (the local account's key)" if hmac.compare_digest(mac, sig) else "signature INVALID"


class PrudpSession:
    """One PRUDP Lite connection carried by one WebSocket."""

    def __init__(self, conn: int, log: Log, key: bytes | None) -> None:
        self.conn, self.log, self.key = conn, log, key
        self.seq = 1
        self.fragments: bytes = b""
        self.logins = 0

    def reply(self, p: prudp.Packet, type_: int, flags: int, options=None, payload: bytes = b"", seq=None) -> bytes:
        out = prudp.Packet(type=type_, flags=flags, src_type=p.dst_type, dst_type=p.src_type, src_port=p.dst_port,
                           dst_port=p.src_port, seq=p.seq if seq is None else seq, options=options or [],
                           payload=payload)
        self.log(f"conn {self.conn} S>C {out.describe()}")
        return prudp.encode(out)

    def feed(self, data: bytes) -> list[bytes]:
        out: list[bytes] = []
        packets, rest = prudp.decode(data)
        if rest:
            self.log(f"conn {self.conn} {len(rest)} bytes after the packets: {rest.hex()}")
        for p in packets:
            self.log(f"conn {self.conn} C>S {p.describe()}")
            out += self.handle(p)
        return out

    def handle(self, p: prudp.Packet) -> list[bytes]:
        ack = p.flags & prudp.FLAG_ACK
        support = p.option(prudp.OPT_SUPPORT)
        echo = [(prudp.OPT_SUPPORT, support)] if support is not None else []
        if p.type == prudp.TYPE_SYN and not ack:
            sig = os.urandom(16)
            opts = echo + [(prudp.OPT_CONNECTION_SIG, sig)]
            for oid in (prudp.OPT_MAX_SUBSTREAM,):
                v = p.option(oid)
                if v is not None:
                    opts.append((oid, v))
            return [self.reply(p, prudp.TYPE_SYN, prudp.FLAG_ACK | prudp.FLAG_HAS_SIZE, opts)]
        if p.type == prudp.TYPE_CONNECT and not ack:
            opts = list(echo)
            for oid in (prudp.OPT_CONNECTION_SIG_LITE, prudp.OPT_MAX_SUBSTREAM, prudp.OPT_INITIAL_SEQ):
                v = p.option(oid)
                if v is not None:
                    opts.append((oid, v))
            if p.payload:
                self.log(f"conn {self.conn} CONNECT payload ({len(p.payload)} bytes): {p.payload.hex()}")
            return [self.reply(p, prudp.TYPE_CONNECT, prudp.FLAG_ACK | prudp.FLAG_HAS_SIZE, opts)]
        if p.type == prudp.TYPE_PING and not ack:
            return [self.reply(p, prudp.TYPE_PING, prudp.FLAG_ACK | prudp.FLAG_HAS_SIZE)]
        if p.type == prudp.TYPE_DISCONNECT:
            return [self.reply(p, prudp.TYPE_DISCONNECT, prudp.FLAG_ACK | prudp.FLAG_HAS_SIZE)]
        if p.type == prudp.TYPE_DATA and not ack:
            out = []
            if p.flags & prudp.FLAG_NEED_ACK:
                out.append(self.reply(p, prudp.TYPE_DATA, prudp.FLAG_ACK | prudp.FLAG_HAS_SIZE))
            self.fragments += p.payload
            if p.fragment == 0:  # (0: the last fragment of a message)
                msg, self.fragments = self.fragments, b""
                out += self.request(p, msg)
            return out
        return []

    def request(self, p: prudp.Packet, msg: bytes) -> list[bytes]:
        self.log.raw(self.conn, "C>S", "rmc", msg)
        req = rmc.parse_request(msg)
        if not req:
            self.log(f"conn {self.conn} not an RMC request: {msg.hex()}")
            return []
        self.log(f"conn {self.conn} RMC request {req.name}")
        if req.protocol == 10:
            self.logins += 1
            for ln in rmc.decode_login(req):
                self.log(f"conn {self.conn}   {ln}")
            tok = self.token_of(req)
            if tok:
                self.log(f"conn {self.conn}   token check: {check_jwt(tok, self.key)}")
        else:
            self.log(f"conn {self.conn}   parameters: {req.params.hex()}")
        body = rmc.error(req.protocol, req.call_id, ERROR_NOT_IMPLEMENTED)
        self.log(f"conn {self.conn} answering {req.name} with error {ERROR_NOT_IMPLEMENTED:#010x} (not implemented yet)")
        seq, self.seq = self.seq, self.seq + 1
        return [self.reply(p, prudp.TYPE_DATA, prudp.FLAG_RELIABLE | prudp.FLAG_NEED_ACK | prudp.FLAG_HAS_SIZE,
                           payload=body, seq=seq)]

    @staticmethod
    def token_of(req: rmc.Request) -> str | None:
        # the first string that looks like a JWT, wherever the layout puts it
        data = req.params
        for i in range(len(data) - 2):
            n = int.from_bytes(data[i:i + 2], "little")
            if 40 < n <= len(data) - i - 2 and data[i + 1 + n] == 0:
                s = data[i + 2:i + 1 + n]
                if s.count(b".") >= 2 and s[:2] == b"ey":
                    return s.decode("ascii", "replace")
        return None


async def serve(args: argparse.Namespace) -> None:
    folder = Path(args.certs)
    log = Log(Path(args.log) if args.log else None, Path(args.capture) if args.capture else None)
    ca, crt, key = certs.ensure(folder, log)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(crt, key)
    ctx.set_alpn_protocols(["http/1.1"])
    sni: dict[int, str] = {}

    def on_sni(sock, name, _ctx):  # (record the name the client asked for)
        sni[id(sock)] = name or ""

    ctx.sni_callback = on_sni
    counter = [0]

    async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        counter[0] += 1
        conn = counter[0]
        peer = writer.get_extra_info("peername")
        sslobj = writer.get_extra_info("ssl_object")
        name = sni.get(id(sslobj), "") if sslobj else ""
        if sslobj and not name:
            name = getattr(sslobj, "server_hostname", "") or ""
        log(f"conn {conn} from {peer[0]}:{peer[1]}: TLS {sslobj.version() if sslobj else '?'} "
            f"{sslobj.cipher()[0] if sslobj else ''} ALPN {sslobj.selected_alpn_protocol() if sslobj else None}")
        try:
            head = await reader.readuntil(b"\r\n\r\n")
            method, path, headers = websocket.parse_request(head)
            log(f"conn {conn} HTTP {method} {path} (Host: {headers.get('host', '?')})")
            for k, v in headers.items():
                shown = v if k not in ("authorization", "cookie") else f"<{len(v)} chars, masked>"
                log(f"conn {conn}   {k}: {shown}")
            log.raw(conn, "C>S", "http", head)
            if headers.get("upgrade", "").lower() != "websocket":
                writer.write(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
                await writer.drain()
                log(f"conn {conn} not a WebSocket upgrade: 404")
                return
            writer.write(websocket.upgrade_response(headers))
            await writer.drain()
            log(f"conn {conn} WebSocket upgraded")
            session = PrudpSession(conn, log, account_key(folder))
            buf, msg, msg_op = b"", b"", 0
            while True:
                data = await reader.read(65536)
                if not data:
                    log(f"conn {conn} closed by the client")
                    return
                buf += data
                while True:
                    fr, used = websocket.parse_frame(buf)
                    if not fr:
                        break
                    buf = buf[used:]
                    fin, op, payload = fr
                    if op == websocket.OP_PING:
                        writer.write(websocket.frame(websocket.OP_PONG, payload))
                        continue
                    if op == websocket.OP_CLOSE:
                        writer.write(websocket.frame(websocket.OP_CLOSE, payload[:2]))
                        await writer.drain()
                        log(f"conn {conn} WebSocket closed by the client")
                        return
                    if op in (websocket.OP_BINARY, websocket.OP_TEXT):
                        msg, msg_op = payload, op
                    elif op == websocket.OP_CONT:
                        msg += payload
                    if not fin:
                        continue
                    log.raw(conn, "C>S", "ws", msg)
                    if msg_op != websocket.OP_BINARY:
                        log(f"conn {conn} text message: {msg[:200]!r}")
                        continue
                    for pkt in session.feed(msg):
                        log.raw(conn, "S>C", "ws", pkt)
                        writer.write(websocket.frame(websocket.OP_BINARY, pkt))
                    await writer.drain()
        except (asyncio.IncompleteReadError, ConnectionError) as e:
            log(f"conn {conn} ended: {e.__class__.__name__}")
        except ValueError as e:
            log(f"conn {conn} bad data: {e}")
        finally:
            writer.close()

    host = args.addr
    server = await asyncio.start_server(handle, host, args.port, ssl=ctx)
    log(f"local NEX test server on {host}:{args.port} (TLS; CA {ca}); the recomp: backend=local-nex "
        f"nex_test_server={host}:{args.port}")
    async with server:
        await server.serve_forever()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="nex_test_server", description=__doc__.splitlines()[0])
    ap.add_argument("--addr", default="127.0.0.1", help="local address to listen on (default 127.0.0.1)")
    ap.add_argument("--port", type=int, default=44300, help="TLS port (default 44300; the recomp maps 443 to it)")
    ap.add_argument("--certs", default="recomp/private/smm2/net",
                    help="private folder for the test CA and certificate (default recomp/private/smm2/net: the "
                         "recomp's data folder net/, where it looks for test-ca.pem)")
    ap.add_argument("--log", help="also append the log here")
    ap.add_argument("--capture", help="append every message (hex) here")
    args = ap.parse_args(argv)
    ip = args.addr.split(".")
    if not (args.addr == "127.0.0.1" or ip[0] in ("127", "10") or args.addr.startswith(("192.168.", "169.254."))
            or (ip[0] == "172" and 16 <= int(ip[1]) <= 31) or (ip[0] == "100" and 64 <= int(ip[1]) <= 127)):
        print("the test server listens on local addresses only", file=sys.stderr)
        return 2
    try:
        asyncio.run(serve(args))
    except KeyboardInterrupt:
        pass
    return 0
