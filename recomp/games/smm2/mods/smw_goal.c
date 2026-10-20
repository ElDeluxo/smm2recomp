// SMW goal wipe: in a Super Mario World style course, a goal-tape clear in real play (not the Course Maker's test
// play) ends the way SMW's levels do. Once the fanfare is over, a black circle closes on Mario with SMW's own
// goal-wipe sound. Where SMM2 shows its clear panel (Course World, Story, Endless...), the black then fades into the
// panel's own wipe; where it shows none (My Courses), the screen stays black while SMM2 changes scene under it and
// fades away when the next screen is up. The castle's axe clears as before.
//
// The sound is SMW's original (SuperMarioWorld_43_GoalWipe: 0.8 s, 32 kHz), which SMM2 keeps in its story mode's
// sound bank (Sound/Resource/Quest_UndoDog.bars): read from the player's own RomFS, nothing shipped. It plays through
// the mods' mixer (smm1/smm1_audio.c); the circle is host-drawn over the game (game_overlay_image, which the 100 Mario
// flow owns: it hands its frames here while it has nothing of its own to show, smm1/flow.c).
//
// One switch in the Mods tab (on by default), kept in smm2mods.ini; an environment variable wins over the ini:
//   RECOMP_SMW_GOAL_WIPE=0/1   smw_goal_wipe=0/1
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mod.h"
#include "romfs_overlay.h"
#include "smm1/smm1_audio.h"

typedef struct { const char *label, *desc; bool *value; bool def; } GameModToggle;  // (runtime/game_hooks.c)
void mod_ini_set(const char *key, const char *val);                                 // (settings.c)

#define WIPE_SECONDS 0.8  // the circle closes in the sound's length
#define HOLD_SECONDS 0.25 // then black before SMM2's scene change goes on (SMW: a short black)
#define OPEN_SECONDS 0.3  // the black fading away over the next screen
#define PANEL_COVER 54    // game frames from the clear panel's start until its staircase wipe covers the course

// ---------------------------------------------------------------- the setting
static bool g_on = true, g_shown;
static void load_once(void) {
    const char *ini = getenv("RECOMP_MOD_INI") ? getenv("RECOMP_MOD_INI") : "smm2mods.ini";
    FILE *f = fopen(ini, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "smw_goal_wipe=", 14)) g_on = atoi(line + 14) != 0;
        fclose(f);
    }
    if (getenv("RECOMP_SMW_GOAL_WIPE")) g_on = atoi(getenv("RECOMP_SMW_GOAL_WIPE")) != 0;
    LOG("[mod] SMW goal wipe %s\n", g_on ? "on" : "off");
}
static bool wipe_on(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, load_once);
    return g_on;
}

int smw_goal_toggles(GameModToggle *out, int room) {  // (settings.c appends it to the Mods tab)
    static const GameModToggle t = {
        "SMW goal wipe",
        "Super Mario World style courses: after the goal tape's fanfare, a black circle closes on Mario with SMW's "
        "own goal-wipe sound, as at the end of SMW's levels, then the clear panel (or the next screen). Off: SMM2's "
        "own fade. Real play only, not the Course Maker's test play. Immediately.",
        &g_shown, true};
    if (room < 1) return 0;
    g_shown = wipe_on();
    out[0] = t;
    return 1;
}
void smw_goal_toggles_changed(void) {
    if (g_shown == wipe_on()) return;
    g_on = g_shown;
    mod_ini_set("smw_goal_wipe", g_on ? "1" : "0");
    LOG("[mod] SMW goal wipe %s\n", g_on ? "on" : "off");
}

// ---------------------------------------------------------------- the sound
static void play_sound(void) {
    static SPcm *p;
    static bool tried;
    if (!tried) {
        tried = true;
        RovBuf b;
        if (rov_read_game("Sound/Resource/Quest_UndoDog.bars", &b)) {
            p = smm1_bars_sound(b.d, b.n, "SuperMarioWorld_43_GoalWipe");
            rov_free(&b);
        }
        if (!p) LOG("[mod] SMW goal wipe: SuperMarioWorld_43_GoalWipe not found in Sound/Resource/Quest_UndoDog.bars\n");
    }
    smm1_play(p);
}

// ---------------------------------------------------------------- the circle
// px (w x h RGBA8): black outside the circle (cx, cy, r), clear inside, the edge antialiased over one pixel.
void smw_iris(uint8_t *px, int w, int h, float cx, float cy, float r) {
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(px + (size_t)y * (size_t)w * 4);
        float dy = (float)y + 0.5f - cy, in2 = (r - 1) * (r - 1), span = (r + 1) * (r + 1) - dy * dy;
        int x0 = w, x1 = w;  // [x0, x1): the pixels that need the distance; black outside, the clear run inside
        if (r > 0 && span > 0) {
            float half = sqrtf(span);
            x0 = (int)floorf(cx - half), x1 = (int)ceilf(cx + half);
            if (x0 < 0) x0 = 0;
            if (x1 > w) x1 = w;
        }
        for (int x = 0; x < x0; x++) row[x] = 0xff000000u;
        for (int x = x0; x < x1; x++) {
            float dx = (float)x + 0.5f - cx, d2 = dx * dx + dy * dy;
            if (r > 1 && d2 <= in2) { row[x] = 0; continue; }
            float a = sqrtf(d2) - r + 0.5f;  // (coverage of the black over this pixel)
            a = a < 0 ? 0 : a > 1 ? 1 : a;
            row[x] = (uint32_t)(a * 255 + 0.5f) << 24;
        }
        for (int x = x1; x < w; x++) row[x] = 0xff000000u;
    }
}

// ---------------------------------------------------------------- the wipe
// The game thread starts it (wipe_start) and holds SMM2's goal sequence while wipe_holding(); the renderers' thread
// draws it (smw_goal_image). Timed by the clock, not by frames: the renderers present more frames than the game runs
// with frame generation on.
enum { W_IDLE, W_CLOSING, W_BLACK, W_OPENING };
static _Atomic int g_phase = W_IDLE;
static _Atomic double g_t0;             // when the phase began (seconds)
static _Atomic float g_cx, g_cy;        // the circle's centre (1280 x 720)

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
// RECOMP_SMW_GOAL_SHOTS=<every>: (tests) a window shot every <every> presented frames for 4 s from the wipe's start,
// build/smwgoal_<frame>.png
extern char g_window_shot_request[256];  // runtime/game_hooks.c, saved by the renderer
extern uint64_t renderer_present_count(void);
static void *shots(void *arg) {
    uint64_t every = (uint64_t)(uintptr_t)arg, from = renderer_present_count();
    for (uint64_t at = from; at < from + 240; at += every) {
        while (renderer_present_count() < at) usleep(2000);
        if (!g_window_shot_request[0])
            snprintf(g_window_shot_request, sizeof g_window_shot_request, "build/smwgoal_%03llu.png", (unsigned long long)(at - from));
    }
    return NULL;
}
static void wipe_start(float cx, float cy) {
    g_cx = cx, g_cy = cy, g_t0 = now_s();
    g_phase = W_CLOSING;
    play_sound();
    LOG("[mod] SMW goal wipe: closing on (%.0f, %.0f)\n", cx, cy);
    const char *e = getenv("RECOMP_SMW_GOAL_SHOTS");
    pthread_t th;
    if (e && atoi(e) > 0 && !pthread_create(&th, NULL, shots, (void *)(uintptr_t)atoi(e))) pthread_detach(th);
}
// the circle closed and held: SMM2's goal sequence may go on (the screen stays black: until the next scene is up, or
// until the clear panel opens over it)
static bool wipe_holding(void) {
    if (g_phase != W_CLOSING) return false;
    if (now_s() - g_t0 < WIPE_SECONDS + HOLD_SECONDS) return true;
    g_t0 = now_s();
    g_phase = W_BLACK;
    return false;
}
static void wipe_open(void) {  // the next screen is up: the black fades away
    if (g_phase != W_BLACK) return;
    g_t0 = now_s();
    g_phase = W_OPENING;
}

#define IW 1280
#define IH 720
#define SCENE_MGR MAIN_ADDR(0x2a390d8) // *: the scene manager; +0x33C target scene (u32), +0x344 change wanted,
                                       //    +0x345 fader (u8); +0x344 drops back to 0 when the new scene is created
                                       //    (after SMM2's own fade out has covered the screen)
// once per presented frame, whoever draws (the 100 Mario flow's screens may cover the black): after a scene change
// the black gives way once the new scene is there
static _Atomic bool g_changing;  // the scene change was asked for (the hook) / seen under way
void smw_goal_poll(void) {
    if (g_phase != W_BLACK || !g_changing) return;
    uint64_t s = g64(SCENE_MGR);
    if (!s || !g8(s + 0x344) || now_s() - g_t0 > 10) g_changing = false, wipe_open();  // (10 s: never stuck)
}

const uint8_t *smw_goal_image(int *w, int *h, int *full, uint64_t *seq) {
    static uint8_t *px;
    static uint64_t n;
    static uint32_t shown;  // the solid colour last drawn (0: the circle)
    int phase = g_phase;
    if (phase == W_IDLE) return NULL;
    if (!px && !(px = malloc((size_t)IW * IH * 4))) return NULL;
    double t = now_s() - g_t0;
    *w = IW, *h = IH, *full = 0;
    if (phase == W_CLOSING) {
        float cx = g_cx, cy = g_cy;
        // from the farthest corner's distance (the whole screen clear) down to nothing, at a steady speed
        float fx = cx < IW / 2 ? IW - cx : cx, fy = cy < IH / 2 ? IH - cy : cy, r0 = sqrtf(fx * fx + fy * fy) + 2;
        float k = t >= WIPE_SECONDS ? 0 : 1 - (float)(t / WIPE_SECONDS);
        smw_iris(px, IW, IH, cx, cy, r0 * k);
        shown = 0, *seq = ++n;
        return px;
    }
    float a = 1;
    if (phase == W_OPENING) {
        if (t >= OPEN_SECONDS) { g_phase = W_IDLE; return NULL; }
        a = 1 - (float)(t / OPEN_SECONDS);
    }
    uint32_t c = (uint32_t)(a * 255 + 0.5f) << 24, *p = (uint32_t *)px;
    if (c != shown) {
        for (size_t i = 0; i < (size_t)IW * IH; i++) p[i] = c;
        shown = c, n++;
    }
    *full = phase == W_BLACK, *seq = n;
    return px;
}

// ---------------------------------------------------------------- the hook
// The goal tape's demo (MariowGoal) runs its Fade state's execute through this thunk, once a frame, and only for that
// goal (each goal type has its own; the castle's axe never comes here). The Fade waits (it returns at once) while a
// fade is under way or while the demo's counter runs (the fanfare, then 35 frames). Then, in Course World, Story,
// Endless..., it opens the clear panel and asks the scene manager for the scene change once the panel is done (Next);
// with no panel (My Courses) it asks at once. The override starts the circle on the first frame the Fade would go on,
// and does not run the Fade until the circle is shut (a skipped frame changes nothing: the Fade only waits). Then the
// Fade runs: a scene change asked for keeps the screen black until the new scene is up; otherwise the panel is opening
// and the black fades away once the panel's own wipe (a staircase, then COURSE CLEAR!) covers the course. Addresses
// v3.0.3; the research is in research/specs/smw-goal-wipe.md (private).
#define MARIOW_FADE_EXEC 0x12d28a0     // MariowGoal demo: Fade execute (x0: the demo controller; +0x64 the wait counter)
#define FADES MAIN_ADDR(0x2c16348)     // *: +0x2C0 > 0 / +0x240 != 0: a fade under way (the Fade waits)
#define PLAY_INFO MAIN_ADDR(0x2c57d58) // *(*+0x30): +0x10 mode (2: play), +0x14 sub-mode, +0x1C course style (2: SMW)
#define CAMERAS MAIN_ADDR(0x2c55080)   // *: +0x7D split screen, +0x88 the view (single screen)
#define DEMO_PLAYER 0xC0               // the controller's player: the one who broke the tape

static bool on_heap(uint64_t a) { return a >= HEAP_BASE && a < HEAP_BASE + HEAP_MAX - 0x1000 && !(a & 7); }
static float g32f(uint64_t a) { float f; uint32_t w = g32(a); memcpy(&f, &w, 4); return f; }

// real play of an SMW course: Course World, My Courses, Story, Endless... (mode 2), not online versus / multi-clear
// (sub-modes 0xC / 0xD) nor the Course Maker's test play (mode 0)
static bool eligible(void) {
    uint64_t g = g64(PLAY_INFO), pi = g ? g64(g + 0x30) : 0;
    if (!pi) return false;
    uint32_t mode = g32(pi + 0x10), sub = g32(pi + 0x14);
    return g32(pi + 0x1c) == 2 && mode == 2 && sub != 0xc && sub != 0xd;
}
// this frame the Fade goes on (the same waits it checks first)
static bool fade_goes_on(uint64_t ctl) {
    uint64_t f = g64(FADES);
    return f && (int32_t)g32(f + 0x2c0) <= 0 && g32(f + 0x240) == 0 && on_heap(ctl) && g32(ctl + 0x64) == 0 && g64(PLAY_INFO);
}

// where Mario's body is on the 1280 x 720 screen (the view rect is world px, y up); the screen centre if unsure
static void mario_on_screen(uint64_t ctl, float *sx, float *sy) {
    *sx = IW / 2, *sy = IH / 2;
    uint64_t pl = on_heap(ctl) ? g64(ctl + DEMO_PLAYER) : 0, h = g64(CAMERAS);
    if (!on_heap(pl) || !h || g8(h + 0x7d)) return;
    uint64_t v = g64(h + 0x88);
    if (!v) return;
    float l = g32f(v + 0xc), b = g32f(v + 0x10), r = g32f(v + 0x14), t = g32f(v + 0x18);
    uint32_t form = g32(pl + 0x4a8);
    bool small = form < 16 && g32(MAIN_ADDR(0x23d5e0c) + 4ull * form) == 1;
    float x = g32f(pl + 0x230), y = g32f(pl + 0x234) + (small ? 8 : 16);
    if (!(r - l > 1) || !(t - b > 1)) return;  // (also NaN)
    float px = (x - l) * IW / (r - l), py = (t - y) * IH / (t - b);
    if (px < -IW || px > 2 * IW || py < -IH || py > 2 * IH) return;
    *sx = px, *sy = py;
}

static void mariow_fade_exec(Cpu *c) {
    static uint64_t done;  // the controller whose wipe has run (the Fade goes on running until the scene changes)
    static int after;      // frames the Fade has run since the circle shut
    uint64_t ctl = c->x[0], s = g64(SCENE_MGR);
    if (on_heap(ctl) && g32(ctl + 0x64)) done = 0;  // (a goal's fanfare still on: this goal's wipe is to come)
    if (done != ctl && g_phase == W_IDLE && wipe_on() && fade_goes_on(ctl) && eligible()) {
        float x, y;
        mario_on_screen(ctl, &x, &y);
        wipe_start(x, y);
        done = ctl, after = 0, g_changing = false;
    }
    if (wipe_holding()) return;  // (the Fade waits with the circle)
    uint8_t wanted = s ? g8(s + 0x344) : 1;
    guest_call_orig(c, MAIN_ADDR(MARIOW_FADE_EXEC));
    if (g_phase != W_BLACK || g_changing) return;
    if (!wanted && s && g8(s + 0x344)) {
        g_changing = true;
        LOG("[mod] SMW goal wipe: scene change 0x%x under the black\n", g32(s + 0x33c));
    } else if (++after >= PANEL_COVER) {  // (no scene change asked for: the clear panel, its wipe over the course now)
        LOG("[mod] SMW goal wipe: into the clear panel\n");
        wipe_open();
    }
}

__attribute__((constructor)) static void smw_goal_register(void) {
    hle_override(MAIN_ADDR(MARIOW_FADE_EXEC), mariow_fade_exec, "SMW goal wipe: goal tape fade");
}
