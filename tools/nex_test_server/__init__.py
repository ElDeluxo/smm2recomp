"""A local NEX test server for the SMM2 recomp's own network stack (backend local-nex).

Our own code, written from public protocol documentation (kinnay's NintendoClients wiki: PRUDP, RMC, the NEX
types and the authentication protocol) and from what the game sends. It accepts the game's TLS connection, completes
the WebSocket upgrade, answers PRUDP Lite, issues the login ticket, runs the secure connection and answers SMM2's
DataStore login calls (nex.py), enough for Course World's top screen. It only ever listens on a local address; it
connects to nothing.

    python3 -m tools.nex_test_server --certs recomp/private/smm2/net      (from the repository root)

The first run makes a local test CA and a server certificate in --certs (private; never committed). The recomp
trusts that CA with tls_trust=local-ca (runtime/net/netcfg.h).
"""
