// The overlay menu's "Try it now" rows (runtime/game_hooks.c game_mod_triggers, drawn by runtime/ui/menu.mm tab_mods):
// a mod effect on demand, for trying a mod out. Each mod adds its own rows from a constructor (mod_trigger_add, mod.h),
// in the order the mods are linked; the menu draws them under the switches.
//
// The rule every row follows (DECISIONS.md, 2026-10-22): a try-it-out helper, greyed out outside a course and in
// Endless / 100 Mario runs (they keep records); trigger() only records a request, the mod applies it on the game's
// thread at the player's next update. Rows so far: "Lanky Mario now" (lanky.c).
#include "mod.h"

#define MAX_TRIGGERS 16
static GameModTrigger g_list[MAX_TRIGGERS];
static int g_n;

void mod_trigger_add(const GameModTrigger *t) {
    if (g_n < MAX_TRIGGERS) g_list[g_n++] = *t;
}

int game_mod_triggers(const GameModTrigger **out) {
    *out = g_n ? g_list : NULL;
    return g_n;
}
