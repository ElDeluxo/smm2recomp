"""Matchmaking structures and sessions (public: kinnay's wiki, "Match Making Types", "Matchmake Extension Protocol").

A MatchmakeSession is a Gathering (its own structure header) followed by the session's fields (another header).
NEX 4.x layout:
  Gathering: u32 id, PID owner, PID host, u16 min, u16 max, u32 participation policy, u32 policy argument,
             u32 flags, u32 state, string description
  MatchmakeSession: u32 game mode, list<u32> attributes, bool open participation, u32 matchmake system type,
             buffer application buffer, u32 participation count, u8 progress score, buffer session key, u32 option0,
             MatchmakeParam (structure: map<string, variant>), DateTime started, string user password,
             u32 refer gid, bool user password enabled, bool system password enabled, string codeword
"""

from __future__ import annotations

import os
import struct
from dataclasses import dataclass, field

from .rmc import Reader, Writer


@dataclass
class Session:
    gid: int = 0
    owner: int = 0
    host: int = 0
    min_part: int = 1
    max_part: int = 4
    policy: int = 0
    policy_arg: int = 0
    flags: int = 0
    state: int = 0
    description: str = ""
    game_mode: int = 0
    attribs: list[int] = field(default_factory=list)
    open: bool = True
    system_type: int = 0
    app_buffer: bytes = b""
    participation: int = 0
    progress: int = 0
    session_key: bytes = b""
    option0: int = 0
    param: bytes = b""      # (MatchmakeParam, kept as it came: a structure)
    started: int = 0
    user_password: str = ""
    refer_gid: int = 0
    user_pw: bool = False
    system_pw: bool = False
    codeword: str = ""
    gversion: int = 0
    mversion: int = 0
    tail: bytes = b""       # (fields after the known ones, kept)
    participants: list[int] = field(default_factory=list)


def read_session(r: Reader) -> Session:
    s = Session()
    s.gversion, glen = r.struct_header()
    end = r.pos + glen
    s.gid, s.owner, s.host = r.u32(), r.u64(), r.u64()
    s.min_part, s.max_part = r.u16(), r.u16()
    s.policy, s.policy_arg, s.flags, s.state = r.u32(), r.u32(), r.u32(), r.u32()
    s.description = r.string()
    r.pos = end
    s.mversion, mlen = r.struct_header()
    end = r.pos + mlen
    s.game_mode = r.u32()
    s.attribs = [r.u32() for _ in range(r.u32())]
    s.open, s.system_type = bool(r.u8()), r.u32()
    s.app_buffer = r.buffer()
    s.participation, s.progress = r.u32(), r.u8()
    s.session_key = r.buffer()
    s.option0 = r.u32()
    pv, pl = r.struct_header()
    s.param = struct.pack("<BI", pv, pl) + r.take(pl)
    s.started = r.u64()
    s.user_password = r.string()
    s.refer_gid, s.user_pw, s.system_pw = r.u32(), bool(r.u8()), bool(r.u8())
    s.codeword = r.string()
    s.tail = r.take(end - r.pos) if end > r.pos else b""
    r.pos = end
    return s


def write_session(s: Session) -> bytes:
    g = (Writer().u32(s.gid).u64(s.owner).u64(s.host).raw(struct.pack("<HH", s.min_part, s.max_part))
         .u32(s.policy).u32(s.policy_arg).u32(s.flags).u32(s.state).string(s.description).bytes())
    m = Writer().u32(s.game_mode).u32(len(s.attribs))
    for a in s.attribs:
        m.u32(a)
    m.u8(int(s.open)).u32(s.system_type).buffer(s.app_buffer).u32(s.participation).u8(s.progress)
    m.buffer(s.session_key).u32(s.option0).raw(s.param or struct.pack("<BII", 0, 4, 0)).u64(s.started)
    m.string(s.user_password).u32(s.refer_gid).u8(int(s.user_pw)).u8(int(s.system_pw)).string(s.codeword).raw(s.tail)
    mb = m.bytes()
    return struct.pack("<BI", s.gversion, len(g)) + g + struct.pack("<BI", s.mversion, len(mb)) + mb


class Matchmaker:
    """Every session this server knows, and who is in which."""

    def __init__(self) -> None:
        self.sessions: dict[int, Session] = {}
        self.next_gid = 0x1000

    def new(self, src: Session, owner: int) -> Session:
        self.next_gid += 1
        s = Session(**{k: getattr(src, k) for k in src.__dataclass_fields__})
        s.gid, s.owner, s.host = self.next_gid, owner, owner
        s.session_key = os.urandom(32)
        s.participants = [owner]
        s.participation = 1
        self.sessions[s.gid] = s
        return s

    def join(self, s: Session, pid: int) -> None:
        if pid not in s.participants:
            s.participants.append(pid)
        s.participation = len(s.participants)

    def find_open(self, src: Session, pid: int) -> Session | None:
        """an open session of the same game mode with room (auto-matchmake)"""
        for s in self.sessions.values():
            if (s.open and s.game_mode == src.game_mode and len(s.participants) < s.max_part
                    and pid not in s.participants and s.attribs[:1] == src.attribs[:1]):
                return s
        return None

    def leave(self, pid: int) -> list[Session]:
        out = []
        for s in list(self.sessions.values()):
            if pid in s.participants:
                s.participants.remove(pid)
                s.participation = len(s.participants)
                out.append(s)
                if not s.participants:
                    del self.sessions[s.gid]
                elif s.owner == pid:
                    s.owner = s.host = s.participants[0]
        return out
