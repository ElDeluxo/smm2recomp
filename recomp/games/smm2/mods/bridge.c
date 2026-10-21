// The run side of the 100 Mario bridge (hundred.h): SMM1's screens on top, SMM2's own course play underneath.
//
// How a course starts without SMM2's Endless screens: they are still there, hidden under the flow's screens, and this
// file drives them. SMM2's Endless machinery is what makes a server course playable in SMM2's own play (the run
// manager, the course list and download jobs, the CRC and validity checks, the course-in card, the HUD with the
// lives and the three banked 1-Ups, the skip, the retry after a miss, the goal), and the mod already answers all of its
// requests from our server (endless.c). So:
//   - the Endless menu's page is caught each frame in its Disp state (its `this`), and the difficulty is chosen by
//     calling the game's own tab handler for it on the game thread;
//   - New Game / Continue / Play / skip / back are the menu's own buttons, pressed through the test-input hook (the
//     player's own input goes to the flow while it is active);
//   - SMM2's pause during a run course stays closed: the play is held in the pause state while the flow shows SMM1's
//     pause, and its choice (resume, Start Over, Save & Quit) leaves that state the way the pause menu's items do;
//   - what happens in the course comes back through the requests the game makes (course start, miss, clear, skip,
//     suspend) and goes to the flow as hundred_flow_course_started() / hundred_course_result().
//
// RECOMP_MOD_AUTOPLAY=1 (=jump) stands in for the player in unattended test runs: it plays the courses (right + run
// [+ jump], skip after RECOMP_MOD_AUTOPLAY_SKIP s) and presses the result screen's Next; RECOMP_MOD_FLOW_TEST=
// <difficulty> makes the stand-in flow below start a run and play it to its end (the evidence runs).
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hundred.h"
#include "mod.h"
#include "../../../runtime/net/json.h"

extern uint64_t g_test_buttons, g_test_buttons_until;  // runtime/game_hooks.c (weak), read by runtime/hle/input.c
extern uint64_t renderer_present_count(void);
void endless_fill_status(struct hundred_status *out, int active_d);  // endless.c
void endless_forget_run(int d);
void endless_prepare_run(int d, bool resume);

enum { BTN_A = 1 << 0, BTN_B = 1 << 1, BTN_Y = 1 << 3, BTN_L = 1 << 6, BTN_R = 1 << 7, BTN_PLUS = 1 << 10, BTN_MINUS = 1 << 11, BTN_LEFT = 1 << 12,
       BTN_UP = 1 << 13, BTN_RIGHT = 1 << 14, BTN_DOWN = 1 << 15 };

// ---- driver: one request at a time, pressed on its own thread
enum { DRV_IDLE, DRV_NEW_GAME, DRV_CONTINUE, DRV_PLAY, DRV_SKIP, DRV_AFTER_CLEAR, DRV_RESULT_QUIT, DRV_BACK,
       DRV_SAVE_QUIT, DRV_COURSE, DRV_PAUSE_TEST, DRV_BROKEN };
static volatile int drv = DRV_IDLE;
static volatile uint64_t drv_since, course_since;
static volatile int run_d = -1;          // the flow's run (difficulty), -1 none
static volatile int pending_tab = -1;    // a difficulty to select on the game thread (the tab's handler)
static volatile int broken_d = -1;       // a corrupt course left the run of this difficulty: back to it
static volatile uint64_t broken_tap;     // the Challenge Modes button was tapped (after a corrupt course) then
static volatile int tab_target = -1;     // a difficulty to select with L / R
static volatile int tab_presses;
static volatile uint64_t menu_page, menu_seen;
static volatile uint64_t back_since, disappear_seen;  // B to Course World asked (hundred_flow_exit); the menu's
                                                      // Disappear last ran  // the Endless menu page and the frame it was last in Disp
static volatile uint64_t opening_since;  // the flow opened as the menu was asked for (seq_activate; presented frame,
                                         // 0 none) and the menu hasn't reached its Disp yet
static volatile bool play_queued;      // Play asked for while the result screen is up
static volatile bool suspending;       // Save & Quit under way: the menu's return reports it
static volatile bool suspend_posted;   // the game posted its suspend (the confirm window was answered)
static volatile bool leaving_won;      // a won run's last result screen: Quit back to the menu, nothing to report
static volatile bool awaiting_next;    // after a clear: the player is on SMM2's result screen (Next or Quit to come)
static volatile bool run_won;          // the last clear completed the run
static volatile uint64_t next_menu_since;  // the menu came back after a clear without the map walk (Quit?)
static volatile bool over_save;        // New Game over a saved run (the menu shows Continue too)
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void set_drv(int d) { drv = d, drv_since = renderer_present_count(); }
static int test_difficulty(void);
static void mod_shot(const char *what, uint64_t after_frames);
static void result(int outcome);
static volatile bool started_pending;    // a course start / retry not yet seen playing
static volatile uint64_t started_since;
static void course_started(const char *why);
static void course_scene(const char *why);
static void switch_sample(uint64_t *prev_b, bool *prev_touch, uint64_t now);
static void mod_burst(const char *what, uint64_t frames);
extern uint64_t input_player_buttons(void);  // runtime/hle/input.c: keyboard, controllers, on-screen pad

// (flow test) RECOMP_MOD_FLOW_TEST_RESULT_KEYS=<letters>: on the result screen, before the Next presses, one key
// every 50 frames from frame RECOMP_MOD_FLOW_TEST_RESULT_AT (600) after the clear, with a shot after each:
// U D L R A B X Y, l r (L / R), z Z (ZL / ZR); h j k l the left stick (left, down, up, right); t a tap at
// RECOMP_MOD_FLOW_TEST_TAP (x/y in game pixels, Quit's 603/530 by default); < > ^ v K real arrow / K key events.
// True while the keys go on.
static bool result_keys(uint64_t t, uint64_t now, uint64_t *b, uint64_t *nk, uint64_t *k0) {
    const char *keys = getenv("RECOMP_MOD_FLOW_TEST_RESULT_KEYS");
    const char *at = getenv("RECOMP_MOD_FLOW_TEST_RESULT_AT");
    *nk = keys ? strlen(keys) : 0, *k0 = at ? strtoull(at, NULL, 0) : 600;
    if (!*nk) return false;
    if (t < *k0) return true;
    if (t >= *k0 + 50 * *nk) return t < *k0 + 50 * *nk + 60;
    uint64_t i = (t - *k0) / 50, in = (t - *k0) % 50;
    char key = keys[i];
    static bool tap_down;
    if (in < 8) {
        static const char letters[] = "UDLRABXYlrzZ";
        static const uint64_t bits[] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B, 1 << 2, BTN_Y,
                                        BTN_L,  BTN_R,    1 << 8,   1 << 9};
        const char *p = strchr(letters, key);
        if (p) *b = bits[p - letters];
        extern int32_t g_test_lx, g_test_ly;  // runtime/hle/input.c
        extern uint64_t g_test_stick_until;
        static const char sticks[] = "hjkl";
        static const int32_t sx[] = {-32767, 0, 0, 32767}, sy[] = {0, -32767, 32767, 0};
        const char *sp = strchr(sticks, key);
        if (sp) g_test_lx = sx[sp - sticks], g_test_ly = sy[sp - sticks], g_test_stick_until = now + 6;
    }
    extern void touch_test(int32_t x, int32_t y, bool down);  // runtime/hle/touch.c
    int tx = 603, ty = 530;
    const char *tp = getenv("RECOMP_MOD_FLOW_TEST_TAP");
    if (tp) sscanf(tp, "%d/%d", &tx, &ty);
    if (key == 't' && in < 8 && !tap_down) tap_down = true, touch_test(tx, ty, true);
    else if (tap_down && in >= 8) tap_down = false, touch_test(tx, ty, false);
    // < > ^ v and K: real key events (the player's arrows and A key, through the host input path)
    extern void input_test_key(const char *name, bool down);  // runtime/hle/input.c
    static const char real[] = "<>^vK[]";  // ([ ] the ZL / ZR keys)
    static const char *const real_names[] = {"Left", "Right", "Up", "Down", "K", "1", "3"};
    static const char *key_down;
    const char *rp = strchr(real, key);
    if (rp && in < 8 && !key_down) key_down = real_names[rp - real], input_test_key(key_down, true);
    else if (key_down && in >= 8) input_test_key(key_down, false), key_down = NULL;
    static uint64_t shot_for;  // (one shot and one log line per key)
    if (shot_for != drv_since + i + 1) {
        shot_for = drv_since + i + 1, mod_shot("key", 40);
        // (the inputs of the comment button [main+0x1be7d30] and of the ratings [main+0x1be6d30]: the course's
        // +0x570 / +0x860, the comment count +0x7896c against the two allowances; the rating's can / why)
        uint64_t ci = g64(MAIN_ADDR(0x2c4b710)), ui = g64(MAIN_ADDR(0x2c115e0));
        LOG("[mod] result key '%c': course +0x570 %u +0x860 %u count %d, comment can %u map %u/%u, rating can %u "
            "why %u\n", key, g8(ci + 0x570), g8(ci + 0x860), (int)g32(ci + 0x7896c), g8(ui + 0x1b8958),
            g8(ui + 0x1b8960), g8(ui + 0x1b8961), g8(ui + 0x1b9c48), g32(ui + 0x1b9c4c));
    }
    return true;
}

// (test) RECOMP_MOD_TEST_SWITCH=<letters>: Challenge Modes switches as a player makes them, through the host's input
// path (both sides read it): from 5 s after the Endless menu first shows, one letter every 36 frames: z ZL, Z ZR (the
// 1 / 3 keys, held 6 frames), t a tap on the mode header (400/40), . nothing.
static void switch_test(uint64_t now) {
    static const char *script;
    static bool read;
    static uint64_t start;
    static int i = -1;
    static const char *key_down;
    static bool tap_down;
    if (!read) read = true, script = getenv("RECOMP_MOD_TEST_SWITCH");
    if (!script || !menu_seen) return;
    if (!start) start = now + 300;
    if (now < start) return;
    uint64_t k = (now - start) / 36, in = (now - start) % 36;
    extern void input_test_key(const char *name, bool down);  // runtime/hle/input.c
    extern void touch_test(int32_t x, int32_t y, bool down);  // runtime/hle/touch.c
    if (in >= 6) {
        if (key_down) input_test_key(key_down, false), key_down = NULL;
        if (tap_down) touch_test(400, 40, false), tap_down = false;
        return;
    }
    if ((int)k == i || k >= strlen(script)) return;
    i = (int)k;
    char c = script[k];
    if (c == '.') return;
    LOG("[mod] switch test: '%c' (%s)\n", c, mod_smm2_session() ? "on SMM2's Endless" : "on 100 Mario");
    mod_shot("switch", 30);
    if (c == 'z' || c == 'Z') input_test_key(key_down = c == 'z' ? "1" : "3", true);
    else if (c == 't') touch_test(400, 40, true), tap_down = true;
}

static void *driver(void *arg) {
    (void)arg;
    const char *ap = getenv("RECOMP_MOD_AUTOPLAY");
    bool jump = ap && !strcmp(ap, "jump");
    uint64_t skip_after =
        (getenv("RECOMP_MOD_AUTOPLAY_SKIP") ? strtoull(getenv("RECOMP_MOD_AUTOPLAY_SKIP"), NULL, 0) : 20) * 60;
    uint64_t sw_prev_b = 0;
    bool sw_prev_touch = false;
    for (;;) {
        usleep(20000);
        uint64_t now = renderer_present_count(), t = now - drv_since, b = 0;
        if (mod_hundred_offered()) switch_sample(&sw_prev_b, &sw_prev_touch, now);
        switch_test(now);
        if ((drv == DRV_NEW_GAME || drv == DRV_CONTINUE) && tab_target >= 0 && now - menu_seen > 10) {
            // (not on SMM2's Endless menu yet, e.g. still leaving the last run: wait for it)
            static uint64_t logged;
            if (now - logged > 600) logged = now, LOG("[mod] bridge: waiting for SMM2's Endless menu\n");
            drv_since = now;
            g_test_buttons = 0, g_test_buttons_until = 0;
            continue;
        }
        if ((drv == DRV_NEW_GAME || drv == DRV_CONTINUE) && tab_target >= 0) {
            // the difficulty, the way a player picks it: L / R until the menu's difficulty (main+0x2c3f618) is the
            // one wanted (calling the tab's handler directly can leave the buttons without a focus when Continue
            // goes away); after 12 presses, the handler (menu_disp)
            int cur = (int)g32(MAIN_ADDR(0x2c3f618));
            if (cur == tab_target) {
                MLOG("bridge: difficulty %d on the menu\n", tab_target);
                tab_target = -1, tab_presses = 0, drv_since = now;
                mod_shot("tab", 60), mod_shot("tab", 95);
            } else if (tab_presses >= 12) {
                pending_tab = tab_target, tab_target = -1, tab_presses = 0;
            } else if (t >= 30) {
                b = cur < tab_target ? BTN_R : BTN_L;
                if (t >= 38) tab_presses++, drv_since = now;
            }
            g_test_buttons = b;
            g_test_buttons_until = b ? now + 4 : 0;
            continue;
        }
        switch (drv) {
        case DRV_NEW_GAME:  // (the tab is set first, on the game thread) New Game, then the confirm window's Yes.
                            // With a saved run the focus starts on Continue, right of New Game [obs]
            if (pending_tab >= 0) { drv_since = now; break; }
            // "Previous save data will be overwritten. Continue?" opens on No, Yes is right of it [obs, flow agent]:
            // Right then A, repeated in case the window comes late (on the map Right does nothing and A is Play)
            // (after L / R the focus is on the tab bar [obs]: Down brings it to the buttons; twice, in case the
            // first falls into the tab change)
            if ((t >= 40 && t < 48) || (t >= 70 && t < 78)) b = BTN_DOWN;
            else if (over_save && t >= 100 && t < 108) b = BTN_LEFT;
            else if (t >= 130 && t < 138) b = BTN_A;
            else if (over_save && t >= 300 && t < 780) {
                if ((t - 300) % 120 < 8) b = BTN_RIGHT;
                else if ((t - 300) % 120 >= 40 && (t - 300) % 120 < 48) b = BTN_A;
            } else if (!over_save && t >= 420 && t < 428) b = BTN_A;
            else if (t > (over_save ? 780u : 440u)) set_drv(DRV_PLAY);
            break;
        case DRV_CONTINUE:  // (the tab is set first) Continue: the focused button when a run is saved [obs]
            if (pending_tab >= 0) { drv_since = now; break; }
            if ((t >= 40 && t < 48) || (t >= 70 && t < 78)) b = BTN_DOWN;
            else if (t >= 130 && t < 138) b = BTN_A;
            else if (t > 160) set_drv(DRV_PLAY);
            break;
        case DRV_PLAY:  // the map's Play, every second until the course starts (presses during the map's walk /
                        // telop are ignored by the map)
            if (t > 30 && t % 60 < 8) b = BTN_A;
            break;
        case DRV_COURSE:
            if (!ap) break;
            if (now - course_since > skip_after) {
                if ((now - course_since - skip_after) % 240 < 200) b = BTN_MINUS;  // (pulsed: see the retry card)
            } else if (t > 120) {
                b = BTN_RIGHT | BTN_Y;
                if (jump && t % 90 < 20) b |= BTN_A;
            }
            break;
        case DRV_BROKEN: {  // "The course data is corrupt." on the map: its OK leaves for Course World's menu, the
                            // focus on Courses [obs]. Back in: a tap on Challenge Modes every 4 s from 4 s after
                            // the OK while the Endless menu isn't up, then (menu_disp) Continue and Play: the run
                            // goes on without the course. (A minute at most.)
            extern void touch_test(int32_t x, int32_t y, bool down);  // runtime/hle/touch.c
            static bool down;
            if (t >= 60 && t < 68) b = BTN_A;
            else if (t >= 300 && (t - 300) % 240 < 8 && now - menu_seen > 60 && !down)
                down = true, broken_tap = now, touch_test(864, 466, true);  // (the button's middle, 1280x720)
            else if (down && (t - 300) % 240 >= 8) down = false, touch_test(864, 466, false);
            else if (t > 3600) {
                LOG("[mod] bridge: no way back from the corrupt course\n");
                set_drv(DRV_IDLE);
            }
            break;
        }
        case DRV_PAUSE_TEST:  // (flow test) + in the course: SMM2's pause, which the mod keeps closed
            if (t < 8) b = BTN_PLUS;
            else if (t > 30) set_drv(DRV_COURSE);
            break;
        case DRV_SKIP:  // SMM1's skip, free: hold - (pulsed: a hold begun during a transition doesn't count)
            if (t % 240 < 200) b = BTN_MINUS;
            break;
        case DRV_AFTER_CLEAR: {  // the result screen (Boo / I like it / Quit / Next): Next, then back on the map
            uint64_t nk = 0, k0 = 0;
            if (result_keys(t, now, &b, &nk, &k0)) break;
            if (nk) t = t - (k0 + 50 * nk + 60) + 480;
            // A (Next has the focus [obs]) every 2 s from 8 s on until the game leaves the result screen (cNextScene),
            // up to a minute (the screen may come late when the machine is busy)
            if (awaiting_next && t >= 480 && t < 4080) {
                if ((t - 480) % 120 < 8) b = BTN_A;
            } else if (t >= 480) {
                set_drv(play_queued ? DRV_PLAY : DRV_IDLE);
                play_queued = false;
            }
            break;
        }
        case DRV_RESULT_QUIT: {  // (flow test) the result screen's Quit, tapped: the D-pad doesn't move the focus off
                                 // Next there [obs]; Quit goes straight back to the menu (no window [obs])
            extern void touch_test(int32_t x, int32_t y, bool down);  // runtime/hle/touch.c
            static bool down;
            if (t >= 600 && t < 610 && !down) down = true, touch_test(603, 530, true);
            else if (t >= 610 && down) down = false, touch_test(603, 530, false);
            else if (t > 1300) set_drv(DRV_IDLE);
            break;
        }
        case DRV_BACK: {  // B on the Endless menu: back to Course World, once the player's own B is released;
                          // again every 1.5 s while the menu is still there (at most 4 times)
            bool on_menu = now - menu_seen <= 2;
            if ((!on_menu && t > 30) || t >= 4 * 90) set_drv(DRV_IDLE);
            else if (input_player_buttons() & BTN_B) drv_since = now;  // (wait for the release)
            else if (t % 90 >= 10 && t % 90 < 18 && on_menu) b = BTN_B;
            break;
        }
        case DRV_SAVE_QUIT:  // the map's Quit (+), then the window's Yes (Right + A until the suspend is posted)
            if (t < 8) b = BTN_PLUS;
            else if (t >= 60 && t < 660 && !suspend_posted) {
                if ((t - 60) % 120 < 8) b = BTN_RIGHT;
                else if ((t - 60) % 120 >= 40 && (t - 60) % 120 < 48) b = BTN_A;
            } else if (t >= 660 || suspend_posted) set_drv(DRV_IDLE);
            break;
        default: break;
        }
        if (started_pending && now - started_since > 600 && hundred_flow_ready_to_start()) course_started("fallback");
        if (awaiting_next && next_menu_since && now - next_menu_since > 300) {
            // back on the menu after a clear and no map walk: Quit on the result screen, the run is kept
            awaiting_next = false, next_menu_since = 0;
            int d = run_d;
            run_d = -1;
            LOG("[mod] bridge: Quit on the result screen (difficulty %d): the run is kept\n", d);
            result(HUNDRED_GIVEUP_SAVE);
        }
        g_test_buttons = b;
        g_test_buttons_until = b ? now + 4 : 0;
    }
    return NULL;
}

static void test_result(int outcome);
static void result(int outcome) {
    if (test_difficulty() >= 0) test_result(outcome);
    else hundred_course_result(outcome);
}

static void driver_start(void) {
    pthread_t th;
    pthread_create(&th, NULL, driver, NULL);
    pthread_detach(th);
}

// ---- the Endless menu page, caught in its Disp state (v3.0.3: page state machine registered at main+0x1a23f50)
#define MENU_DISP_EXEC MAIN_ADDR(0x1a24bf0)
#define MENU_TAB0 MAIN_ADDR(0x1a22260)  // the four tab handlers, 0xc0 apart (Easy .. Super Expert)

// Challenge Modes on SMM2's side: on SMM2's Endless menu, ZL / ZR or a tap on its header ("ZL < Endless Challenge >
// ZR", relabel.c; the header's band at the top left) switch to the 100 Mario Challenge. A press is caught both on the
// driver thread (50 Hz) and on the menu's Disp, and is good for a second: the Disp doesn't run while the menu changes
// difficulty or redraws its map (as right after each switch), and a press then used to be lost.
extern bool touch_first(int32_t *x, int32_t *y);  // runtime/hle/touch.c
static volatile uint64_t switch_press_at;  // presented frame of the last ZL / ZR press or header touch (0: none)
static volatile bool switch_blocked;       // after a switch: nothing counts until ZL, ZR and the touch are let go
static void switch_sample(uint64_t *prev_b, bool *prev_touch, uint64_t now) {
    uint64_t b = input_player_buttons() & (1u << 8 | 1u << 9);  // ZL, ZR
    int32_t x = 0, y = 0;
    bool down = touch_first(&x, &y), touch = down && x >= 90 && x < 760 && y < 90;
    if (switch_blocked) {
        if (!b && !down) switch_blocked = false;
    } else if ((b & ~*prev_b) || (touch && !*prev_touch)) {
        switch_press_at = now ? now : 1;
    }
    *prev_b = b, *prev_touch = touch;
}
static bool mode_switch_pressed(uint64_t now) {
    static uint64_t prev_b;
    static bool prev_touch;
    switch_sample(&prev_b, &prev_touch, now);
    uint64_t at = switch_press_at;
    if (!at) return false;
    switch_press_at = 0;
    return now - at < 60;
}

// choose the difficulty asked for (pending_tab) the way the tab buttons do, on the menu page's Disp
static void select_pending_tab(Cpu *c, uint64_t page) {
    if (pending_tab < 0) return;
    int d = pending_tab;
    uint64_t save[8];
    memcpy(save, c->x, sizeof save);
    c->x[0] = g64(page + 0x48);  // (the tab buttons' handlers get the page's owner [obs: their this = page+0x48])
    // (a handler does nothing for the difficulty already shown (main+0x2c3f618): another one first, so the page
    // redraws the run as the records say now [inf, main+0x1a22260])
    s32(MAIN_ADDR(0x2c3f618), (uint32_t)(d + 1) % 4);
    if (c->x[0]) guest_call(c, MENU_TAB0 + 0xc0ull * (uint64_t)d);
    memcpy(c->x, save, sizeof save);
    pending_tab = -1;
    MLOG("bridge: difficulty %d selected\n", d);
}

static void menu_disp(Cpu *c) {
    pthread_once(&once, driver_start);  // (its thread also catches the Challenge Modes presses)
    uint64_t now0 = renderer_present_count();
    bool opened = opening_since;  // (the flow opened as the menu was asked for: not again, even if B closed it)
    opening_since = 0;
    if (drv == DRV_BROKEN && broken_d >= 0 && broken_tap > drv_since) {  // (back after a corrupt course)
        LOG("[mod] bridge: back on the Endless menu after the corrupt course: Continue\n");
        tab_target = broken_d, broken_d = -1, over_save = false;
        set_drv(DRV_CONTINUE);
    }
    // (re)entering the menu with no run going: Challenge Modes opens on the mode used last
    if (mod_hundred_offered() && test_difficulty() < 0 && run_d < 0 && now0 - menu_seen > 30 && g_mod.mode_endless &&
        !mod_smm2_session() && !hundred_flow_active() && !leaving_won && !suspending)
        mod_set_smm2_session(true);
    // (SMM2's own Endless Challenge: 100 Mario not offered, or Challenge Modes on Endless)
    if ((!mod_hundred_offered() || mod_smm2_session()) && test_difficulty() < 0) {
        bool settled = now0 - menu_seen <= 30;
        menu_seen = now0;
        select_pending_tab(c, c->x[0]);  // (after a switch: the page shows the records as this mode sees them)
        guest_call_orig(c, MENU_DISP_EXEC);
        if (mod_hundred_offered() && settled && mode_switch_pressed(now0)) {
            LOG("[mod] bridge: Challenge Modes: to the 100 Mario Challenge\n");
            switch_blocked = true;
            mod_remember_mode(false);
            mod_set_smm2_session(false);
            hundred_flow_open();
        }
        return;
    }
    uint64_t page = c->x[0], now = now0;
    bool back = now - menu_seen > 30;  // (first Disp frame after a while: the menu is (back) on screen)
    menu_page = page, menu_seen = now;
    select_pending_tab(c, page);
    guest_call_orig(c, MENU_DISP_EXEC);
    if (back && leaving_won) {  // a won run: SMM2 is back on its menu, ready for the next one
        leaving_won = suspending = false;
        set_drv(DRV_IDLE);
        LOG("[mod] bridge: back on SMM2's Endless menu after the win\n");
        return;
    }
    if (back && awaiting_next && !suspending) next_menu_since = now;  // (Next walks Mario on the map; Quit doesn't)
    if (back && suspending) {  // Save & Quit done: the game is back on its Endless menu
        suspending = false, awaiting_next = false;
        int d = run_d;
        run_d = -1;
        set_drv(DRV_IDLE);
        LOG("[mod] bridge: Save & Quit done (difficulty %d)\n", d);
        result(HUNDRED_GIVEUP_SAVE);
        return;
    }
    if (!back || run_d >= 0) return;
    if (test_difficulty() >= 0) {
        LOG("[mod] bridge: Endless menu entered: flow test run\n");
        const char *cont = getenv("RECOMP_MOD_FLOW_TEST_CONTINUE");
        hundred_run_start(test_difficulty(), cont && atoi(cont));
    } else if (!opened && !hundred_flow_active()) {  // (normally it opened already, seq_activate below)
        LOG("[mod] bridge: Endless menu entered: the 100 Mario flow opens\n");
        hundred_flow_open();
    }
}

// ---- Challenge Modes chosen on Course World: the 100 Mario select comes instead of SMM2's Endless menu
// Course World's Endless sub-sequence (NetworkEndlessModeSeq, registered at main+0x1b20910) leaves its Idle for
// Activate (index 2, exec main+0x1a22730) once Course World has the run records (its status download: the spinner).
// Activate's first frame activates the menu page; the next ones wait for the page and go on to Appear, which brings
// the menu in (its In) [obs research/decomp/menus/v303/endlessseq; frames 4852 / 4856 / 4882 for Activate, the
// menu's Appear and its first Disp]. In the 100 Mario mode the flow opens on that first frame and Activate is held
// until the select has faded in over Course World, so SMM2's menu comes in under it unseen. (The flow used to open
// on the menu's first Disp, and SMM2's Endless menu showed for half a second before the select.) A run's Save & Quit
// comes back without Activate (the sequence is in Appear at once, its menu up under SMM2's fade from black [obs]):
// menu_disp reports it there and the select fades in over that menu, as before.
#define SEQ_ACTIVATE_EXEC MAIN_ADDR(0x1a22730)
__attribute__((weak)) bool hundred_flow_covering(void) { return true; }

static void seq_activate(Cpu *c) {
    uint64_t now = renderer_present_count();
    if (g32(c->x[0] + 0x244) == 0) {
        // (menu_disp's conditions for opening: no run going or coming back, Challenge Modes on 100 Mario)
        if (mod_hundred_offered() && test_difficulty() < 0 && run_d < 0 && !g_mod.mode_endless && !mod_smm2_session() &&
            !hundred_flow_active() && !leaving_won && !suspending) {
            LOG("[mod] bridge: Challenge Modes chosen: the 100 Mario flow opens\n");
            opening_since = now ? now : 1;
            hundred_flow_open();
        }
    } else if (opening_since && !hundred_flow_covering() && now - opening_since < 40) {
        return;  // (the select is still fading in: the menu waits; at most 40 frames, e.g. a switch to Endless)
    }
    guest_call_orig(c, SEQ_ACTIVATE_EXEC);
}

// ---- back from a run's Save & Quit (from the pause, or Quit on the result screen): black until the select
// SMM2's scene changes go through its resident fade (ResidentSeq/UISceneFadeSeq): Out (enter main+0x192f1a0, exec
// main+0x192f470: the Out animation, 30 frames for the plain black one), then a state that only waits, the screen
// black, while the next scene loads (enter main+0x192f580), then In (main+0x192f5a0) [obs research/decomp (the
// fade's state functions); frames 8730 / 8761 / 8771 for Out, black and In on the way back after a Quit]. The way
// back to the Endless menu skips Activate (above): the menu is up under SMM2's fade from black, and the select used
// to come over it ~0.3 s later. So on that black the flow's own black comes up and stays, and the select comes out
// of it when the menu reports the run (menu_disp: HUNDRED_GIVEUP_SAVE). (A won run goes back under the flow's
// ending; a game over goes through SMM2's GAME OVER scene to Course World, not the menu [obs]: neither shows it.)
#define FADE_OUT_ENTER MAIN_ADDR(0x192f1a0)
#define FADE_BLACK_ENTER MAIN_ADDR(0x192f580)
__attribute__((weak)) void hundred_flow_cover(void) {}
static volatile uint64_t fade_page;  // the fade's page (its Out's this): the black state's enter is a bare return

static void fade_out(Cpu *c) {
    fade_page = c->x[0];
    guest_call_orig(c, FADE_OUT_ENTER);
}
static void fade_black(Cpu *c) {
    guest_call_orig(c, FADE_BLACK_ENTER);
    if (c->x[0] != fade_page || test_difficulty() >= 0 || !mod_hundred_rules() || leaving_won) return;
    if (suspending && run_d >= 0) {
        LOG("[mod] bridge: SMM2 goes back to its Endless menu: the flow keeps the screen black\n");
        hundred_flow_cover();
    }
}

// ---- the course's start with SMM2's course-in card (the "SMM1 course card" setting off)
// The Endless menu's CourseIn state (index 0xb of its machine; exec main+0x1a258f0) posts 0x80 (to the course) every
// frame once the map Mario's Decide animation is over [obs research/decomp/mods/menu]. The flow's map and iris come
// first: the state is held (its exec not run) until the flow is ready (hundred_flow_ready_to_start), so SMM2 changes
// scene under the flow's black and its card shows whole; the course scene's first state tells the flow
// (hundred_flow_course_in), which lets go of the black. The play scene's two download states (cCourseListDownLoad,
// enter main+0x1bbb820; cCourseDataDownLoad, enter main+0x1bbb9a0) and the course start request are where the course
// scene shows first [obs research/specs/endless-challenge.md 4.3].
#define MENU_COURSEIN_EXEC MAIN_ADDR(0x1a258f0)
#define PLAY_LIST_DOWNLOAD_ENTER MAIN_ADDR(0x1bbb820)
#define PLAY_DATA_DOWNLOAD_ENTER MAIN_ADDR(0x1bbb9a0)
static volatile bool course_scene_due;  // the CourseIn was let go: the next course scene goes to the flow
__attribute__((weak)) void hundred_flow_course_in(void) {}

static void menu_course_in(Cpu *c) {
    if (run_d >= 0 && mod_hundred_rules() && test_difficulty() < 0 && !hundred_smm1_card()) {
        static bool held;
        if (!hundred_flow_ready_to_start()) {
            if (!held) held = true, LOG("[mod] bridge: Play pressed on SMM2's map: it waits for the flow's map\n");
            return;
        }
        if (held || !course_scene_due) {
            held = false, course_scene_due = true;
            LOG("[mod] bridge: SMM2 leaves its map for the course\n");
            mod_burst("courseIn", 480);
        }
    }
    guest_call_orig(c, MENU_COURSEIN_EXEC);
}
static void course_scene(const char *why) {
    if (!course_scene_due) return;
    course_scene_due = false;
    LOG("[mod] bridge: SMM2's course scene (%s): its card comes\n", why);
    if (test_difficulty() < 0) hundred_flow_course_in();
}
static void play_list_download(Cpu *c) {
    course_scene("course list");
    guest_call_orig(c, PLAY_LIST_DOWNLOAD_ENTER);
}
static void play_data_download(Cpu *c) {
    course_scene("course data");
    guest_call_orig(c, PLAY_DATA_DOWNLOAD_ENTER);
}

// ---- SMM2's pause during a run course -> SMM1's pause (the flow's)
// The Endless play scene's pause state (cPausePlay, index 3 of its machine at scene+0x238) runs this exec each frame
// (the scene's virtual at vtable+0x390: v3.0.3 main+0x1bba050, the Endless play scene's only). With a 100 Mario run
// the pause menu is not opened: the play stays held in that state while the flow shows SMM1's pause, and its choice
// leaves the state the way the menu's own items do: resume -> state 1 (as when the menu closes), Start Over -> 0x13
// cRestartEndlessMode, Save & Quit -> 0x12 cSuspendEndlessMode (as after the confirm window's Yes).
// [obs: 0x7101bba050, 0x7101bbad00, 0x7101bbb350]
#define PAUSE_EXEC MAIN_ADDR(0x1bba050)
#define CHANGE_STATE MAIN_ADDR(0x8b9320)  // (machine, state index)
#define PAUSE_FLAGS MAIN_ADDR(0x2a37ed8)  // ptr: +0x330 bit 0 cleared when the pause closes
#define RUN_OBJECT MAIN_ADDR(0x2c56bb0)   // ptr: the Endless run (+0x49: u16 cleared before a suspend)
enum { PAUSE_NONE, PAUSE_RESUME, PAUSE_RETRY, PAUSE_SAVE_QUIT };
static volatile int pause_cmd;
static volatile bool paused, restart_from_pause;
static void test_pause(void);

static void change_state(Cpu *c, uint64_t machine, uint32_t st) {
    uint64_t save[8];
    memcpy(save, c->x, sizeof save);
    c->x[0] = machine, c->x[1] = st;
    guest_call(c, CHANGE_STATE);
    memcpy(c->x, save, sizeof save);
}

static void vcall(Cpu *c, uint64_t obj, uint32_t slot) {
    uint64_t save[8];
    memcpy(save, c->x, sizeof save);
    c->x[0] = obj;
    guest_call(c, g64(g64(obj) + slot));
    memcpy(c->x, save, sizeof save);
}

static void pause_exec(Cpu *c) {
    uint64_t s = c->x[0];
    // (owner, 2026-10-19: SMM2's own pause during a run; its Save & Quit suspends the run. The SMM1 pause below only
    // with RECOMP_MOD_SMM1_PAUSE=1.)
    static int smm1_pause = -1;
    if (smm1_pause < 0) smm1_pause = getenv("RECOMP_MOD_SMM1_PAUSE") && atoi(getenv("RECOMP_MOD_SMM1_PAUSE"));
    if (!smm1_pause || !mod_hundred_rules() || run_d < 0) { guest_call_orig(c, PAUSE_EXEC); return; }
    uint64_t p = g64(s + 0x2e0);
    uint32_t ps = p ? g32(p + 0xa8) - 3 : 99;
    bool ps_on = ps < 4 && ((0xbu >> ps) & 1);
    if (ps_on && g32(p + 0x248) == 5) { guest_call_orig(c, PAUSE_EXEC); return; }  // (the game's own skip path)
    if (g32(s + 0x244) == 0) {  // just paused
        s32(s + 0x280, 1);
        if (ps_on) vcall(c, p, 0xb0);  // (what the pause does first, as the game's own)
        pause_cmd = PAUSE_NONE, paused = true;
        LOG("[mod] bridge: paused (SMM1's pause)\n");
        mod_shot("paused", 60);
        if (g_mod.log) {  // the machine's first states (enter / exec / exit member pointers), for the record
            uint64_t tab = g64(s + 0x268);
            for (int i = 0; i < 3 && tab; i++)
                LOG("[mod]   play state %d: enter %llx/%llx exec %llx/%llx exit %llx/%llx\n", i,
                    (unsigned long long)g64(tab + 0x40 * i + 0x10), (unsigned long long)g64(tab + 0x40 * i + 0x18),
                    (unsigned long long)g64(tab + 0x40 * i + 0x20), (unsigned long long)g64(tab + 0x40 * i + 0x28),
                    (unsigned long long)g64(tab + 0x40 * i + 0x30), (unsigned long long)g64(tab + 0x40 * i + 0x38));
        }
        if (test_difficulty() >= 0) test_pause();
        else hundred_flow_pause();
        return;
    }
    int cmd = pause_cmd;
    if (cmd == PAUSE_NONE) return;
    pause_cmd = PAUSE_NONE, paused = false;
    uint64_t flags = g64(PAUSE_FLAGS);
    switch (cmd) {
    case PAUSE_RESUME:
        if (flags) s32(flags + 0x330, g32(flags + 0x330) & ~1u);
        s32(s + 0x280, 4);
        change_state(c, s + 0x238, 1);
        LOG("[mod] bridge: pause closed, the course goes on\n");
        break;
    case PAUSE_RETRY:
        restart_from_pause = true;
        s32(s + 0x280, 4);
        change_state(c, s + 0x238, 0x13);
        LOG("[mod] bridge: Start Over\n");
        break;
    case PAUSE_SAVE_QUIT:
        suspending = true;
        s8(s + 0x2c0, 1);
        s32(s + 0x280, 4);
        change_state(c, s + 0x238, 0x12);
        if (g64(RUN_OBJECT)) s16(g64(RUN_OBJECT) + 0x49, 0);
        LOG("[mod] bridge: Save & Quit from the pause\n");
        break;
    }
}

// The play state (index 1 of the same machine; exec = the Endless scene's virtual at vtable+0x350, main+0x1bb9440)
// runs while the course plays: its first frame after a course start (or a retry) is when the course is on screen
// and playing, and the flow hears hundred_flow_course_started() then. (Fallback: 10 s after the start request.)
#define PLAY_EXEC MAIN_ADDR(0x1bb9440)

static void course_started(const char *why) {
    if (!started_pending) return;
    started_pending = false;
    MLOG("bridge: course on screen (%s)\n", why);
    mod_shot("playing", 120);
    if (test_difficulty() < 0) hundred_flow_course_started();
}

static void play_exec(Cpu *c) {
    // A course loaded underneath the flow's map waits at its first play frame until the flow's card is up
    // (hundred_flow_ready_to_start), so it doesn't run hidden.
    static bool holding;
    if (started_pending && run_d >= 0 && mod_hundred_rules() && test_difficulty() < 0 && !hundred_flow_ready_to_start()) {
        if (!holding) holding = true, LOG("[mod] bridge: the course waits for the flow's card\n");
        return;
    }
    if (holding) holding = false, LOG("[mod] bridge: the course starts\n");
    if (started_pending) course_started("play state");
    guest_call_orig(c, PLAY_EXEC);
}

bool bridge_take_start_over(void) {  // endless.c: the life being posted is a Start Over
    bool v = restart_from_pause;
    restart_from_pause = false;
    return v;
}

// ---- RECOMP_MOD_SHOTS=<name>: window shots at the run's moments (tests and evidence), build/<name>_<n>_<what>.png
extern char g_window_shot_request[256];  // runtime/game_hooks.c (weak), saved by the renderer
struct shot { uint64_t at; char name[48]; };
static void *shot_later(void *arg) {
    struct shot *s = arg;
    while (renderer_present_count() < s->at) usleep(5000);
    static int n;
    snprintf(g_window_shot_request, sizeof g_window_shot_request, "build/%s_%03d_%s.png", getenv("RECOMP_MOD_SHOTS"),
             ++n, s->name);
    free(s);
    return NULL;
}
static void mod_shot(const char *what, uint64_t after_frames) {
    if (!getenv("RECOMP_MOD_SHOTS")) return;
    struct shot *s = malloc(sizeof *s);
    s->at = renderer_present_count() + after_frames;
    snprintf(s->name, sizeof s->name, "%s", what);
    pthread_t th;
    pthread_create(&th, NULL, shot_later, s);
    pthread_detach(th);
}

// RECOMP_MOD_SHOT_BURST=<every>: a window shot every <every> frames for a while after the run's transitions (Next,
// SMM2 leaving its map), build/<RECOMP_MOD_SHOTS>_<n>_<what>.png
struct burst { uint64_t from, frames, every; char name[40]; };
static void *burst_run(void *arg) {
    struct burst *b = arg;
    static int n;
    for (uint64_t at = b->from; at < b->from + b->frames; at += b->every) {
        while (renderer_present_count() < at) usleep(2000);
        if (g_window_shot_request[0]) continue;
        snprintf(g_window_shot_request, sizeof g_window_shot_request, "build/%s_b%04d_%s_%llu.png",
                 getenv("RECOMP_MOD_SHOTS") ? getenv("RECOMP_MOD_SHOTS") : "burst", ++n, b->name,
                 (unsigned long long)(at - b->from));
    }
    free(b);
    return NULL;
}
static void mod_burst(const char *what, uint64_t frames) {
    const char *e = getenv("RECOMP_MOD_SHOT_BURST");
    if (!e || atoi(e) <= 0) return;
    struct burst *b = malloc(sizeof *b);
    b->from = renderer_present_count(), b->frames = frames, b->every = (uint64_t)atoi(e);
    snprintf(b->name, sizeof b->name, "%s", what);
    pthread_t th;
    pthread_create(&th, NULL, burst_run, b);
    pthread_detach(th);
}

// ---- events from endless.c (the game's own requests)
void bridge_event(const char *what, uint32_t d) {
    pthread_once(&once, driver_start);
    if (!strcmp(what, "start") || !strcmp(what, "retry")) mod_shot(what, 300), mod_shot(what, 900);
    else if (!strcmp(what, "miss")) mod_shot(what, 20);
    else if (!strncmp(what, "clear", 5)) mod_shot("clear", 30), mod_shot("result", 600), mod_shot("result", 900);
    if (!strcmp(what, "start")) {
        course_scene("course start");
        drv = DRV_COURSE, drv_since = course_since = renderer_present_count();
        started_since = renderer_present_count(), started_pending = true;
    } else if (!strcmp(what, "miss")) {
        drv = DRV_COURSE, drv_since = renderer_present_count();  // (SMM2 retries the course by itself)
        result(HUNDRED_MISS);
    } else if (!strcmp(what, "retry")) {  // (hooks.c: the course is set up again after a miss / Start Over)
        drv = DRV_COURSE, drv_since = renderer_present_count();  // (the autoplay's skip time counts on)
        LOG("[mod] bridge: the course again (retry)\n");
        started_since = renderer_present_count(), started_pending = true;
    } else if (!strcmp(what, "clear") || !strcmp(what, "clear_last")) {
        // (the result screen's comment button wants the course's "comments on" flag, course global +0x860 [inf,
        // main+0x1be7d30]: our server takes comments on every course)
        uint64_t course = g64(MAIN_ADDR(0x2c4b710));
        if (course) s8(course + 0x860, 1);  // (not +0x861: that one greys the Tag button [obs])
        // SMM2's own clear: goal, fanfare, result screen (Boo / Like / Quit / Next) are the player's. Next shows up
        // as the map's Mario walk (HUNDRED_NEXT); Quit as the menu without it (HUNDRED_GIVEUP_SAVE).
        run_won = !strcmp(what, "clear_last");
        awaiting_next = true, next_menu_since = 0;
        // (the flow test, or the autoplay standing in for the player, presses Next itself)
        set_drv(test_difficulty() >= 0 || getenv("RECOMP_MOD_AUTOPLAY") ? DRV_AFTER_CLEAR : DRV_IDLE);
        result(HUNDRED_CLEAR);
    } else if (!strcmp(what, "skip")) {
        set_drv(DRV_IDLE);  // (SMM2's Endless starts the next course at once)
        result(HUNDRED_SKIP);
    } else if (!strcmp(what, "new")) {
        run_d = (int)d;
        mod_shot("map", 200);
    } else if (!strcmp(what, "broken")) {  // "The course data is corrupt.": OK, then the run goes on (Play)
        LOG("[mod] bridge: a broken course: the window is answered, the next course comes\n");
        mod_shot("broken", 30), mod_shot("broken", 300), mod_shot("broken", 700), mod_shot("broken", 1200);
        broken_d = (int)d;
        set_drv(DRV_BROKEN);
        result(HUNDRED_SKIP);
    } else if (!strcmp(what, "suspend")) {
        suspend_posted = true;
        if (!leaving_won) suspending = true;
    }
}

// ---- the run side's API (hundred.h)
bool hundred_run_status(struct hundred_status *out) {
    endless_fill_status(out, run_d);
    return true;
}

bool hundred_run_start(int difficulty, bool resume_saved) {
    if (difficulty < 0 || difficulty > 3 || (!mod_hundred_rules() && test_difficulty() < 0)) return false;
    struct hundred_status st;
    endless_fill_status(&st, -1);
    if (!st.unlocked[difficulty]) return false;
    if (resume_saved && !st.can_continue[difficulty]) resume_saved = false;
    over_save = !resume_saved && st.can_continue[difficulty];
    pthread_once(&once, driver_start);
    endless_prepare_run(difficulty, resume_saved);
    run_d = difficulty;
    tab_target = difficulty, tab_presses = 0;
    suspending = paused = false;
    set_drv(resume_saved ? DRV_CONTINUE : DRV_NEW_GAME);
    mod_shot("menu", 60), mod_shot("menu", 330);
    LOG("[mod] bridge: run %s, difficulty %d\n", resume_saved ? "continued" : "start", difficulty);
    return true;
}

bool hundred_play_next(void) {
    if (run_d < 0) return false;
    if (drv == DRV_AFTER_CLEAR) play_queued = true;
    else if (drv == DRV_IDLE) set_drv(DRV_PLAY);
    return true;
}

void hundred_play_resume(void) {
    if (paused) pause_cmd = PAUSE_RESUME;
}
void hundred_play_retry(void) {  // Start Over from the pause (after a miss SMM2's Endless retries by itself)
    if (paused) pause_cmd = PAUSE_RETRY;
}
void hundred_play_skip(void) { set_drv(DRV_SKIP); }

static void *post_async(void *path) {
    Json *j = srv_call("POST", (const char *)path, "{}", NULL);
    json_free(j);
    free(path);
    return NULL;
}

static void post_later(const char *what) {
    static const char *const diff[4] = {"easy", "normal", "expert", "super_expert"};
    if (run_d < 0) return;
    char *path = malloc(80);
    snprintf(path, 80, "/api/endless/%s/%s?ruleset=hundred", diff[run_d], what);
    pthread_t th;
    pthread_create(&th, NULL, post_async, path);
    pthread_detach(th);
}

// Save & Quit: from the pause, the pause's own suspend; after a clear, the result screen's Quit; else the map's
// Quit. The game posts the suspend itself; back on its menu the flow gets HUNDRED_GIVEUP_SAVE.
void hundred_run_save_quit(void) {
    if (run_d < 0) return;
    if (paused) { pause_cmd = PAUSE_SAVE_QUIT; return; }
    suspending = true, suspend_posted = false;
    set_drv(drv == DRV_AFTER_CLEAR ? DRV_RESULT_QUIT : DRV_SAVE_QUIT);
}

void hundred_run_finish(void) {
    LOG("[mod] bridge: run finished (difficulty %d)%s\n", run_d, run_won ? ": back to SMM2's menu" : "");
    post_later("finish");
    endless_forget_run(run_d);
    run_d = -1;
    awaiting_next = false;
    if (run_won) {  // (won: the last result screen's Next became its Quit, SMM2 is going back to its menu)
        run_won = false;
        set_drv(DRV_IDLE);
    } else if (drv != DRV_RESULT_QUIT && drv != DRV_SAVE_QUIT) {
        set_drv(DRV_IDLE);
    }
}

// B on SMM1's difficulty select: SMM2's own Endless Challenge, whose menu is underneath (both challenges in one
// build). Leaving that menu (B there) ends it: the next visit opens 100 Mario again.
void hundred_flow_exit(void) {  // (B on SMM2's Endless menu underneath: Course World)
    LOG("[mod] bridge: back to Course World\n");
    pthread_once(&once, driver_start);
    back_since = renderer_present_count(), disappear_seen = 0;
    set_drv(DRV_BACK);
}
// After hundred_flow_exit: SMM2's Endless menu has finished leaving (its Disappear ran and has stopped), so Course
// World is what is under the select. (false until then; the flow also gives up waiting after a while)
bool hundred_menu_left(void) {
    uint64_t now = renderer_present_count();
    return back_since && disappear_seen > back_since && now - disappear_seen > 2;
}
// (the menu reaches its Disp about 45 frames after the flow opens on Activate; 300 frames: it never came)
bool hundred_menu_in(void) { return !opening_since || renderer_present_count() - opening_since > 300; }

bool hundred_modes_available(void) { return mod_hundred_offered(); }
void hundred_mode_switch(void) {  // ZL / ZR / the tap on the flow's select: SMM2's own Endless Challenge
    LOG("[mod] bridge: Challenge Modes: to SMM2's Endless Challenge\n");
    set_drv(DRV_IDLE);
    mod_remember_mode(true);
    mod_set_smm2_session(true);
    switch_press_at = 0, switch_blocked = true;  // (the press that switched is no press on SMM2's menu)
    uint32_t d = g32(MAIN_ADDR(0x2c3f618));  // (the menu's difficulty: its tab again redraws the run shown)
    pending_tab = d < 4 ? (int)d : 0;
}

// (the flow's: whether a course may start playing; this stand-in, for builds without it, says yes)
__attribute__((weak)) bool hundred_flow_ready_to_start(void) { return true; }

// The Endless menu's Disappear state (index 5 of its machine; exec main+0x1a24d30): the menu closes towards Course
// World (a course start goes through CourseIn / InFade instead).
#define MENU_DISAPPEAR_EXEC MAIN_ADDR(0x1a24d30)
// The course-end machine after SMM2's result screen (registered at main+0x1bed050). Its cPostRatingCheckCan exec
// (main+0x1bef440) acts on the result screen's choice (screen object +0x2a0, done +0x2a4): 5 Next -> cNextScene
// (0x4b, enter main+0x1bf6430), 6 / 7 the two ratings, 8 Quit -> in the Endless Challenge cEndlessMode_GetStatus
// (0x4d, enter main+0x1bf6630) and back to the menu, 9 comment, 10 tags [inf, from the code].
#define END_NEXT_SCENE_ENTER MAIN_ADDR(0x1bf6430)
#define END_ENDLESS_STATUS_ENTER MAIN_ADDR(0x1bf6630)
#define END_CHECK_CAN_EXEC MAIN_ADDR(0x1bef440)
// After the run's last clear, Next is turned into Quit (SMM2's Endless would go on to its map and another course):
// SMM2 goes back to its Endless menu underneath while the flow plays the ending, which Next starts (HUNDRED_NEXT).
static void end_check_can(Cpu *c) {
    uint64_t screen = g64(c->x[0] + 0x288);
    if (awaiting_next && run_won && run_d >= 0 && screen && g8(screen + 0x2a4) && g32(screen + 0x2a0) == 5) {
        s32(screen + 0x2a0, 8);
        awaiting_next = false, next_menu_since = 0, leaving_won = true;
        LOG("[mod] bridge: Next on the result screen (the run is won): SMM2 goes back to its menu\n");
        result(HUNDRED_NEXT);
    }
    guest_call_orig(c, END_CHECK_CAN_EXEC);
}
static void end_next_scene(Cpu *c) {
    if (awaiting_next && run_d >= 0) {
        awaiting_next = false, next_menu_since = 0;
        LOG("[mod] bridge: Next on the result screen%s\n", run_won ? " (the run is won)" : "");
        mod_burst("next", 900);
        result(HUNDRED_NEXT);
    }
    guest_call_orig(c, END_NEXT_SCENE_ENTER);
}
static void end_endless_status(Cpu *c) {
    if (awaiting_next && run_d >= 0) {  // Quit: the run is kept; the menu's return reports HUNDRED_GIVEUP_SAVE
        awaiting_next = false, next_menu_since = 0, suspending = true;
        LOG("[mod] bridge: Quit on the result screen\n");
    }
    guest_call_orig(c, END_ENDLESS_STATUS_ENTER);
}
static void menu_disappear(Cpu *c) {
    disappear_seen = renderer_present_count();
    // (towards Course World: the next visit opens on the mode used last)
    if (g32(c->x[0] + 0x244) == 0) mod_set_smm2_session(g_mod.mode_endless);
    guest_call_orig(c, MENU_DISAPPEAR_EXEC);
}

// ---- RECOMP_MOD_FLOW_TEST=<difficulty>: an unattended run in place of the flow (evidence, regression checks). The
// flow's screens are left out (hundred_flow_open is not called); the run starts on menu entry (Continue with
// RECOMP_MOD_FLOW_TEST_CONTINUE=1), Play follows every clear, and the win plays the ending (hundred_ending_show) and
// finishes the run. RECOMP_MOD_FLOW_TEST_PAUSE=<script> pauses in the courses and answers the pause with the script's
// letters in turn (r resume, o Start Over, q Save & Quit), each 8 s into a course.
static int flow_test = -2;

static int test_difficulty(void) {
    if (flow_test == -2) flow_test = getenv("RECOMP_MOD_FLOW_TEST") ? atoi(getenv("RECOMP_MOD_FLOW_TEST")) : -1;
    return flow_test;
}

static void *test_win(void *arg) {
    (void)arg;
    struct hundred_status st;
    hundred_run_status(&st);
    hundred_ending_show(st.difficulty, st.ncredits, st.credits);
    for (int i = 0; i < 30 * 60 * 50 && !hundred_ending_done(); i++) usleep(20000);
    hundred_run_finish();
    return NULL;
}

static void test_result(int outcome) {
    static const char *const names[] = {"clear", "miss", "skip", "save & quit", "error", "next"};
    struct hundred_status st;
    hundred_run_status(&st);
    LOG("[mod] flow test: result %s, lives %d, cleared %d / %d\n", names[outcome], st.lives, st.cleared, st.total);
    if (outcome == HUNDRED_CLEAR) {
        static int quits;  // RECOMP_MOD_FLOW_TEST_QUIT_AFTER_CLEAR=<n>: Quit on the first n result screens
        const char *qa = getenv("RECOMP_MOD_FLOW_TEST_QUIT_AFTER_CLEAR");
        if (qa && quits < atoi(qa) && !(st.total && st.cleared >= st.total)) {
            quits++;
            LOG("[mod] flow test: Quit on the result screen\n");
            hundred_run_save_quit();
        }
        return;
    }
    if (outcome != HUNDRED_NEXT) return;
    if (st.total && st.cleared >= st.total) {
        LOG("[mod] flow test: 100 Mario Challenge won\n");
        pthread_t th;
        pthread_create(&th, NULL, test_win, NULL);
        pthread_detach(th);
    } else {
        hundred_play_next();
    }
}

static int test_pause_step;
static void *test_pause_answer(void *arg) {
    int what = (int)(intptr_t)arg;
    usleep(3000000);
    struct hundred_status st;
    hundred_run_status(&st);
    LOG("[mod] flow test: pause answered '%c' (lives %d, course %s)\n", what, st.lives, st.current_code);
    if (what == 'o') hundred_play_retry();
    else if (what == 'q') hundred_run_save_quit();
    else hundred_play_resume();
    return NULL;
}
static void test_pause(void) {
    const char *script = getenv("RECOMP_MOD_FLOW_TEST_PAUSE");
    int what = script && script[test_pause_step] ? script[test_pause_step++] : 'r';
    pthread_t th;
    pthread_create(&th, NULL, test_pause_answer, (void *)(intptr_t)what);
    pthread_detach(th);
}

static void *test_pauser(void *arg) {  // presses + 8 s into each course while the script has letters
    (void)arg;
    const char *script = getenv("RECOMP_MOD_FLOW_TEST_PAUSE");
    uint64_t last = 0;
    for (;;) {
        usleep(100000);
        if (!script || !script[test_pause_step]) return NULL;
        uint64_t now = renderer_present_count();
        if (drv == DRV_COURSE && course_since != last && now - course_since > 480) {
            last = course_since;
            set_drv(DRV_PAUSE_TEST);
        }
    }
}

static void tab_probe(Cpu *c) {
    uint64_t x0 = c->x[0], pc = c->pc;
    long off = -1;
    for (long o = 0; menu_page && o < 0x800; o += 8)
        if (g64(menu_page + (uint64_t)o) == x0) { off = o; break; }
    LOG("[mod] tab handler %llx: this %llx, menu page %llx, found at page+%#lx\n", (unsigned long long)pc,
        (unsigned long long)x0, (unsigned long long)menu_page, off);
    guest_call_orig(c, pc);
}

void bridge_register(void) {
    hle_override(MENU_DISP_EXEC, menu_disp, "mod: Endless menu Disp (bridge)");
    hle_override(SEQ_ACTIVATE_EXEC, seq_activate, "mod: Course World's Endless Activate (bridge)");
    hle_override(FADE_OUT_ENTER, fade_out, "mod: scene fade Out (bridge)");
    hle_override(FADE_BLACK_ENTER, fade_black, "mod: scene fade black (bridge)");
    hle_override(PAUSE_EXEC, pause_exec, "mod: Endless pause (bridge)");
    hle_override(PLAY_EXEC, play_exec, "mod: Endless play state (bridge)");
    hle_override(MENU_DISAPPEAR_EXEC, menu_disappear, "mod: Endless menu Disappear (bridge)");
    hle_override(END_NEXT_SCENE_ENTER, end_next_scene, "mod: course end cNextScene (bridge)");
    hle_override(END_ENDLESS_STATUS_ENTER, end_endless_status, "mod: course end cEndlessMode_GetStatus (bridge)");
    hle_override(END_CHECK_CAN_EXEC, end_check_can, "mod: course end cPostRatingCheckCan (bridge)");
    hle_override(MENU_COURSEIN_EXEC, menu_course_in, "mod: Endless menu CourseIn (bridge)");
    hle_override(PLAY_LIST_DOWNLOAD_ENTER, play_list_download, "mod: Endless play cCourseListDownLoad (bridge)");
    hle_override(PLAY_DATA_DOWNLOAD_ENTER, play_data_download, "mod: Endless play cCourseDataDownLoad (bridge)");
    if (getenv("RECOMP_MOD_TABPROBE")) {
        hle_override(MENU_TAB0, tab_probe, "probe");
        hle_override(MENU_TAB0 + 0xc0, tab_probe, "probe");
    }
    if (test_difficulty() >= 0 && getenv("RECOMP_MOD_FLOW_TEST_PAUSE")) {
        pthread_t th;
        pthread_create(&th, NULL, test_pauser, NULL);
        pthread_detach(th);
    }
}
