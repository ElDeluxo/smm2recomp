// SMM1 extras: settings that each turn one SMM1 behaviour on in the otherwise exact SMM2 game. All off by default,
// so the game is SMM2 unless the player opts in. One switch each in the overlay menu's Mods tab (settings.c lists
// them after the 100 Mario mod's), kept in smm2mods.ini; an environment variable wins over the ini:
//
//   RECOMP_SMM1_SHELL_JUMPS=1   smm1_shell_jumps=1     mid-air shell jumps as in SMM1
//   RECOMP_SMM1_PIPES=1         smm1_overlap_pipes=1   pipes may overlap pipes (Course Maker and courses)
//   RECOMP_HUNDRED_SMM1_CARD=1  hundred_smm1_card=1    100 Mario: SMM1's start card at a course's start (off: SMM2's
//                                                      own course-in card, as after a miss; mods/smm1/flow.c)
//
// The behaviour comes from the game's own code: host hooks after single instructions (games/smm2/mods/smm1_extras.py
// lists them) change a register or the flags while a setting is on, and do nothing otherwise. What SMM1 does was
// read from SMM1's own code (Block.rpx v208); both are described in docs/mods.md ("SMM1 extras").
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod.h"

typedef struct { const char *label, *desc; bool *value; bool def; } GameModToggle;  // (runtime/game_hooks.c)

static bool shell_jumps, overlap_pipes, smm1_card;

static const struct Setting {
    const char *key, *env;
    bool *value;
} settings[] = {
    {"smm1_shell_jumps", "RECOMP_SMM1_SHELL_JUMPS", &shell_jumps},
    {"smm1_overlap_pipes", "RECOMP_SMM1_PIPES", &overlap_pipes},
    {"hundred_smm1_card", "RECOMP_HUNDRED_SMM1_CARD", &smm1_card},
};
#define NSETTINGS (sizeof settings / sizeof *settings)

static const char *ini_path(void) { return getenv("RECOMP_MOD_INI") ? getenv("RECOMP_MOD_INI") : "smm2mods.ini"; }

static void load_once(void) {
    FILE *f = fopen(ini_path(), "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '=');
            if (line[0] == '#' || !eq) continue;
            *eq = 0;
            for (size_t i = 0; i < NSETTINGS; i++)
                if (!strcmp(line, settings[i].key)) *settings[i].value = atoi(eq + 1) != 0;
        }
        fclose(f);
    }
    for (size_t i = 0; i < NSETTINGS; i++) {
        if (getenv(settings[i].env)) *settings[i].value = atoi(getenv(settings[i].env)) != 0;
        if (*settings[i].value) LOG("[mod] SMM1 extras: %s on\n", settings[i].key);
    }
}
static void load(void) {  // (the game's threads and the menu: once)
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, load_once);
}

void mod_ini_set(const char *key, const char *val);  // (settings.c: replace or add one key=value line)

bool hundred_smm1_card(void) {  // (hundred.h; the flow asks on its render thread)
    load();
    return smm1_card;
}

// ---- the Mods tab (settings.c appends these to its own)
static bool shown[NSETTINGS];  // what the tab last showed, to see which one changed

int smm1_extras_toggles(GameModToggle *out, int room) {
    static const GameModToggle toggles[] = {
        {"Mid-air shell jumps (SMM1)",
         "SMM1's timing for landing on a shell: a flat stomp window when falling (SMM2 narrows it at slow falls and "
         "widens it at fast ones), and after a mid-air kick off a wall only SMM1's short no-touch time, so the shell "
         "coming back can be landed on. Immediately.",
         &shown[0], false},
        {"Overlapping pipes (SMM1)",
         "Pipes may overlap other pipes, in the Course Maker and in courses, as in SMM1 (SMM2 refuses the placement "
         "and treats such a course as broken). From the next course start.",
         &shown[1], false},
        {"SMM1 course card (100 Mario)",
         "In the 100 Mario Challenge, SMM1's course card (the course, its maker and the lives) after the map instead "
         "of SMM2's own course-in card. From the next course.",
         &shown[2], false},
    };
    load();
    int n = 0;
    for (size_t i = 0; i < NSETTINGS && n < room; i++) {
        shown[i] = *settings[i].value;
        out[n++] = toggles[i];
    }
    return n;
}

void smm1_extras_toggles_changed(void) {
    for (size_t i = 0; i < NSETTINGS; i++) {
        if (shown[i] == *settings[i].value) continue;
        *settings[i].value = shown[i];
        mod_ini_set(settings[i].key, shown[i] ? "1" : "0");
        LOG("[mod] SMM1 extras: %s %s\n", settings[i].key, shown[i] ? "on" : "off");
    }
}

// ---- the hooks (smm1_extras.py)

// A shell: an enemy in its shell mode (its shell state object's mode 1; SMM2's enemy contact picks the still / sliding
// shell profiles on the same test).
static bool on_heap(uint64_t a) { return a >= HEAP_BASE && a < HEAP_BASE + HEAP_MAX - 0x1000 && !(a & 7); }
static bool in_shell(uint64_t enemy) {
    if (!on_heap(enemy)) return false;
    uint64_t st = g64(enemy + 0x5c8);
    return on_heap(st) && g32(st + 0x28) == 1;
}

static float f32v(Cpu *c, int r) { float f; memcpy(&f, &c->v[r], 4); return f; }
static float g32f(uint64_t a) { float f; uint32_t w = g32(a); memcpy(&f, &w, 4); return f; }

void smm1_extras_after(Cpu *c, uint64_t addr) {
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("RECOMP_SMM1_LOG") != NULL;
    load();
    switch (addr - MAIN_ADDR(0)) {
    case 0x1267ce8:  // stomp test, cmp w11, #1 ("falling") before SMM2's step for ordinary-size targets
        if (!in_shell(g64(c->x[20] + 0xa8))) break;
        if (dbg) {  // the window both ways: s0 the table's, s8 the extra; SMM2 adds -3 + the player's fall this frame
            uint64_t p = c->x[19];
            float w = f32v(c, 0), extra = f32v(c, 8), dy = g32f(p + 0x234) - g32f(p + 0x308);
            bool step = FLAG_Z(c) && (uint32_t)c->x[9] == 0;  // falling, ordinary-size target
            LOG("[smm1] shell stomp test: falling %d, window SMM1 %.3f, SMM2 %.3f (player y %.3f dy %.3f)%s\n",
                (int)FLAG_Z(c), w + extra, step ? w - 3 + extra + (dy < 0 ? -dy : 0) : w + extra, g32f(p + 0x234), dy,
                shell_jumps ? " -> SMM1" : "");
        }
        if (shell_jumps) (void)subs32(c, 0, 1);  // -> b.ne: the flat window
        break;
    case 0xfe827c: {  // overlap rule: x19 the pair (this part's record at +0, the other's at +0x28), w0 "other solid"
        uint16_t self = (uint16_t)g32(c->x[19] + 0x20), other = (uint16_t)g32(c->x[19] + 0x48);
        bool smm1 = overlap_pipes && self == 9 && other == 9;
        if (dbg && (self == 9 || other == 9))
            LOG("[smm1] overlap rule: part %u against part %u: %s%s\n", self, other, (c->x[0] & 1) ? "checked" : "free",
                smm1 ? " -> free (SMM1)" : "");
        if (smm1) c->x[0] = 0;
        break;
    }
    case 0x1202c88:  // enemy contact, kick accepted with no window written: mov w9, #16
        if (!in_shell(g64(c->x[23]))) break;
        if (dbg) LOG("[smm1] mid-air kick without a no-hit window: %s\n", shell_jumps ? "5 frames (SMM1)" : "16 frames (SMM2)");
        if (shell_jumps) c->x[9] = 0;  // -> the epilogue's 5 frames
        break;
    }
}
