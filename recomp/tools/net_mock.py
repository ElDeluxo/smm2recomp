#!/usr/bin/env python3
"""A tiny HTTP(S) mock for tools/test_net.sh (stdlib only).
Usage: net_mock.py --port N [--tls cert.pem key.pem]
  GET /hello         -> 200 "hello"
  GET /echo-headers  -> 200, the request headers as received
Prints "ready" once it listens."""
import argparse, http.server, ssl, sys


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def reply(self, code, body):
        b = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(b)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        if self.path == "/hello":
            self.reply(200, "hello")
        elif self.path == "/echo-headers":
            self.reply(200, "".join(f"{k}: {v}\r\n" for k, v in self.headers.items()))
        else:
            self.reply(404, "not found")

    def log_message(self, *a):
        pass


ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, required=True)
ap.add_argument("--tls", nargs=2, metavar=("CERT", "KEY"))
a = ap.parse_args()
srv = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), H)
if a.tls:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(*a.tls)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
print("ready", flush=True)
srv.serve_forever()
