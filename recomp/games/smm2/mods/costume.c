// Mod 2, Mystery Mushroom costumes (research/specs/recomp-mods.md §3): SMM1's costumes worn in SMB1-style courses.
//
// The player wearing a costume stays form 0 (small Mario) for the game's logic: size class, physics, hit boxes and the
// block and power-up rules are untouched, which is SMM1's costume behaviour. Only what is drawn changes:
//   - the model builder (0x710155fcd0, once per player per course) loads, for the SMB1 style, a costume model into the
//     spare form slot 12 (Balloon: unused in SMB1, size class 1 like small Mario) by pointing that slot's model name
//     at "Player_Chara_<res>" for the call (the models are generated from the player's dumps, smm1/costume_gen.c,
//     and laid over the romfs: game_romfs_overlay);
//   - the visual slot setter (0x710155fcc0) shows slot 12 where the game asks for slot 0 while a costume is worn.
// Settings: RECOMP_MOD_COSTUMES=1 / costumes=1 in smm2mods.ini turns it on (models generated at the next start);
// RECOMP_COSTUME=<res> (e.g. Peach) wears that costume from the start (tests).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod.h"

bool costume_generate(void);           // smm1/costume_gen.c
const char *costume_overlay_dir(void);
int costume_count(void);
const char *costume_res(int i);

#define BUILDER MAIN_ADDR(0x155fcd0)      // model holder build (holder, style, character, heap)
#define SETTER MAIN_ADDR(0x155fcc0)       // visual slot setter (holder, int *slot): holder +0x478
#define NAME_TABLE MAIN_ADDR(0x2c17638)   // model base names: SafeString {vtable, char *} [style * 16 + form]
#define COSTUME_SLOT 12                   // Balloon: SMB1 never uses it; size class 1 (small)

// ---------------------------------------------------------------- settings (a block of its own in smm2mods.ini)
static int g_on = -1;
static char g_wear[48];  // the costume worn (res name), "" none
static char g_model_name[96] = "Player_Chara_Mario";

static int ini_costumes(void) {
    FILE *f = fopen("smm2mods.ini", "r");
    if (!f) return 0;
    char line[256];
    int v = 0;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "costumes=", 9)) v = atoi(line + 9);
    fclose(f);
    return v;
}
bool costumes_on(void) {
    if (g_on < 0) {
        const char *e = getenv("RECOMP_MOD_COSTUMES");
        g_on = e ? atoi(e) != 0 : ini_costumes() != 0;
        const char *w = getenv("RECOMP_COSTUME");
        if (g_on && w && *w) snprintf(g_wear, sizeof g_wear, "%s", w);
        if (g_on) LOG("[costume] Mystery Mushroom costumes on%s%s\n", g_wear[0] ? ", wearing " : "", g_wear);
    }
    return g_on;
}

void costume_wear(const char *res) {  // NULL / "": none (takes effect when the player's models are next built)
    snprintf(g_wear, sizeof g_wear, "%s", res ? res : "");
}
const char *costume_worn(void) { return g_wear; }

// the runtime lays this folder over the game's romfs (runtime/services/fs.c): the rebuilt Pack/M1_Model.pack
const char *game_romfs_overlay(void) {
    if (!costumes_on() || !costume_generate()) return NULL;
    return costume_overlay_dir();
}

// ---------------------------------------------------------------- the hooks
static void builder(Cpu *c) {
    uint32_t style = (uint32_t)c->x[1];
    if (!costumes_on() || style != 0 || !g_wear[0]) {
        guest_call_orig(c, BUILDER);
        return;
    }
    snprintf(g_model_name, sizeof g_model_name, "Player_Chara_%s", g_wear);
    uint64_t slot = NAME_TABLE + (uint64_t)(style * 16 + COSTUME_SLOT) * 16;
    uint64_t was = g64(slot + 8);
    s64(slot + 8, (uint64_t)(uintptr_t)g_model_name);
    guest_call_orig(c, BUILDER);
    s64(slot + 8, was);
    LOG("[costume] player models built with %s in slot %d\n", g_model_name, COSTUME_SLOT);
}

static void setter(Cpu *c) {
    uint64_t holder = c->x[0], ps = c->x[1];
    int32_t slot = (int32_t)g32(ps);
    if (costumes_on() && g_wear[0] && slot == 0 && g32(holder + 0x470) == 0) slot = COSTUME_SLOT;
    s32(holder + 0x478, (uint32_t)slot);
}

__attribute__((constructor)) static void costume_register(void) {
    hle_override(BUILDER, builder, "costume: model builder");
    hle_override(SETTER, setter, "costume: visual slot");
}
