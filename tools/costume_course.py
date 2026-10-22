"""Test course for the Mystery Mushroom costumes mod (recomp/docs/mods.md, "Mystery Mushroom costumes").

Builds our own small SMB1-style course and installs it as all ten title courses of a mod folder, so the game plays it
on its title screen straight away (RECOMP_MOD_DIR), with the scripted input this file also writes. As
tools/shell_jump_course.py: nothing of the game goes into the course; only the title-course archives' wrapping is copied
from the player's own RomFS, and the course is encrypted with the key table found in the player's own executable. The
output is private (never commit it).

    python -I tools/costume_course.py --romfs recomp/private/smm2/extracted/romfs \\
        --elf <main.elf> | --table <course_table.bin>  --out <mod folder>

The course (tiles of 16 px, y up; the ground 5 tiles high, walls 2 tiles above it, pens between them):
  - pen 1 (x 7-12): a Super Mushroom, bouncing between the start and the wall at x = 13;
  - pen 2 (x 14-21): a Big Mushroom (SMB1: the Mystery Mushroom; 2 x 2);
  - pen 3 (x 23-30): another Big Mushroom;
  - pen 4 (x 32-39): a Koopa.
The input (<out>/costume.pad, RECOMP_PAD_SCRIPT, one line per player update): wait for the Super Mushroom, jump into
pen 2 and walk into the Mystery Mushroom as Super Mario (a costume, through the power-down), stand and hold Up (the
taunt: the appeal pose and sound), jump into pen 3 and take the second (another costume: SMM1 draws one at each
pickup), then jump into pen 4 and stand, where the Koopa's hit takes the costume off. RECOMP_COSTUME_TRY presses the
overlay menu's "Mystery Mushroom costume now" at a presented frame (e.g. while standing in pen 3).
--variant subarea: a warp pipe facing left at tiles 12-13 of the main area and the same of the sub-area (warp id 1), for
a player in costume (RECOMP_COSTUME) who walks right into it, comes out in the sub-area, and walks back in.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import oead  # noqa: E402

from smmfmt.smm2 import course, crypto  # noqa: E402
from smmfmt.smm2.keyfind import find_table  # noqa: E402

COLUMNS = 235
GROUND = 5
PAD = {
    "pickups": "# Mystery Mushroom costumes (tools/costume_course.py): one line per player update\n"
               "200 -\n"
               "100 RIGHT\n16 RIGHT A\n40 RIGHT\n120 -\n"
               "160 UP\n40 -\n"
               "100 RIGHT\n16 RIGHT A\n60 RIGHT\n200 -\n"
               "100 RIGHT\n16 RIGHT A\n20 RIGHT\n300 -\n",
    "hit": "# Mystery Mushroom costumes, the hit (tools/costume_course.py --variant hit): one line per player update\n"
           "20 -\n160 UP\n400 -\n",
    "subarea": "# Mystery Mushroom costumes, a pipe to the sub-area and back (tools/costume_course.py --variant subarea)\n"
               "200 -\n240 RIGHT\n200 -\n60 LEFT\n240 RIGHT\n200 -\n",
}


def build(variant: str) -> bytes:
    c = course.Course.blank("M1")
    h = c.header
    h.timer, h.game_version, h.management_flags, h.clear_check_time = 500, 1, 1, 0xFFFFFFFF
    h.year, h.month, h.day, h.hour, h.minute = 2026, 10, 22, 12, 0
    h.name = "Costume test"
    for area in c.areas:
        area.header.boundary_right, area.header.boundary_top = COLUMNS * 16, 432
    a = c.main
    for area in c.areas:
        area.header.liquid_start_height = area.header.liquid_end_height = 1
    h.start_y, h.goal_y, h.goal_x = GROUND, GROUND, (COLUMNS - 10) * 10 + 5

    def ground(area, cols: dict[int, int]) -> None:
        g = {(x, y) for x, height in cols.items() for y in range(height)}

        def tile_id(x: int, y: int) -> int:  # the SMB1 ground ids (top / sides / fill), from the neighbours
            up, left, right = (x, y + 1) in g, (x - 1, y) in g, (x + 1, y) in g
            if not up:
                return 59 if left and right else 58 if right else 60 if left else 29
            return 62 if left and right else 61 if right else 63 if left else 29

        for x, y in sorted(g):
            t = course.GroundTile()
            t.x, t.y, t.id, t.background_id = x, y, tile_id(x, y), 0
            area.ground.items.append(t)

    def put(obj_id: int, x: int, size: int = 1, area=a, flags: int = 0x06000040) -> course.CourseObject:
        o = course.CourseObject()  # one object standing on the ground from tile x
        o.x, o.y = x * 160 + 80 * size, GROUND * 160 + 80 * size
        o.width = o.height = size
        o.flags, o.child_flags, o.extended_data = flags, 0x06000040, 0
        o.id, o.child_id, o.link_id, o.sound_effect_id = obj_id, -1, -1, -1
        area.objects.items.append(o)
        return o

    if variant == "subarea":
        # a pipe facing left (direction 1, flags bits 5-6) with warp id 1 (bits 20-23) on the ground at tiles 12-13 of
        # each area (its anchor is the right-hand tile's centre, its length 2): walking right into one leads to the
        # other area's, out of whose mouth the player comes walking left
        ground(a, {x: GROUND for x in range(7, COLUMNS - 10)})
        ground(c.sub, {x: GROUND for x in range(0, 40)})
        for area in c.areas:
            pipe = put(9, 13, area=area, flags=0x06000000 | 1 << 5 | 1 << 20)
            pipe.width = pipe.height = 2  # (height: its length)
        return c.to_bytes()

    cols = {x: GROUND for x in range(7, COLUMNS - 10)}
    for x in (13, 22, 31, 40):
        cols[x] = GROUND + 2
    ground(a, cols)

    if variant == "hit":
        put(1, 11)  # a Koopa, walking to the start
        return c.to_bytes()
    put(20, 10)     # a Super Mushroom
    put(44, 17, 2)  # the Big Mushroom (SMB1's style power-up: the Mystery Mushroom; 2 x 2)
    put(44, 26, 2)
    put(1, 36)      # a Koopa
    return c.to_bytes()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--romfs", type=Path, required=True, help="the game's RomFS (3.0.3), for the title-course archives")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--elf", type=Path, help="the game's main executable as an ELF (the key table is found in it)")
    src.add_argument("--table", type=Path, help="the course key table (smmfmt.smm2.keyfind output)")
    ap.add_argument("--out", type=Path, required=True, help="the mod folder to write (RECOMP_MOD_DIR)")
    ap.add_argument("--variant", choices=sorted(PAD), default="pickups",
                    help="pickups (the course above); hit: pen 1 holds a Koopa walking to the start, for a player in "
                         "costume from the start (RECOMP_COSTUME) who holds Up (the taunt), then waits for its hit; "
                         "subarea: a pipe to the sub-area and back, for a player in costume")
    args = ap.parse_args()

    names = ["title_course_data_%02d" % n for n in range(10)]
    archives = [oead.Sarc(oead.yaz0.decompress((args.romfs / "Course" / (n + ".szs")).read_bytes())) for n in names]
    if args.table:
        table = crypto.table_from_bytes(args.table.read_bytes())
    else:
        bcd = bytes(next(iter(archives[0].get_files())).data)
        table = crypto.table_from_bytes(find_table(args.elf.read_bytes(), bcd)[1])
    body = build(args.variant)
    out = args.out / "romfs" / "Course"
    out.mkdir(parents=True, exist_ok=True)
    for name, sarc in zip(names, archives):
        w = oead.SarcWriter.from_sarc(sarc)
        w.files[name + ".bcd"] = crypto.encrypt_bcd(body, table, iv=bytes(16), random_state=(1, 2, 3, 4))
        (out / (name + ".szs")).write_bytes(oead.yaz0.compress(w.write()[1]))
    (args.out / "costume.pad").write_text(PAD[args.variant])
    print(f"{args.out}: the test course as the ten title courses, and costume.pad")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
