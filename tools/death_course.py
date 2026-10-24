"""A test course where Mario dies within seconds, again and again (100 Mario's miss / lives card / return loop).

Builds our own SMB1-style course - ground under the start only, then a pit - as a raw (decrypted) course file, which
a test server takes with `mmk upload`. Nothing of the game goes into it. Walking right (RECOMP_MOD_AUTOPLAY=1) falls
into the pit about two seconds after each start, so a 100 Mario run that plays it (RECOMP_MOD_TEST_BREAK=1:<code>)
loses a life every few seconds on the same course: the owner's "died on the same level several times" case.

    python -I tools/death_course.py --out <file.bin>
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from smmfmt.smm2 import course  # noqa: E402

COLUMNS = 235
GROUND = 5


def build() -> bytes:
    c = course.Course.blank("M1")
    h = c.header
    h.timer, h.game_version, h.management_flags, h.clear_check_time = 500, 1, 1, 0xFFFFFFFF
    h.year, h.month, h.day, h.hour, h.minute = 2026, 10, 24, 12, 0
    h.name = "Pit test"
    for area in c.areas:
        area.header.boundary_right, area.header.boundary_top = COLUMNS * 16, 432
        area.header.liquid_start_height = area.header.liquid_end_height = 1
    h.start_y, h.goal_y, h.goal_x = GROUND, GROUND, (COLUMNS - 10) * 10 + 5
    # as tools/costume_course.py lays its ground (the blank course's start and goal platforms stay), but only a short
    # run from tile 7: then a pit to the goal
    g = {(x, y) for x in range(7, 12) for y in range(GROUND)}

    def tile_id(x: int, y: int) -> int:  # the SMB1 ground ids (top / sides / fill), from the neighbours
        up, left, right = (x, y + 1) in g, (x - 1, y) in g, (x + 1, y) in g
        if not up:
            return 59 if left and right else 58 if right else 60 if left else 29
        return 62 if left and right else 61 if right else 63 if left else 29

    for x, y in sorted(g):
        t = course.GroundTile()
        t.x, t.y, t.id, t.background_id = x, y, tile_id(x, y), 0
        c.main.ground.items.append(t)
    return c.to_bytes()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", type=Path, required=True, help="the raw course file to write")
    a = ap.parse_args()
    a.out.write_bytes(build())
    print(f"{a.out}: the pit test course ({a.out.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
