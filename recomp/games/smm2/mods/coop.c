// Co-op in Endless and 100 Mario: with this on and more than one player's controller connected (runtime/hle/input.c
// input_players), an Endless / 100 Mario course is played the way SMM2 plays Play Together's: every player in the
// course on their own controller, the rules SMM2's local multiplayer has (owner, 2026-10-21: lives are shared, one
// is lost only when every player is down, and anyone reaching the goal clears the course).
//
// What it takes (hooks.c, patches.py):
//   - the course's play mode ([*0x2a6bcd0 + 0xe0], stored by the player spawn 0x7100e581d0) is 0 for one player
//     (Endless, the title course, ...), 1 for local multiplayer (Play Together), 2 online. In an Endless course it is
//     made 1, with the set-up's player count ([*0x2a66fb8 + 0x40]) the number of players: SMM2 then spawns them all
//     and gives each their own controller.
//   - the controllers' poll (sead's Npad device, 0x7100263d90) reads Npads 0 .. [+0x28]; Endless leaves it at 1.
//   - Endless's miss request (endless.c start_course) comes for any player's miss: while a player still stands, no
//     life is lost.
//
// One switch in the Mods tab (off by default), kept in smm2mods.ini; an environment variable wins over the ini:
//   RECOMP_COOP=0/1   coop=0/1
// RECOMP_COOP_TEST=<n> plays as if n players were connected (tests: LARECOMP_INPUT2 scripts player 2).
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod.h"

typedef struct { const char *label, *desc; bool *value; bool def; } GameModToggle;  // (runtime/game_hooks.c)
void mod_ini_set(const char *key, const char *val);                                 // (settings.c)
int input_players(void);                                                            // runtime/hle/input.c

static bool g_on, g_shown;
static int g_test = -1;
static void load_once(void) {
    const char *ini = getenv("RECOMP_MOD_INI") ? getenv("RECOMP_MOD_INI") : "smm2mods.ini";
    FILE *f = fopen(ini, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "coop=", 5)) g_on = atoi(line + 5) != 0;
        fclose(f);
    }
    if (getenv("RECOMP_COOP")) g_on = atoi(getenv("RECOMP_COOP")) != 0;
    g_test = getenv("RECOMP_COOP_TEST") ? atoi(getenv("RECOMP_COOP_TEST")) : 0;
    if (g_test > 1) g_on = true;
    LOG("[mod] co-op in Endless / 100 Mario %s\n", g_on ? "on" : "off");
}
static bool coop_on(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, load_once);
    return g_on;
}

int coop_toggles(GameModToggle *out, int room) {  // (settings.c appends it to the Mods tab)
    static const GameModToggle t = {
        "Co-op in Endless and 100 Mario",
        "With a second controller connected, Endless Challenge and 100 Mario courses are played together, as in "
        "Play Together: each controller is a player (Mario, Luigi, Toad, Toadette). Lives are shared: one is lost "
        "only when every player is down, and anyone reaching the goal clears the course. From the next course.",
        &g_shown, false};
    if (room < 1) return 0;
    g_shown = coop_on();
    out[0] = t;
    return 1;
}
void coop_toggles_changed(void) {
    if (g_shown == coop_on()) return;
    g_on = g_shown;
    mod_ini_set("coop", g_on ? "1" : "0");
    LOG("[mod] co-op in Endless / 100 Mario %s\n", g_on ? "on" : "off");
}

// players for the next Endless course: 1 without co-op
int coop_players(void) {
    if (!coop_on()) return 1;
    int n = g_test > 1 ? g_test : input_players();
    return n < 1 ? 1 : n > 4 ? 4 : n;
}

// the course being played is an Endless one in co-op (hooks.c sets it at the course's play mode store)
static volatile bool g_course;
void coop_course(bool on) {
    if (on != g_course) LOG("[mod] co-op %s\n", on ? "course: local multiplayer" : "off for this course");
    g_course = on;
}
bool coop_in_course(void) { return g_course; }

// The course's players (the player manager *0x2c19290: four slots at +0x28): one is down when its state (+0x2424)
// is 3, in a bubble when bit 5 of +0x48c is set (as the course's own update checks them)
int coop_players_standing(void) {
    uint64_t pm = g64(MAIN_ADDR(0x2c19290));
    if (!pm) return 0;
    int n = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t p = g64(pm + 0x28 + 8 * (uint64_t)i);
        if (!p) continue;
        uint32_t st = g32(p + 0x2424);
        bool bubble = (g8(p + 0x48c) >> 5) & 1;
        LOG("[mod] co-op: player %d state %u%s\n", i, st, bubble ? " (bubble)" : "");
        if (st != 3 && !bubble) n++;
    }
    return n;
}
