"""RMC (NEX remote method calls) and the NEX types the login uses (public: kinnay's NintendoClients wiki, "RMC
Protocol", "NEX Common Types", "Authentication Protocol").

Request, little-endian:  u32 size, u8 protocol | 0x80, [u16 protocol if 0x7f], u32 call id, u32 method id, params
Response:                u32 size, u8 protocol, u8 1, u32 call id, u32 method id | 0x8000, data
Error response:          u32 size, u8 protocol, u8 0, u32 error code, u32 call id
"""

from __future__ import annotations

import base64
import json
import struct
from dataclasses import dataclass

# protocol ids [public: kinnay's NintendoClients, each protocol's PROTOCOL_ID]
PROTOCOLS = {1: "RemoteLog", 3: "NATTraversal", 10: "Authentication", 11: "SecureConnection", 14: "Notification",
             0x12: "Health", 0x13: "Monitoring", 0x15: "MatchMaking", 0x17: "Messaging", 0x19: "AccountManagement",
             0x64: "NintendoNotification", 0x65: "Friends", 0x6D: "MatchmakeExtension", 0x6E: "Utility",
             0x70: "Ranking", 0x73: "DataStore", 0x74: "Debug", 0x79: "Subscriber", 0x7A: "Ranking2",
             0x7B: "AAUser", 0x7C: "Screening"}
UTILITY_METHODS = {1: "AcquireNexUniqueId", 2: "AcquireNexUniqueIdWithPassword", 3: "AssociateNexUniqueIdWithMyPrincipalId",
                   4: "AssociateNexUniqueIdsWithMyPrincipalId", 5: "GetAssociatedNexUniqueIdWithMyPrincipalId",
                   6: "GetAssociatedNexUniqueIdsWithMyPrincipalId", 7: "GetIntegerSettings", 8: "GetStringSettings"}
# SMM2's DataStore methods [public: kinnay's NintendoClients, datastore_smm2.py]
DATASTORE_SMM2_METHODS = {8: "GetMeta", 9: "GetMetas", 25: "PrepareGetObject", 47: "RegisterUser", 48: "GetUsers",
                          49: "SyncUserProfile", 50: "SearchUsersUserPoint", 59: "UpdateLastLoginTime",
                          63: "GetMiiClothes", 64: "PostActivityQuest", 65: "GetUsernameNgType", 70: "GetCourses", 71: "SearchCoursesPointRanking",
                          73: "SearchCoursesLatest", 74: "SearchCoursesPostedBy", 79: "SearchCoursesEndlessMode",
                          125: "GetNewNotification", 131: "GetUserOrCourse", 134: "GetReqGetInfoHeadersInfo",
                          151: "LoginCheck", 152: "UpdateLastLoginInfo", 154: "GetEventCourseStatus"}
MATCHMAKE_EXT_METHODS = {1: "CloseParticipation", 2: "OpenParticipation", 3: "AutoMatchmake_Postpone",
                         4: "BrowseMatchmakeSession", 6: "CreateMatchmakeSession", 7: "JoinMatchmakeSession",
                         11: "UpdateApplicationBuffer", 12: "UpdateMatchmakeSessionAttribute",
                         14: "UpdateMatchmakeSession", 15: "AutoMatchmakeWithSearchCriteria_Postpone",
                         16: "GetPlayingSession", 34: "UpdateProgressScore", 38: "CreateMatchmakeSessionWithParam",
                         39: "JoinMatchmakeSessionWithParam", 40: "AutoMatchmakeWithParam_Postpone"}
NAT_METHODS = {1: "RequestProbeInitiation", 2: "InitiateProbe", 3: "RequestProbeInitiationExt",
               4: "ReportNATTraversalResult", 5: "ReportNATProperties", 6: "GetRelaySignatureKey",
               7: "ReportNATTraversalResultDetail"}
MATCHMAKING_METHODS = {41: "GetSessionURLs", 42: "UpdateSessionHost", 43: "UpdateGatheringOwnership"}
EXTRA_METHODS: dict[int, dict[int, str]] = {0x6E: UTILITY_METHODS, 0x73: DATASTORE_SMM2_METHODS,
                                            0x6D: MATCHMAKE_EXT_METHODS, 3: NAT_METHODS, 0x15: MATCHMAKING_METHODS,
                                            0x32: {1: "EndParticipation"}, 14: {1: "ProcessNotificationEvent"}}
AUTH_METHODS = {1: "ValidateAndRequestTicket", 2: "ValidateAndRequestTicketWithCustomData", 3: "RequestTicket",
                4: "GetPID", 5: "GetName", 6: "ValidateAndRequestTicketWithParam"}
SECURE_METHODS = {1: "Register", 2: "RequestConnectionData", 3: "RequestUrls", 4: "RegisterEx", 5: "TestConnectivity",
                  6: "UpdateURLs", 7: "ReplaceURL", 8: "SendReport"}


@dataclass
class Request:
    protocol: int
    call_id: int
    method: int
    params: bytes

    @property
    def name(self) -> str:
        methods = AUTH_METHODS if self.protocol == 10 else SECURE_METHODS if self.protocol == 11 else \
            EXTRA_METHODS.get(self.protocol) or {}
        return f"{PROTOCOLS.get(self.protocol, hex(self.protocol))}.{methods.get(self.method, self.method)}"


def parse_request(data: bytes) -> Request | None:
    if len(data) < 13:
        return None
    size = struct.unpack_from("<I", data, 0)[0]
    proto = data[4]
    if not proto & 0x80:
        return None
    proto &= 0x7F
    off = 5
    if proto == 0x7F:
        proto = struct.unpack_from("<H", data, off)[0]
        off += 2
    call_id, method = struct.unpack_from("<II", data, off)
    return Request(proto, call_id, method, data[off + 8:4 + size])


def request(protocol: int, call_id: int, method: int, params: bytes) -> bytes:
    head = (bytes([protocol | 0x80]) if protocol < 0x7F else bytes([0xFF]) + struct.pack("<H", protocol))
    rest = head + struct.pack("<II", call_id, method) + params
    return struct.pack("<I", len(rest)) + rest


def parse_response(data: bytes) -> tuple[int, bool, int, int] | None:
    """(protocol, success, call id, method or error code) of a response, None if not one"""
    if len(data) < 14 or data[4] & 0x80:
        return None
    proto, off = data[4], 5
    if proto == 0x7F:
        proto, off = struct.unpack_from("<H", data, 5)[0], 7
    ok = data[off] == 1
    a, b = struct.unpack_from("<II", data, off + 1)
    return (proto, ok, a, b & 0x7FFF) if ok else (proto, ok, b, a)


def response(protocol: int, call_id: int, method: int, body: bytes) -> bytes:
    head = (bytes([protocol]) if protocol < 0x7F else bytes([0x7F]) + struct.pack("<H", protocol))
    rest = head + b"\x01" + struct.pack("<II", call_id, method | 0x8000) + body
    return struct.pack("<I", len(rest)) + rest


def error(protocol: int, call_id: int, code: int) -> bytes:
    rest = bytes([protocol]) + b"\x00" + struct.pack("<II", code, call_id)
    return struct.pack("<I", len(rest)) + rest


class Reader:
    """NEX types: string = u16 length (with the NUL) + bytes; buffer = u32 length + bytes; qbuffer = u16 + bytes."""

    def __init__(self, data: bytes) -> None:
        self.data, self.pos = data, 0

    def left(self) -> int:
        return len(self.data) - self.pos

    def take(self, n: int) -> bytes:
        if n > self.left():
            raise ValueError(f"wanted {n} bytes, {self.left()} left")
        b = self.data[self.pos:self.pos + n]
        self.pos += n
        return b

    def u8(self) -> int:
        return self.take(1)[0]

    def u16(self) -> int:
        return struct.unpack("<H", self.take(2))[0]

    def u32(self) -> int:
        return struct.unpack("<I", self.take(4))[0]

    def u64(self) -> int:
        return struct.unpack("<Q", self.take(8))[0]

    def string(self) -> str:
        n = self.u16()
        return self.take(n).rstrip(b"\0").decode("utf-8", "replace")

    def buffer(self) -> bytes:
        return self.take(self.u32())

    def struct_header(self) -> tuple[int, int]:
        """A structure's version header (NEX 3.5 and later, as on the Switch): u8 version, u32 content length."""
        return self.u8(), self.u32()


class Writer:
    def __init__(self) -> None:
        self.parts: list[bytes] = []

    def raw(self, b: bytes) -> Writer:
        self.parts.append(b)
        return self

    def u8(self, v: int) -> Writer:
        return self.raw(struct.pack("<B", v))

    def u32(self, v: int) -> Writer:
        return self.raw(struct.pack("<I", v))

    def u64(self, v: int) -> Writer:
        return self.raw(struct.pack("<Q", v))

    def string(self, s: str) -> Writer:
        b = s.encode() + b"\0"
        return self.raw(struct.pack("<H", len(b)) + b)

    def buffer(self, b: bytes) -> Writer:
        return self.raw(struct.pack("<I", len(b)) + b)

    def bytes(self) -> bytes:
        return b"".join(self.parts)


def mask_token(token: str) -> str:
    """A JWT with its signature masked, plus its decoded header and claims (they are not secret)."""
    parts = token.split(".")
    if len(parts) != 3:
        return f"<{len(token)} chars>" if len(token) > 16 else token

    def dec(p: str) -> object:
        try:
            return json.loads(base64.urlsafe_b64decode(p + "=" * (-len(p) % 4)))
        except ValueError:
            return p

    claims = dec(parts[1])
    if isinstance(claims, dict) and isinstance(claims.get("nnex"), str):  # (a NEX login token: a secret)
        claims["nnex"] = f"<{len(claims['nnex'])} chars, masked>"
    return f"JWT header {json.dumps(dec(parts[0]))} claims {json.dumps(claims)} signature <{len(parts[2])} chars, masked>"


def decode_login(req: Request) -> list[str]:
    """What the game's login carries, field by field (best effort: the layout is from public docs, so every field
    that does not parse is shown raw)."""
    out = [f"{req.name} (protocol {req.protocol}, method {req.method}, call {req.call_id}, {len(req.params)} bytes of parameters)"]
    r = Reader(req.params)
    try:
        if req.protocol == 10 and req.method == 6:  # ValidateAndRequestTicketWithParam(ValidateAndRequestTicketParam)
            ver, length = r.struct_header()
            out.append(f"  ValidateAndRequestTicketParam (structure version {ver}, {length} bytes)")
            out.append(f"    platform type: {r.u32()}")
            out.append(f"    user name: {r.string()!r}")
            name = r.string()
            r.u32()
            inner = Reader(r.take(r.u32()))
            out.append(f"    data: AnyDataHolder {name!r} ({inner.left()} bytes)")
            if name == "AuthenticationInfo":
                iv, il = inner.struct_header()  # AuthenticationInfo derives from Data
                out.append(f"      (Data base: structure version {iv}, {il} bytes)")
                dv, dl = inner.struct_header()
                out.append(f"      AuthenticationInfo (structure version {dv}, {dl} bytes)")
                tok = inner.string()
                out.append(f"      token: {mask_token(tok)}")
                out.append(f"      NGS version: {inner.u32()}")
                out.append(f"      token type: {inner.u8()}")
                out.append(f"      server version: {inner.u32()}")
            elif inner.left():
                out.append(f"      raw: {inner.take(inner.left()).hex()}")
            out.append(f"    skip version check: {bool(r.u8())}")
            out.append(f"    NEX version: {r.u32()}")
            out.append(f"    client version: {r.u32()}")
        elif req.protocol == 10 and req.method in (1, 2):  # ValidateAndRequestTicket(WithCustomData)
            out.append(f"  user name: {r.string()!r}")
            if req.method == 2:
                name = r.string()
                r.u32()
                out.append(f"  custom data: {name!r} {r.buffer().hex()}")
        if r.left():
            out.append(f"  (left over: {r.take(r.left()).hex()})")
    except ValueError as e:
        out.append(f"  (stopped: {e}; the rest raw: {req.params[r.pos:].hex()})")
    return out
