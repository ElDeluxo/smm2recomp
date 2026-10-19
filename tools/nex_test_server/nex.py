"""The NEX side of the test server: PRUDP Lite connections and the RMC methods it answers.

On the Switch one WebSocket server is both the authentication server (PRUDP port 1) and the secure server (port 2)
[public: kinnay's wiki, "PRUDP Protocol", virtual ports]. Answered so far:
  - Authentication.ValidateAndRequestTicketWithParam: the account's PID, a ticket for the secure server, its station
    URL (0.0.0.1 port 1: "the same server"), the time, a build name and the source key;
  - Authentication.RequestTicket;
  - the secure server's CONNECT (the ticket checked, the client's check value + 1 back);
  - SecureConnection.Register / RegisterEx: the connection id and the client's own station URL;
  - Utility.GetIntegerSettings / GetStringSettings: empty;
  - DataStore (SMM2): RegisterUser, GetUsers (a user with a default Mii), SyncUserProfile, UpdateLastLoginTime,
    GetMiiClothes, PostActivityQuest, GetUserNameNgType, GetNewNotification, LoginCheck, UpdateLastLoginInfo,
    GetEventCourseStatus.
Every other request is logged with its parameters and answered Core::NotImplemented.
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import os
import struct
import time
from collections.abc import Callable

from . import kerberos, matchmaking, mii, prudp, rmc

CORE_NOT_IMPLEMENTED = 0x80010002
SECURE_PID = 257049437023956657  # Quazal Rendez-Vous on Switch servers [public: kinnay's wiki, special accounts]
FRAGMENT = 1300
RESULT_SUCCESS = 0x00010001


class RmcError(Exception):
    def __init__(self, code: int) -> None:
        super().__init__(f"{code:#010x}")
        self.code = code


def datetime_now() -> int:
    t = time.gmtime()
    return (t.tm_year << 26 | t.tm_mon << 22 | t.tm_mday << 17 | t.tm_hour << 12 | t.tm_min << 6 | t.tm_sec)


def structure(content: bytes, version: int = 0) -> bytes:
    return struct.pack("<BI", version, len(content)) + content


def check_jwt(token: str, keys: bytes | list[bytes] | None) -> str:
    parts = token.split(".")
    if len(parts) != 3:
        return "not a JWT"
    keys = [keys] if isinstance(keys, bytes) else keys or []
    if not keys:
        return "signature not checked (no net/account.txt here)"
    sig = base64.urlsafe_b64decode(parts[2] + "=" * (-len(parts[2]) % 4))
    for key in keys:
        mac = hmac.new(key, f"{parts[0]}.{parts[1]}".encode(), hashlib.sha256).digest()
        if hmac.compare_digest(mac, sig):
            return "signature valid (a local account's key)"
    return "signature INVALID"


def token_of(req: rmc.Request) -> str | None:
    """the first string in the parameters that looks like a JWT"""
    data = req.params
    for i in range(len(data) - 2):
        n = int.from_bytes(data[i:i + 2], "little")
        if 40 < n <= len(data) - i - 2 and data[i + 1 + n] == 0:
            s = data[i + 2:i + 1 + n]
            if s.count(b".") >= 2 and s[:2] == b"ey":
                return s.decode("ascii", "replace")
    return None


class Server:
    """State shared by every connection: this server's key, the account key (to check tokens), the handlers."""

    def __init__(self, log: Callable[[str], None], account_key: bytes | list[bytes] | None, name: str = "Player") -> None:
        self.log, self.account_key = log, account_key
        self.users: dict[int, dict] = {}  # PID -> {"name", "mii", "country", "region"} (registered this run)
        self.default_name = name
        self.config: dict[int, int] = {k: 1 for k in range(38)}  # (the application config, GetIntegerSettings(0))
        self.key = os.urandom(16)
        self.next_cid = 1
        self.players: dict[int, Connection] = {}  # PID -> its secure connection (for requests the server sends)
        self.urls: dict[int, list[str]] = {}       # PID -> its station URLs (Register, UpdateURLs, ReplaceURL)
        self.mm = matchmaking.Matchmaker()
        self.handlers: dict[tuple[int, int], Callable[[Connection, rmc.Request], bytes]] = {
            (10, 6): self.validate_and_request_ticket_with_param,
            (10, 3): self.request_ticket,
            (11, 1): self.register,
            (11, 4): self.register,
            (11, 6): self.update_urls,
            (11, 7): self.replace_url,
            (11, 8): self.empty,             # SendReport
            (3, 1): self.probe_initiation,
            (3, 3): self.probe_initiation,
            (3, 4): self.empty,              # ReportNATTraversalResult
            (3, 5): self.nat_properties,
            (3, 6): self.relay_signature_key,
            (3, 7): self.empty,              # ReportNATTraversalResultDetail
            (0x6D, 1): self.empty,           # CloseParticipation
            (0x6D, 2): self.empty,           # OpenParticipation
            (0x6D, 3): self.auto_matchmake,  # AutoMatchmake_Postpone
            (0x6D, 6): self.create_session,
            (0x6D, 7): self.join_session,
            (0x6D, 11): self.empty,          # UpdateApplicationBuffer
            (0x6D, 12): self.empty,          # UpdateMatchmakeSessionAttribute
            (0x6D, 14): self.empty,          # UpdateMatchmakeSession
            (0x6D, 15): self.auto_matchmake, # AutoMatchmakeWithSearchCriteria_Postpone
            (0x6D, 16): self.empty_list,     # GetPlayingSession
            (0x6D, 34): self.empty,          # UpdateProgressScore
            (0x6D, 38): self.create_session,
            (0x6D, 39): self.join_session,
            (0x6D, 40): self.auto_matchmake, # AutoMatchmakeWithParam_Postpone
            (0x15, 41): self.session_urls,
            (0x15, 42): self.empty,          # UpdateSessionHost
            (0x32, 1): self.end_participation,
            (0x6E, 7): self.integer_settings,
            (0x6E, 8): self.string_settings,
            (0x73, 47): self.register_user,
            (0x73, 48): self.get_users,
            (0x73, 49): self.sync_user_profile,
            (0x73, 59): self.empty,          # UpdateLastLoginTime
            (0x73, 63): self.empty_list,     # GetMiiClothes: no costumes
            (0x73, 64): self.empty,          # PostActivityQuest
            (0x73, 65): self.user_name_ng_type,
            (0x73, 151): self.empty,         # LoginCheck
            (0x73, 152): self.empty,         # UpdateLastLoginInfo
            (0x73, 125): self.new_notification,
            (0x73, 154): self.event_course_status,
        }

    # -- authentication
    def ticket_for(self, pid: int) -> tuple[bytes, str]:
        source_key = os.urandom(16)
        session_key = os.urandom(kerberos.SESSION_KEY_SIZE)
        return kerberos.client_ticket(source_key, self.key, session_key, pid, SECURE_PID), source_key.hex()

    def validate_and_request_ticket_with_param(self, conn: Connection, req: rmc.Request) -> bytes:
        for ln in rmc.decode_login(req):
            self.log(f"{conn.name}   {ln}")
        tok = token_of(req)
        if tok:
            self.log(f"{conn.name}   token check: {check_jwt(tok, self.account_key)}")
        r = rmc.Reader(req.params)
        r.struct_header()
        r.u32()
        user = r.string()
        pid = int(user) if user.isdigit() else 0
        ticket, source_key = self.ticket_for(pid)
        url = f"prudps:/address=0.0.0.1;port=1;CID=1;PID={SECURE_PID};sid=2;stream=10;type=2"
        body = (struct.pack("<Q", pid) + kerberos.buffer(ticket) + rmc.Writer().string(url).bytes()
                + struct.pack("<Q", datetime_now())
                + rmc.Writer().string("branch:recomp/local-nex-test build:4_6_5_0").string(source_key).bytes())
        self.log(f"{conn.name}   -> ticket for PID {pid}, secure server {url}, source key <masked>")
        return structure(body)

    def request_ticket(self, conn: Connection, req: rmc.Request) -> bytes:
        source, target = struct.unpack_from("<QQ", req.params)
        ticket, source_key = self.ticket_for(source)
        self.log(f"{conn.name}   RequestTicket {source} -> {target}")
        return struct.pack("<I", RESULT_SUCCESS) + kerberos.buffer(ticket) + rmc.Writer().string(source_key).bytes()

    # -- secure connection
    def secure_connect(self, conn: Connection, payload: bytes) -> bytes:
        r = rmc.Reader(payload)
        internal, request = r.buffer(), r.buffer()
        issued, pid, session_key = kerberos.open_internal(self.key, internal)
        plain = kerberos.decrypt(session_key, request)
        rpid, cid, check = struct.unpack_from("<QII", plain)
        conn.pid, conn.session_key = pid, session_key
        self.players[pid] = conn
        self.log(f"{conn.name} secure CONNECT: PID {rpid} (ticket for {pid}, {int(time.time()) - issued} s old), "
                 f"connection id {cid}")
        return struct.pack("<II", 4, (check + 1) & 0xFFFFFFFF)

    def register(self, conn: Connection, req: rmc.Request) -> bytes:
        r = rmc.Reader(req.params)
        urls = [r.string() for _ in range(r.u32())]
        for u in urls:
            self.log(f"{conn.name}   the client's station URL: {u}")
        if req.method == 4 and r.left():
            name = r.string()
            r.u32()
            data = r.take(r.u32())
            self.log(f"{conn.name}   login data: {name} ({len(data)} bytes)")
            tok = token_of(rmc.Request(0, 0, 0, data))
            if tok:
                self.log(f"{conn.name}   login data token: {rmc.mask_token(tok)} ({check_jwt(tok, self.account_key)})")
        cid, self.next_cid = self.next_cid, self.next_cid + 1
        public = urls[0] if urls else "prudp:/"
        self.urls[conn.pid or 0] = urls
        return struct.pack("<II", RESULT_SUCCESS, cid) + rmc.Writer().string(public).bytes()


    def update_urls(self, conn: Connection, req: rmc.Request) -> bytes:
        r = rmc.Reader(req.params)
        urls = [r.string() for _ in range(r.u32())]
        self.urls[conn.pid or 0] = urls
        self.log(f"{conn.name}   station URLs now: {urls}")
        return b""

    def replace_url(self, conn: Connection, req: rmc.Request) -> bytes:
        r = rmc.Reader(req.params)
        old, new = r.string(), r.string()
        urls = self.urls.setdefault(conn.pid or 0, [])
        self.urls[conn.pid or 0] = [new if u == old else u for u in urls] if old in urls else urls + [new]
        self.log(f"{conn.name}   station URL {old} -> {new}")
        return b""

    # -- NAT traversal [public: kinnay's wiki, "NAT Traversal Protocol"]
    def probe_initiation(self, conn: Connection, req: rmc.Request) -> bytes:
        r = rmc.Reader(req.params)
        targets = [r.string() for _ in range(r.u32())]
        mine = r.string() if req.method == 3 else (self.urls.get(conn.pid or 0) or [""])[-1]
        for t in targets:
            pid = next((p for p, us in self.urls.items() if t in us), None)
            if pid is None:
                pid = next((int(kv[4:]) for kv in t.split(";") if kv.startswith("PID=") and kv[4:].isdigit()), None)
            other = self.players.get(pid) if pid is not None else None
            self.log(f"{conn.name}   probe initiation towards {t} (player {pid}): "
                     f"{'InitiateProbe sent' if other else 'unknown station'}")
            if other:
                other.push(3, 2, rmc.Writer().string(mine).bytes(), f"NATTraversal.InitiateProbe({mine})")
        return b""

    def nat_properties(self, conn: Connection, req: rmc.Request) -> bytes:
        mapping, filtering, rtt = struct.unpack_from("<III", req.params)
        self.log(f"{conn.name}   NAT properties: mapping {mapping}, filtering {filtering}, rtt {rtt}")
        return b""

    def relay_signature_key(self, conn: Connection, req: rmc.Request) -> bytes:
        # no relay: mode 0, now, no address
        return struct.pack("<iQ", 0, datetime_now()) + rmc.Writer().string("").bytes() + struct.pack("<HiI", 0, 0, 0x22306D00)

    # -- matchmaking [public: kinnay's wiki, "Matchmake Extension Protocol", "Match Making Types"]
    def notify(self, to: int, source: int, kind: int, p1: int, p2: int, text: str = "", p3: int = 0) -> None:
        other = self.players.get(to)
        if other:
            ev = (struct.pack("<QIQQ", source, kind, p1, p2) + rmc.Writer().string(text).bytes()
                  + struct.pack("<Q", p3))
            other.push(14, 1, structure(ev), f"Notification {kind} (from {source}, {p1}, {p2})")

    def matched(self, conn: Connection, src: matchmaking.Session, how: str) -> matchmaking.Session:
        pid = conn.pid or 0
        s = self.mm.find_open(src, pid)
        if s:
            self.mm.join(s, pid)
            self.log(f"{conn.name}   {how}: joins session {s.gid} (game mode {s.game_mode}, attributes {s.attribs}, "
                     f"{len(s.participants)}/{s.max_part}: {s.participants})")
            self.notify(s.owner, pid, 3001, s.gid, pid)
        else:
            s = self.mm.new(src, pid)
            self.log(f"{conn.name}   {how}: new session {s.gid} (game mode {s.game_mode}, attributes {s.attribs}, "
                     f"max {s.max_part})")
        return s

    def auto_matchmake(self, conn: Connection, req: rmc.Request) -> bytes:
        r = rmc.Reader(req.params)
        if req.method == 15:  # list<criteria> first (each a structure: skipped by its length)
            for _ in range(r.u32()):
                _, n = r.struct_header()
                r.take(n)
        if req.method in (3, 15):  # Data<Gathering>
            name = r.string()
            r.u32()
            inner = rmc.Reader(r.take(r.u32()))
            src = matchmaking.read_session(inner)
            s = self.matched(conn, src, f"AutoMatchmake ({name})")
            data = matchmaking.write_session(s)
            return rmc.Writer().string("MatchmakeSession").u32(len(data) + 4).u32(len(data)).raw(data).bytes()
        r.struct_header()  # AutoMatchmakeParam
        src = matchmaking.read_session(r)
        return matchmaking.write_session(self.matched(conn, src, "AutoMatchmakeWithParam"))

    def create_session(self, conn: Connection, req: rmc.Request) -> bytes:
        r = rmc.Reader(req.params)
        if req.method == 6:
            r.string()
            r.u32()
            src = matchmaking.read_session(rmc.Reader(r.take(r.u32())))
        else:
            r.struct_header()
            src = matchmaking.read_session(r)
        s = self.mm.new(src, conn.pid or 0)
        self.log(f"{conn.name}   CreateMatchmakeSession: session {s.gid} (game mode {s.game_mode}, attributes {s.attribs})")
        if req.method == 6:
            return struct.pack("<I", s.gid) + kerberos.buffer(s.session_key)
        return matchmaking.write_session(s)

    def join_session(self, conn: Connection, req: rmc.Request) -> bytes:
        r = rmc.Reader(req.params)
        if req.method == 39:
            r.struct_header()
        gid = r.u32()
        s = self.mm.sessions.get(gid)
        if not s:
            raise RmcError(0x8006000B)  # (RendezVous::SessionVoid [inf])
        self.mm.join(s, conn.pid or 0)
        self.notify(s.owner, conn.pid or 0, 3001, s.gid, conn.pid or 0)
        self.log(f"{conn.name}   JoinMatchmakeSession {gid}: {s.participants}")
        return kerberos.buffer(s.session_key) if req.method == 7 else matchmaking.write_session(s)

    def session_urls(self, conn: Connection, req: rmc.Request) -> bytes:
        gid = struct.unpack_from("<I", req.params)[0]
        s = self.mm.sessions.get(gid)
        urls = self.urls.get(s.host, []) if s else []
        self.log(f"{conn.name}   GetSessionURLs({gid}): host {s.host if s else None}: {urls}")
        w = rmc.Writer().u32(len(urls))
        for u in urls:
            w.string(u)
        return w.bytes()

    def end_participation(self, conn: Connection, req: rmc.Request) -> bytes:
        gid = struct.unpack_from("<I", req.params)[0]
        s = self.mm.sessions.get(gid)
        if s and (conn.pid or 0) in s.participants:
            s.participants.remove(conn.pid or 0)
            s.participation = len(s.participants)
            for p in s.participants:
                self.notify(p, conn.pid or 0, 3008, gid, conn.pid or 0)
            if not s.participants:
                del self.mm.sessions[gid]
        self.log(f"{conn.name}   EndParticipation({gid})")
        return b"\1"

    # -- utility
    def integer_settings(self, conn: Connection, req: rmc.Request) -> bytes:
        # [obs] index 0 is SMM2's application config: map<u16, s32>, keys 0..37 land in the game's config array
        # (the request function main+0x14ce2f0); 0 = "This service is not active right now" (keys 0..3: the Endless
        # Challenge's difficulties; which key gates Network Play: [?]). Every key 1 here (--config k=v overrides).
        index = struct.unpack_from("<I", req.params)[0]
        values = dict(self.config) if index == 0 else {}
        self.log(f"{conn.name}   GetIntegerSettings({index}): {len(values)} values")
        w = rmc.Writer().u32(len(values))
        for k in sorted(values):
            w.raw(struct.pack("<Hi", k, values[k]))
        return w.bytes()

    def string_settings(self, conn: Connection, req: rmc.Request) -> bytes:
        self.log(f"{conn.name}   GetStringSettings({struct.unpack_from('<I', req.params)[0]}): none (empty map)")
        return struct.pack("<I", 0)


    # -- DataStore (SMM2) [public: kinnay's NintendoClients, datastore_smm2.py; the wiki's "Data Store Protocol (SMM 2)"]
    def empty(self, conn: Connection, req: rmc.Request) -> bytes:
        return b""

    def empty_list(self, conn: Connection, req: rmc.Request) -> bytes:
        return struct.pack("<I", 0)

    def user_name_ng_type(self, conn: Connection, req: rmc.Request) -> bytes:
        return b"\0"  # the name is fine

    def new_notification(self, conn: Connection, req: rmc.Request) -> bytes:
        # [obs] GetNewNotification answers a structure of three bools (its extractor, main+0x5ef890, reads the
        # version / length header, then three bools, the request function main+0x14edee0 keeps them); all false:
        # nothing new. (An error here leads the game's network worker to "Pure virtual function called" later.)
        return structure(b"\0\0\0")

    def event_course_status(self, conn: Connection, req: rmc.Request) -> bytes:
        return structure(struct.pack("<Q", 0) + b"\0" + struct.pack("<Q", 0))  # EventCourseStatusInfo: no event

    def user(self, pid: int) -> dict:
        return self.users.get(pid) or {"name": self.default_name, "mii": mii.default_charinfo(self.default_name),
                                       "country": "US", "region": 1}

    @staticmethod
    def maker_code(pid: int) -> str:
        chars = "0123456789BCDFGHJKLMNPQRSTVWXY"  # (the characters of SMM2's maker and course IDs)
        return "".join(chars[(pid >> (5 * i)) % 30] for i in range(9))

    def user_info(self, pid: int) -> bytes:
        """UserInfo, revision 3 [public: kinnay's wiki, "Data Store Protocol (SMM 2)"]"""
        u = self.user(pid)
        w = rmc.Writer().u64(pid).string(self.maker_code(pid)).string(u["name"])
        w.raw(structure(struct.pack("<4H", 0, 0, 0, 0)))                 # UnknownStruct1
        w.raw(struct.pack("<H", len(u["mii"])) + u["mii"])                 # the Mii (CharInfo)
        w.string(u["country"]).u8(u["region"]).u64(datetime_now())        # country, region, last active
        w.raw(b"\0\0\0")                                                   # three flags
        empty_map = struct.pack("<I", 0)
        w.raw(empty_map * 5)                                               # play / maker / endless / multi / 0x400
        w.raw(struct.pack("<I", 0))                                        # badges
        w.raw(empty_map * 2)
        w.raw(b"\0").u64(datetime_now()).raw(b"\0")                         # revision 1
        w.raw(structure(b"\0" + struct.pack("<Q", 0)))                     # revision 2: UnknownStruct3
        w.string("").raw(empty_map).raw(b"\0")                             # revision 3
        return structure(w.bytes(), version=3)

    def get_users(self, conn: Connection, req: rmc.Request) -> bytes:
        r = rmc.Reader(req.params)
        r.struct_header()  # GetUsersParam {list<PID> pids, u32 option}
        pids = [r.u64() for _ in range(r.u32())]
        option = r.u32()
        self.log(f"{conn.name}   GetUsers({pids}, option {option:#x}): " +
                 ", ".join(f"{p}: {self.user(p)['name']} ({self.maker_code(p)}, default Mii)" for p in pids))
        return (struct.pack("<I", len(pids)) + b"".join(self.user_info(p) for p in pids)
                + struct.pack("<I", len(pids)) + struct.pack("<I", RESULT_SUCCESS) * len(pids))

    def read_profile(self, r: rmc.Reader) -> tuple[str, bytes, int, str]:
        name = r.string()
        r.struct_header()
        r.take(8)                      # UnknownStruct1
        m = r.take(r.u16())            # the Mii
        region = r.u8()
        country = r.string()
        return name, m, region, country

    def register_user(self, conn: Connection, req: rmc.Request) -> bytes:
        r = rmc.Reader(req.params)
        r.struct_header()
        name, m, region, country = self.read_profile(r)
        self.users[conn.pid or 0] = {"name": name, "mii": m if len(m) >= 0x58 else mii.default_charinfo(name),
                                     "country": country or "US", "region": region}
        self.log(f"{conn.name}   RegisterUser: {name!r}, Mii {len(m)} bytes, region {region}, country {country!r}")
        return b""

    def sync_user_profile(self, conn: Connection, req: rmc.Request) -> bytes:
        r = rmc.Reader(req.params)
        r.struct_header()
        name, m, region, country = self.read_profile(r)
        flags = r.take(2)
        self.log(f"{conn.name}   SyncUserProfile: {name!r}, Mii {len(m)} bytes, region {region}, country {country!r}")
        pid = conn.pid or 0
        u = self.user(pid)
        if name:
            u["name"] = name
        if len(m) >= 0x58:
            u["mii"] = m
        self.users[pid] = u
        res = (rmc.Writer().u64(pid).string(u["name"]).bytes() + structure(struct.pack("<4H", 0, 0, 0, 0))
               + struct.pack("<H", len(u["mii"])) + u["mii"] + bytes([region]) + rmc.Writer().string(country).bytes()
               + b"\0" + flags)
        return structure(res)


class Connection:
    """One PRUDP connection: (client port, server port) on one WebSocket."""

    def __init__(self, server: Server, ws_id: int, client_port: int, server_port: int,
                 send: Callable[[bytes], None] | None = None) -> None:
        self.server, self.ws_id, self.send = server, ws_id, send
        self.call_id = 0x10000  # (requests the server sends)
        self.client_port, self.server_port = client_port, server_port
        self.name = f"conn {ws_id}:{client_port}>{server_port}"
        self.seq = 1
        self.fragments = b""
        self.seen: set[int] = set()  # reliable DATA sequence ids already handled (a resend is only acknowledged)
        self.pid: int | None = None
        self.session_key: bytes | None = None

    def packet(self, p: prudp.Packet, type_: int, flags: int, options=None, payload=b"", seq=None, frag=0) -> bytes:
        out = prudp.Packet(type=type_, flags=flags, src_type=p.dst_type, dst_type=p.src_type, src_port=p.dst_port,
                           dst_port=p.src_port, seq=p.seq if seq is None else seq, fragment=frag,
                           options=options or [], payload=payload)
        self.server.log(f"{self.name} S>C {out.describe()}")
        return prudp.encode(out)

    def handle(self, p: prudp.Packet) -> list[bytes]:
        log = self.server.log
        ack = p.flags & prudp.FLAG_ACK
        support = p.option(prudp.OPT_SUPPORT)
        echo = [(prudp.OPT_SUPPORT, support)] if support is not None else []
        if p.type == prudp.TYPE_SYN and not ack:
            opts = echo + [(prudp.OPT_CONNECTION_SIG, os.urandom(16))]
            if p.option(prudp.OPT_MAX_SUBSTREAM) is not None:
                opts.append((prudp.OPT_MAX_SUBSTREAM, p.option(prudp.OPT_MAX_SUBSTREAM)))
            return [self.packet(p, prudp.TYPE_SYN, prudp.FLAG_ACK | prudp.FLAG_HAS_SIZE, opts)]
        if p.type == prudp.TYPE_CONNECT and not ack:
            payload = b""
            if p.payload:
                try:
                    payload = self.server.secure_connect(self, p.payload)
                except ValueError as e:
                    log(f"{self.name} secure CONNECT refused: {e}")
                    return []
            return [self.packet(p, prudp.TYPE_CONNECT, prudp.FLAG_ACK | prudp.FLAG_HAS_SIZE, list(echo), payload)]
        if p.type == prudp.TYPE_PING and not ack:
            return [self.packet(p, prudp.TYPE_PING, prudp.FLAG_ACK | prudp.FLAG_HAS_SIZE)]
        if p.type == prudp.TYPE_DISCONNECT and not ack:
            return [self.packet(p, prudp.TYPE_DISCONNECT, prudp.FLAG_ACK | prudp.FLAG_HAS_SIZE)] * 3
        if p.type == prudp.TYPE_DATA and not ack and not p.flags & prudp.FLAG_MULTI_ACK:
            out = []
            if p.flags & prudp.FLAG_NEED_ACK:
                out.append(self.packet(p, prudp.TYPE_DATA, prudp.FLAG_ACK | prudp.FLAG_HAS_SIZE))
            if p.flags & prudp.FLAG_RELIABLE:
                if p.seq in self.seen:
                    log(f"{self.name} (sequence id {p.seq} again: a resend, acknowledged only)")
                    return out
                self.seen.add(p.seq)
            self.fragments += p.payload
            if p.fragment == 0:  # (the last fragment of a message)
                msg, self.fragments = self.fragments, b""
                out += self.request(p, msg)
            return out
        return []

    def push(self, protocol: int, method: int, params: bytes, what: str) -> None:
        """Send the client a request (a notification, NAT traversal's InitiateProbe ...)"""
        if not self.send:
            return
        self.call_id += 1
        body = rmc.request(protocol, self.call_id, method, params)
        chunks = [body[i:i + FRAGMENT] for i in range(0, len(body), FRAGMENT)]
        self.server.log(f"{self.name} S>C request {what} (call {self.call_id}, {len(params)} bytes)")
        for n, chunk in enumerate(chunks):
            seq, self.seq = self.seq, self.seq + 1
            frag = 0 if n == len(chunks) - 1 else n + 1
            p = prudp.Packet(type=prudp.TYPE_DATA, flags=prudp.FLAG_RELIABLE | prudp.FLAG_NEED_ACK | prudp.FLAG_HAS_SIZE,
                             src_type=10, dst_type=10, src_port=self.server_port, dst_port=self.client_port, seq=seq,
                             fragment=frag, payload=chunk)
            self.send(prudp.encode(p))

    def request(self, p: prudp.Packet, msg: bytes) -> list[bytes]:
        log = self.server.log
        req = rmc.parse_request(msg)
        if not req:
            r = rmc.parse_response(msg)
            if r:
                log(f"{self.name} the client answers call {r[2]}: {'ok' if r[1] else f'error {r[3]:#010x}'}")
            else:
                log(f"{self.name} not an RMC message: {msg.hex()}")
            return []
        log(f"{self.name} RMC request {req.name} (call {req.call_id}, {len(req.params)} bytes)")
        handler = self.server.handlers.get((req.protocol, req.method))
        if req.protocol not in (10,):
            log(f"{self.name}   parameters: {req.params[:400].hex()}{' ...' if len(req.params) > 400 else ''}")
        try:
            if not handler:
                raise RmcError(CORE_NOT_IMPLEMENTED)
            body = rmc.response(req.protocol, req.call_id, req.method, handler(self, req))
            log(f"{self.name}   answered ({len(body)} bytes)")
        except RmcError as e:
            body = rmc.error(req.protocol, req.call_id, e.code)
            log(f"{self.name}   answered with error {e.code:#010x}{' (not implemented)' if e.code == CORE_NOT_IMPLEMENTED else ''}")
        except (ValueError, IndexError, struct.error) as e:
            body = rmc.error(req.protocol, req.call_id, CORE_NOT_IMPLEMENTED)
            log(f"{self.name}   could not answer ({e}): error")
        chunks = [body[i:i + FRAGMENT] for i in range(0, len(body), FRAGMENT)] or [b""]
        out = []
        for n, chunk in enumerate(chunks):
            seq, self.seq = self.seq, self.seq + 1
            frag = 0 if n == len(chunks) - 1 else n + 1
            out.append(self.packet(p, prudp.TYPE_DATA, prudp.FLAG_RELIABLE | prudp.FLAG_NEED_ACK | prudp.FLAG_HAS_SIZE,
                                   payload=chunk, seq=seq, frag=frag))
        return out


class Session:
    """Every PRUDP connection carried by one WebSocket."""

    def __init__(self, server: Server, ws_id: int, send: Callable[[bytes], None] | None = None) -> None:
        self.server, self.ws_id, self.send = server, ws_id, send
        self.conns: dict[tuple[int, int], Connection] = {}

    def feed(self, data: bytes) -> list[bytes]:
        out: list[bytes] = []
        packets, rest = prudp.decode(data)
        if rest:
            self.server.log(f"conn {self.ws_id} {len(rest)} bytes after the packets: {rest.hex()}")
        for p in packets:
            key = (p.src_port, p.dst_port)
            conn = self.conns.get(key)
            if conn is None or (p.type == prudp.TYPE_SYN and not p.flags & prudp.FLAG_ACK):
                conn = self.conns[key] = Connection(self.server, self.ws_id, p.src_port, p.dst_port, self.send)
            self.server.log(f"{conn.name} C>S {p.describe()}")
            out += conn.handle(p)
        return out
