"""A default Mii as nn::mii::CharInfo (0x58 bytes): the same values as the recomp's runtime/services/mii.c
(mii_default_charinfo). SMM2 takes a user's Mii from its DataStore UserInfo as this structure [obs: the GetUsers
converter, main+0x14d0a60, copies the buffer's first 0x58 bytes into the user entry's Mii]."""

from __future__ import annotations

FACE = bytes([
    0, 0, 0, 64, 64, 0,         # font region, favourite colour, gender, height, build, type
    0,                          # region move
    0, 0, 0, 0,                 # faceline type, colour, wrinkle, make
    33, 1, 0,                   # hair type, colour, flip
    2, 8, 4, 3, 4, 2, 12,       # eye type, colour, scale, aspect, rotate, x, y
    6, 1, 4, 3, 6, 2, 10,       # eyebrow type, colour, scale, aspect, rotate, x, y
    1, 4, 9,                    # nose type, scale, y
    23, 19, 4, 3, 13,           # mouth type, colour, scale, aspect, y
    8, 0, 0, 4, 10,             # beard colour, beard type, mustache type, scale, y
    0, 8, 4, 10,                # glass type, colour, scale, y
    0, 4, 2, 20,                # mole type, scale, x, y
    0,                          # reserved
])
CREATE_ID = bytes([0x52, 0x65, 0x63, 0x6F, 0x6D, 0x70, 0x4D, 0x69, 0x80, 1, 2, 3, 4, 5, 6, 7])


def default_charinfo(nickname: str = "Player") -> bytes:
    name = (nickname or "Player")[:10].encode("utf-16-le")
    out = CREATE_ID + name + bytes(0x26 - 16 - len(name)) + FACE
    assert len(out) == 0x58
    return out
