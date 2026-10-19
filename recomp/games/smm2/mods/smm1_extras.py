"""SMM1 extras: host hooks after single instructions of SMM2 v3.0.3 (merged into games/smm2/patches.py).

Each entry keeps the game's own instruction and then calls smm1_extras_after(c, address)
(games/smm2/mods/smm1_extras.c), which changes something only while that feature's setting is on. With every setting
off the instruction behaves exactly as before.

Each point is checked against the instruction word it expects, so another build of the game fails loudly instead of
being hooked in the wrong place. Offsets are from the start of `main` (the recomp loads it at 0x7000000000; the
generator rebases them when the guest window moves).
"""

MAIN = 0x7000000000

# offset in main -> (expected instruction word, what the host does there when the setting is on)
HOOK_POINTS = {
    # Mid-air shell jumps (SMM1): the player-on-enemy stomp test. For an ordinary-size target while the player is
    # falling, SMM2 narrows the window by 3 px and widens it by the player's fall this frame (cmp w11, #1 / b.ne:
    # "falling?" before that step); SMM1 has a flat window. On a shell the host makes the compare say "not falling"
    # for that step only, so the SMM1 window applies.
    0x1267ce8: (0x7100057F, 'shell jumps: stomp window, SMM2-only step'),
    # Mid-air shell jumps (SMM1): enemy contact, a kick the shell accepted without writing the kicker's no-hit window
    # (the mid-air wall-assist kick): SMM2 fills it with 16 frames (mov w9, #0x10); SMM1 leaves it empty, so the
    # contact epilogue's 5 frames apply. On a shell the host makes the value 0.
    0x1202c88: (0x321C03E9, 'shell jumps: kick no-hit window 16'),
    # Overlapping pipes (SMM1): the course check's rule for a pipe against another part (the placement analyzer; the
    # course loader rejects a course whose pipes overlap and plays a blank one instead): bl to the "is the other part
    # solid" test, which counts pipes. For a pipe against a pipe the host answers "no".
    0xfe827c: (0x94000085, 'pipes: overlap rule, other part solid?'),
}


def _hook(word):
    def patch(gen, addr):
        have = gen.words[(addr - gen.lo) // 4]
        if have != word:
            raise SystemExit(f'mods/smm1_extras.py: {addr:#x} is {have:#010x}, not {word:#010x} (not v3.0.3?)')
        ins = gen.lift(addr)
        if ins.kind == 'bl':  # (a call: through the dispatcher, so the host runs after it returns)
            orig = f'c->x[30] = {addr + 4:#x}ull; c->pc = {ins.target:#x}ull; rt_dispatch(c);'
        else:
            orig = ' '.join(gen.render_single(ins, addr, None, set()))
        return f'{orig} {{ extern void smm1_extras_after(Cpu *, uint64_t); smm1_extras_after(c, {addr:#x}ull); }}'
    return patch


PATCHES = {MAIN + off: _hook(word) for off, (word, _) in HOOK_POINTS.items()}
