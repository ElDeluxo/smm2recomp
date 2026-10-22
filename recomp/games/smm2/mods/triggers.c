// The overlay menu's "Try it now" rows (runtime/game_hooks.c game_mod_triggers, drawn by runtime/ui/menu.mm tab_mods):
// a mod effect on demand, for trying a mod out. Each mod adds its own rows from a constructor (mod_trigger_add, mod.h),
// in the order the mods are linked; the menu draws them under the switches.
//
// The rule every row follows (DECISIONS.md, 2026-10-22): a try-it-out helper, greyed out outside a course and in
// Endless / 100 Mario runs (they keep records); trigger() only records a request, the mod applies it on the game's
// thread at the player's next update. Rows so far: "Lanky Mario now" (lanky.c), "Mystery Mushroom costume now" with a
// choice of costume (costume.c: its choices are filled in later, through the row mod_trigger_add returns, once the
// costume names are read; the menu reads choices and nchoices at each draw).
#include "mod.h"

#define MAX_TRIGGERS 16
static GameModTrigger g_list[MAX_TRIGGERS];
static int g_n;

GameModTrigger *mod_trigger_add(const GameModTrigger *t) {
    if (g_n == MAX_TRIGGERS) return NULL;
    g_list[g_n] = *t;
    return &g_list[g_n++];
}

int game_mod_triggers(const GameModTrigger **out) {
    *out = g_n ? g_list : NULL;
    return g_n;
}
