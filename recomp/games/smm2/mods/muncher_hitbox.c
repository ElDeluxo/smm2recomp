// Muncher hit box (SMM1): Munchers hurt the player the way SMM1's did, so a one-tile gap between two Muncher columns
// can be fallen or jumped through with room to spare instead of only dead centre.
//
// SMM2 gives a Muncher one solid body, a 16 x 16 box that is also a spike: whatever side the player touches hurts.
// Small or big, the player's wall sensors reach 7.9 px either side of him, so in a 16 px gap any sideways offset
// beyond 0.1 px is a touch, and a touch in the air is a hit. SMM1 built the Muncher from two bodies: the same 16 x 16
// box, harmless, for everything else, and for the player only a spike box 2 px smaller on every side (12 x 12; a Big
// Muncher's scales with it). The player meets the spike 2 px later from any side: the gap is 20 px wide for him and
// he can be 2.1 px off centre either way. The research is in research/specs/objects/57-muncher.md (private).
//
// The mod gives the player SMM1's box. While a player's per-frame update runs (that is where he moves and his
// sensors meet the ground; it is called through his vtable), every live Muncher's body (its edges and its bounds) is
// shrunk to the inner box, and put back exactly as it was when the update returns. Enemies, items and everything else
// keep meeting SMM2's 16 x 16 box, as they met SMM1's outer one. Munchers are found when they are built and dropped when they are
// destroyed (both through the actor's own function pointers). Nothing else changes.
//
// One switch in the Mods tab (off by default), kept in smm2mods.ini; an environment variable wins over the ini:
//   RECOMP_MUNCHER_SMM1_HITBOX=0/1   muncher_smm1_hitbox=0/1
// It applies from the next player update (immediately).
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod.h"

typedef struct { const char *label, *desc; bool *value; bool def; } GameModToggle;  // (runtime/game_hooks.c)
void mod_ini_set(const char *key, const char *val);                                 // (settings.c)

// ---------------------------------------------------------------- the setting
static bool g_on, g_shown;
static void load_once(void) {
    const char *ini = getenv("RECOMP_MOD_INI") ? getenv("RECOMP_MOD_INI") : "smm2mods.ini";
    FILE *f = fopen(ini, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "muncher_smm1_hitbox=", 20)) g_on = atoi(line + 20) != 0;
        fclose(f);
    }
    if (getenv("RECOMP_MUNCHER_SMM1_HITBOX")) g_on = atoi(getenv("RECOMP_MUNCHER_SMM1_HITBOX")) != 0;
    LOG("[mod] Muncher hit box (SMM1) %s\n", g_on ? "on" : "off");
}
static bool box_on(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, load_once);
    return g_on;
}

int muncher_hitbox_toggles(GameModToggle *out, int room) {  // (settings.c appends it to the Mods tab)
    static const GameModToggle t = {
        "Muncher hit box (SMM1)",
        "Munchers hurt the player when he reaches 2 px inside them, as in SMM1, instead of at their edge: a one-tile "
        "gap between Muncher columns can be fallen or jumped through up to 2 px off centre, not only dead centre. "
        "Enemies and items still meet the whole block. Immediately.",
        &g_shown, false};
    if (room < 1) return 0;
    g_shown = box_on();
    out[0] = t;
    return 1;
}
void muncher_hitbox_toggles_changed(void) {
    if (g_shown == box_on()) return;
    g_on = g_shown;
    mod_ini_set("muncher_smm1_hitbox", g_on ? "1" : "0");
    LOG("[mod] Muncher hit box (SMM1) %s\n", g_on ? "on" : "off");
}

// ---------------------------------------------------------------- the game (v3.0.3)
#define BUILD 0x107d340         // BlackPakkun's build function: both profiles (Muncher, Big Muncher) point at it
#define DTOR 0x107ed30          // its destructor (vtable +0x18) ...
#define DTOR_DELETE 0x107ed80   // ... and the deleting one (vtable +0x20)
#define PLAYER_UPDATE 0x158a590 // the player's per-frame update (vtable slot 9)
#define ACTOR_VT 0x28b9128      // a Muncher's vtable
#define BODY 0xd20              // its body: a 4-point BG collider (vtable BODY_VT)
#define BODY_VT 0x28695f8
#define BODY_KIND 0x360         // 0x14 (a spike) ...
#define BODY_ATTR 0x364         // ... with attributes 0x110714 (the top 5 bits are the theme's)
#define SEG_N 0x3c0             // edges this frame: count (4), pointer (inside the actor, +0x1120) ...
#define SEG 0x3c8
#define PREV_N 0x3d0            // ... and last frame's (+0x1180), for moving bodies
#define PREV 0x3d8
#define SEG_SIZE 0x18           // an edge: x0, y0, x1, y1 at +4..+0x10, relative to the body's centre
#define BOXES 0x238             // its world bounds this frame, last frame and both (min x, y, max x, y; the player's
                                // queries go by these as well as by the edges)
#define SAVE_SIZE (8 * SEG_SIZE + 3 * 16)
#define INSET 0.75f             // SMM1's spike box / its solid box: 12 / 16 (6 / 8 a side)

static bool on_heap(uint64_t a) { return a >= HEAP_BASE && a < HEAP_BASE + HEAP_MAX - 0x2000 && !(a & 7); }

// a live Muncher whose body is SMM2's 4-edge box as built (anything else is left alone)
static bool muncher(uint64_t a) {
    if (!on_heap(a) || g64(a) != MAIN_ADDR(ACTOR_VT)) return false;
    uint64_t b = a + BODY;
    return g64(b) == MAIN_ADDR(BODY_VT) && g32(b + BODY_KIND) == 0x14 && (g32(b + BODY_ATTR) & 0x7ffffff) == 0x110714 &&
           g32(b + SEG_N) == 4 && g64(b + SEG) == a + 0x1120 && g32(b + PREV_N) == 4 && g64(b + PREV) == a + 0x1180;
}

// ---------------------------------------------------------------- the Munchers alive
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t *g_list;
static int g_n, g_cap;

static void track(uint64_t a) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_n; i++)
        if (g_list[i] == a) { pthread_mutex_unlock(&g_lock); return; }
    if (g_n == g_cap) {
        int cap = g_cap ? g_cap * 2 : 256;
        uint64_t *l = realloc(g_list, (size_t)cap * sizeof *l);
        if (!l) { pthread_mutex_unlock(&g_lock); return; }
        g_list = l, g_cap = cap;
    }
    g_list[g_n++] = a;
    pthread_mutex_unlock(&g_lock);
}
static void untrack(uint64_t a) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_n; i++)
        if (g_list[i] == a) { g_list[i] = g_list[--g_n]; break; }
    pthread_mutex_unlock(&g_lock);
}
RT_ON_RESET(muncher_reset) {  // (guest memory is wiped: no Muncher survives a reboot)
    free(g_list);
    g_list = NULL, g_n = g_cap = 0;
}

static void build(Cpu *c) {
    guest_call_orig(c, MAIN_ADDR(BUILD));
    uint64_t a = c->x[0];
    if (muncher(a)) track(a);
}
static void dtor(Cpu *c) {
    untrack(c->x[0]);
    guest_call_orig(c, MAIN_ADDR(DTOR));
}
static void dtor_delete(Cpu *c) {
    untrack(c->x[0]);
    guest_call_orig(c, MAIN_ADDR(DTOR_DELETE));
}

// ---------------------------------------------------------------- the player's update
// Both edge lists (this frame's and last frame's, which a moving body is swept between) and the three bounds are scaled
// towards their own centre: the box keeps its centre and turns 4 px narrower and lower (8 for a Big Muncher), whichever
// way it faces.
static float gf(uint64_t a) { float f; uint32_t w = g32(a); memcpy(&f, &w, 4); return f; }
static void sf(uint64_t a, float f) { uint32_t w; memcpy(&w, &f, 4); s32(a, w); }
static void shrink(uint64_t seg) {
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 2; k++) {
            float x = gf(seg + i * SEG_SIZE + 4 + k * 8), y = gf(seg + i * SEG_SIZE + 8 + k * 8);
            x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1, y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
        }
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 2; k++) {
            uint64_t p = seg + i * SEG_SIZE + 4 + k * 8;
            sf(p, cx + (gf(p) - cx) * INSET);
            sf(p + 4, cy + (gf(p + 4) - cy) * INSET);
        }
}

static void shrink_box(uint64_t b) {
    float x0 = gf(b), y0 = gf(b + 4), x1 = gf(b + 8), y1 = gf(b + 12), cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    if (!(x1 > x0) || !(y1 > y0) || x1 - x0 > 1e6f) return;  // (an empty box: +-FLT_MAX)
    sf(b, cx + (x0 - cx) * INSET), sf(b + 4, cy + (y0 - cy) * INSET);
    sf(b + 8, cx + (x1 - cx) * INSET), sf(b + 12, cy + (y1 - cy) * INSET);
}
static void player_update(Cpu *c) {
    static uint8_t *saved;
    static uint64_t *who;
    static int cap;
    int n = 0;
    if (box_on()) {
        pthread_mutex_lock(&g_lock);
        if (g_n > cap) {
            uint8_t *s = realloc(saved, (size_t)g_n * SAVE_SIZE);
            if (s) saved = s;
            uint64_t *w = realloc(who, (size_t)g_n * sizeof *w);
            if (w) who = w;
            if (s && w) cap = g_n;
        }
        for (int i = 0; i < g_n && n < cap; i++) {
            uint64_t a = g_list[i];
            if (!muncher(a)) continue;
            uint8_t *sv = saved + (size_t)n * SAVE_SIZE;
            memcpy(sv, (const void *)(uintptr_t)(a + 0x1120), 8 * SEG_SIZE);
            memcpy(sv + 8 * SEG_SIZE, (const void *)(uintptr_t)(a + BODY + BOXES), 0x30);
            who[n++] = a;
            shrink(a + 0x1120);
            shrink(a + 0x1180);
            for (int k = 0; k < 3; k++) shrink_box(a + BODY + BOXES + k * 16);
        }
        pthread_mutex_unlock(&g_lock);
    }
    guest_call_orig(c, MAIN_ADDR(PLAYER_UPDATE));
    for (int i = 0; i < n; i++)  // (as they were, byte for byte; not into a Muncher destroyed meanwhile)
        if (g64(who[i]) == MAIN_ADDR(ACTOR_VT)) {
            memcpy((void *)(uintptr_t)(who[i] + 0x1120), saved + (size_t)i * SAVE_SIZE, 8 * SEG_SIZE);
            memcpy((void *)(uintptr_t)(who[i] + BODY + BOXES), saved + (size_t)i * SAVE_SIZE + 8 * SEG_SIZE, 0x30);
        }
}

__attribute__((constructor)) static void muncher_hitbox_register(void) {
    hle_override(MAIN_ADDR(BUILD), build, "Muncher hit box: build");
    hle_override(MAIN_ADDR(DTOR), dtor, "Muncher hit box: destructor");
    hle_override(MAIN_ADDR(DTOR_DELETE), dtor_delete, "Muncher hit box: deleting destructor");
    hle_override(MAIN_ADDR(PLAYER_UPDATE), player_update, "Muncher hit box: player update");
}
