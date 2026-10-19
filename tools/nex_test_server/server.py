"""The local NEX test server (see __init__.py): TLS -> HTTP/1.1 WebSocket upgrade -> PRUDP Lite -> RMC (nex.py)."""

from __future__ import annotations

import argparse
import asyncio
import ssl
import sys
import time
from pathlib import Path

from . import certs, nex, websocket


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


async def serve(args: argparse.Namespace) -> None:
    folder = Path(args.certs)
    log = Log(Path(args.log) if args.log else None, Path(args.capture) if args.capture else None)
    ca, crt, key = certs.ensure(folder, log)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(crt, key)
    ctx.set_alpn_protocols(["http/1.1"])
    sni: dict[int, str] = {}

    def on_sni(sock, name, _ctx):  # (the name the client asked for)
        sni[id(sock)] = name or ""

    ctx.sni_callback = on_sni
    server = nex.Server(log, account_key(folder), args.name)
    counter = [0]

    async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        counter[0] += 1
        conn = counter[0]
        peer = writer.get_extra_info("peername")
        sslobj = writer.get_extra_info("ssl_object")
        log(f"conn {conn} from {peer[0]}:{peer[1]}: TLS {sslobj.version() if sslobj else '?'} "
            f"{sslobj.cipher()[0] if sslobj else ''}, SNI {sni.get(id(sslobj), '-') if sslobj else '-'}, "
            f"ALPN {sslobj.selected_alpn_protocol() if sslobj else None}")
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
            session = nex.Session(server, conn)
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

    srv = await asyncio.start_server(handle, args.addr, args.port, ssl=ctx)
    log(f"local NEX test server on {args.addr}:{args.port} (TLS; CA {ca}); the recomp: backend=local-nex "
        f"nex_test_server={args.addr}:{args.port}")
    async with srv:
        await srv.serve_forever()


def is_local(addr: str) -> bool:
    try:
        a = [int(x) for x in addr.split(".")]
    except ValueError:
        return False
    return len(a) == 4 and (a[0] in (127, 10) or a[:2] == [192, 168] or a[:2] == [169, 254]
                            or (a[0] == 172 and 16 <= a[1] <= 31) or (a[0] == 100 and 64 <= a[1] <= 127))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="nex_test_server", description="A local NEX test server for the SMM2 recomp")
    ap.add_argument("--addr", default="127.0.0.1", help="local address to listen on (default 127.0.0.1)")
    ap.add_argument("--port", type=int, default=44300, help="TLS port (default 44300; the recomp maps 443 to it)")
    ap.add_argument("--certs", default="recomp/private/smm2/net",
                    help="private folder for the test CA and certificate (default recomp/private/smm2/net: the "
                         "recomp's data folder net/, where it looks for test-ca.pem)")
    ap.add_argument("--name", default="Player", help="the player's name until the game registers one")
    ap.add_argument("--log", help="also append the log here")
    ap.add_argument("--capture", help="append every message (hex) here")
    args = ap.parse_args(argv)
    if not is_local(args.addr):
        print("the test server listens on local addresses only", file=sys.stderr)
        return 2
    try:
        asyncio.run(serve(args))
    except KeyboardInterrupt:
        pass
    return 0
