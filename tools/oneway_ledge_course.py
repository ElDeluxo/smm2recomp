"""Test course for the SMM1 extra "One-way wall ledge (SMM1)" (recomp/docs/mods.md, "SMM1 extras").

Builds our own small SMB1-style course and installs it as all ten title courses of a mod folder, so the game plays
it on its title screen straight away (RECOMP_MOD_DIR), with the scripted input this file also writes. Nothing of the
game goes into the course: it is made from scratch here; only the title-course archives' wrapping is copied from the
player's own RomFS, and the course is encrypted with the key table found in the player's own executable. The output is
private (never commit it).

    python -I tools/oneway_ledge_course.py --romfs recomp/private/smm2/extracted/romfs \\
        --elf <main.elf> | --table <course_table.bin>  --out <mod folder>

The course (tiles of 16 px, y up): flat ground 5 tiles high, a one-way wall (2 x 2, facing right: it lets Mario through
going right and stops him coming back, d = 0) centred at x = 240 px on the ground. The input (<out>/oneway_ledge.pad,
RECOMP_PAD_SCRIPT, one line per player update): walk right through the wall and on, then jump back left into it. With
the setting on, SMM1's ledge (8 px wide, the wall's height) is there on the wall's right: Mario lands on top of it, 2
tiles up; off, the wall is only its one-way line and he drops back to the ground.
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import oead  # noqa: E402

from smmfmt.smm2 import course, crypto  # noqa: E402
from smmfmt.smm2.keyfind import find_table  # noqa: E402

COLUMNS = 120
GROUND = 5
PAD = "# one-way wall ledge (tools/oneway_ledge_course.py): one line per player update\n" \
      "60 -\n170 RIGHT\n12 -\n20 LEFT A\n60 LEFT\n120 -\n"


def build() -> bytes:
    c = course.Course.blank("M1")
    h = c.header
    h.timer, h.game_version, h.management_flags, h.clear_check_time = 500, 1, 1, 0xFFFFFFFF
    h.year, h.month, h.day, h.hour, h.minute = 2026, 10, 31, 12, 0
    h.name = "One-way ledge test"
    for area in c.areas:
        area.header.boundary_right, area.header.boundary_top = COLUMNS * 16, 432
    a = c.main
    a.header.liquid_start_height = a.header.liquid_end_height = 1
    h.start_y, h.goal_y, h.goal_x = GROUND, GROUND, (COLUMNS - 10) * 10 + 5
    g = {(x, y) for x in range(7, COLUMNS - 10) for y in range(GROUND)}

    def tile_id(x: int, y: int) -> int:  # the SMB1 ground ids (top / sides / fill), from the neighbours
        up, left, right = (x, y + 1) in g, (x - 1, y) in g, (x + 1, y) in g
        if not up:
            return 59 if left and right else 58 if right else 60 if left else 29
        return 62 if left and right else 61 if right else 63 if left else 29

    for x, y in sorted(g):
        t = course.GroundTile()
        t.x, t.y, t.id, t.background_id = x, y, tile_id(x, y), 0
        a.ground.items.append(t)

    o = course.CourseObject()  # the one-way wall: 2 x 2, centre (240, 96) px, facing right (d = 0: flags bits 22-23)
    o.x, o.y = 2400, (GROUND * 16 + 16) * 10
    o.width = o.height = 2
    o.flags, o.child_flags, o.extended_data = 0x06000040, 0x06000040, 0
    o.id, o.child_id, o.link_id, o.sound_effect_id = 67, -1, -1, -1
    a.objects.items.append(o)
    return c.to_bytes()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--romfs", type=Path, required=True, help="the game's RomFS (3.0.3), for the title-course archives")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--elf", type=Path, help="the game's main executable as an ELF (the key table is found in it)")
    src.add_argument("--table", type=Path, help="the course key table (smmfmt.smm2.keyfind output)")
    ap.add_argument("--out", type=Path, required=True, help="the mod folder to write (RECOMP_MOD_DIR)")
    args = ap.parse_args()

    names = ["title_course_data_%02d" % n for n in range(10)]
    archives = [oead.Sarc(oead.yaz0.decompress((args.romfs / "Course" / (n + ".szs")).read_bytes())) for n in names]
    if args.table:
        table = crypto.table_from_bytes(args.table.read_bytes())
    else:
        bcd = bytes(next(iter(archives[0].get_files())).data)
        table = crypto.table_from_bytes(find_table(args.elf.read_bytes(), bcd)[1])
    body = build()
    out = args.out / "romfs" / "Course"
    out.mkdir(parents=True, exist_ok=True)
    for name, sarc in zip(names, archives):
        w = oead.SarcWriter.from_sarc(sarc)
        w.files[name + ".bcd"] = crypto.encrypt_bcd(body, table, iv=bytes(16), random_state=(1, 2, 3, 4))
        (out / (name + ".szs")).write_bytes(oead.yaz0.compress(w.write()[1]))
    (args.out / "oneway_ledge.pad").write_text(PAD)
    print(f"{args.out}: the test course as the ten title courses, and oneway_ledge.pad")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
