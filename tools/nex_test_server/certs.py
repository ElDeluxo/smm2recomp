"""The local test CA and the test server's certificate, made with the openssl command on first use, into a private
folder (recomp/private/...: git ignores it and scripts/check_no_proprietary.py blocks keys and certificates). The
recomp trusts the CA with tls_trust=local-ca; nothing else should."""

from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path

# The names the game connects to (resolved to the test server by the recomp's redirect table). Wildcards cover one
# label, so each level the game uses is listed.
SERVER_NAMES = ["*.s.n.srv.nintendo.net", "*.n.n.srv.nintendo.net", "*.srv.nintendo.net", "*.nintendo.net",
                "*.baas.nintendo.com", "*.nintendo.com", "localhost"]


def ensure(folder: Path, log=print) -> tuple[Path, Path, Path]:
    """(ca certificate, server certificate, server key) in folder, made if missing"""
    folder.mkdir(parents=True, exist_ok=True)
    ca, ca_key = folder / "test-ca.pem", folder / "test-ca.key"
    crt, key = folder / "test-server.pem", folder / "test-server.key"
    if ca.exists() and crt.exists() and key.exists():
        return ca, crt, key

    def run(*args: str) -> None:
        subprocess.run(["openssl", *args], check=True, capture_output=True)

    log(f"making a local test CA and server certificate in {folder} (private)")
    run("req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes", "-days", "3650",
        "-subj", "/CN=SMM2 Recomp local NEX test CA", "-keyout", str(ca_key), "-out", str(ca),
        "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign")
    with tempfile.TemporaryDirectory() as tmp:
        csr, ext = Path(tmp) / "server.csr", Path(tmp) / "ext.cnf"
        run("req", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes",
            "-subj", "/CN=SMM2 Recomp local NEX test server", "-keyout", str(key), "-out", str(csr))
        sans = ",".join([f"DNS:{n}" for n in SERVER_NAMES] + ["IP:127.0.0.1"])
        ext.write_text(f"subjectAltName={sans}\nbasicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\n"
                       "extendedKeyUsage=serverAuth\nsubjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid,issuer\n")
        run("x509", "-req", "-in", str(csr), "-CA", str(ca), "-CAkey", str(ca_key), "-CAcreateserial",
            "-days", "825", "-extfile", str(ext), "-out", str(crt))
    for p in (ca_key, key):
        p.chmod(0o600)
    return ca, crt, key
