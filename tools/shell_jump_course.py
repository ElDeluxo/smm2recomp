"""Test course for the SMM1 extra "Mid-air shell jumps (SMM1)" (recomp/docs/mods.md, "SMM1 extras").

Builds our own small SMB1-style course and installs it as all ten title courses of a mod folder, so the game plays
it on its title screen straight away (RECOMP_MOD_DIR), with the scripted input this file also writes. Nothing of the
game goes into the course: it is made from scratch here; only the title-course archives' wrapping is copied from the
player's own RomFS, and the course is encrypted with the key table found in the player's own executable. The output is
private (never commit it).

    python -I tools/shell_jump_course.py --romfs recomp/private/smm2/extracted/romfs \\
        --elf <main.elf> | --table <course_table.bin>  --out <mod folder>

The course (tiles of 16 px, y up): a plateau 5 tiles high to x = 19 with a low ceiling over it (no running jump from
it), a 2-tile pit, a 1-tile ledge 3 tiles high at x = 22 against a wall 8 tiles high (x = 23 on, to the goal). A
Buzzy Shell rests on the ledge with its centre at x = 354 px, 14 px from the wall: where the game's own wall assist
leaves a shell (one tile from the wall, about 352 px), between tiles, which the Course Maker can't place.

The input (<out>/shell_jump.pad, RECOMP_PAD_SCRIPT, one line per player update): walk right off the plateau; falling
past the ledge, Mario's side touches the shell: the mid-air kick off the wall (the shell is kicked into the wall, Mario
gets a small hop); then A. With the setting on (SMM1: 5 frames before the kicker can touch the shell again) Mario
lands on the shell coming back off the wall, bounces and reaches the top of the wall; off (SMM2: 16 frames) the shell
passes under him and he drops back onto the ledge.
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

COLUMNS = 235
PAD = "# mid-air shell jump (tools/shell_jump_course.py): one line per player update\n" \
      "60 -\n199 RIGHT\n25 RIGHT A\n120 RIGHT\n"


def build() -> bytes:
    c = course.Course.blank("M1")
    h = c.header
    h.timer, h.game_version, h.management_flags, h.clear_check_time = 500, 1, 1, 0xFFFFFFFF
    h.year, h.month, h.day, h.hour, h.minute = 2026, 10, 22, 12, 0
    h.name = "Shell jump test"
    for area in c.areas:
        area.header.boundary_right, area.header.boundary_top = COLUMNS * 16, 432
    a = c.main
    a.header.liquid_start_height = a.header.liquid_end_height = 1
    h.start_y, h.goal_y, h.goal_x = 5, 8, (COLUMNS - 10) * 10 + 5

    cols = {}
    for x0, x1, height in ((7, 19, 5), (20, 21, 1), (22, 22, 3), (23, COLUMNS - 11, 8)):
        for x in range(x0, x1 + 1):
            cols[x] = height
    g = {(x, y) for x, height in cols.items() for y in range(height)}
    g |= {(x, y) for x in range(8, 20) for y in (7, 8)}  # the ceiling over the plateau

    def tile_id(x: int, y: int) -> int:  # the SMB1 ground ids (top / sides / fill), from the neighbours
        up, left, right = (x, y + 1) in g, (x - 1, y) in g, (x + 1, y) in g
        if not up:
            return 59 if left and right else 58 if right else 60 if left else 29
        return 62 if left and right else 61 if right else 63 if left else 29

    for x, y in sorted(g):
        t = course.GroundTile()
        t.x, t.y, t.id, t.background_id = x, y, tile_id(x, y), 0
        a.ground.items.append(t)

    o = course.CourseObject()  # the Buzzy Shell (Buzzy Beetle, alternate form), centre 354 px, on the ledge (y 3)
    o.x, o.y = 3540, 3 * 160 + 80
    o.width = o.height = 1
    o.flags, o.child_flags, o.extended_data = 0x06000044, 0x06000040, 0
    o.id, o.child_id, o.link_id, o.sound_effect_id = 28, -1, -1, -1
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
    (args.out / "shell_jump.pad").write_text(PAD)
    print(f"{args.out}: the test course as the ten title courses, and shell_jump.pad")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
