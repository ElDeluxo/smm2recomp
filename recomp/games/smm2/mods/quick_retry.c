// Quick level return: after a miss, the course is back at its start (or the checkpoint) in 1.5-2.5 s instead of SMM2's
// 4.5-10 (measured on an M-series Mac; the course's reload itself, about 0.7 s, isn't shortened). The game itself does everything as before: the miss, the lives, the restart. The
// frames from the death fanfare until the player is updated again in the restarted course are run as fast as the
// machine can, without being shown (runtime/nvn/objects.c g_present_hidden). Lives count as before (100 Mario and
// Endless: a miss still costs one) and clear rules don't change; the fanfare is only heard for as long as it lasts.
//
// The miss is the death fanfare (music state 7, mods/costume.c down_fanfare). The restart is, in Endless and 100 Mario,
// the Endless scene's play state running again (after the lives card; mods/bridge.c), elsewhere the first player
// update after a stretch of frames without one (the course is torn down and built again). A cap ends the
// fast-forward whatever happens (a game over, a menu), and outside Endless it lapses within a second where no player
// is updated (test play's miss goes back to the Course Maker), so the screen never stays still for long.
//
// One switch in the Mods tab (off by default), kept in smm2mods.ini; an environment variable wins over the ini:
//   RECOMP_QUICK_RETRY=0/1   quick_retry=0/1
// Off, nothing at all changes.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod.h"

typedef struct { const char *label, *desc; bool *value; bool def; } GameModToggle;  // (runtime/game_hooks.c)
void mod_ini_set(const char *key, const char *val);                                 // (settings.c)
extern _Atomic int g_present_hidden;                                                // runtime/nvn/objects.c
extern uint64_t renderer_present_count(void);

#define CAP_FRAMES 360  // at most this many frames after the miss are fast-forwarded (SMM2's own miss to restart: ~300)
#define CAP_ENDLESS 900 // ... in Endless and 100 Mario, lives card included (SMM2's own: ~545)
#define LEASE_FRAMES 60 // the fast-forward lasts this long past the last player update (none: a scene without a player)
#define GAP_FRAMES 3    // frames without a player update: the course is being built again

// ---------------------------------------------------------------- the setting
static bool g_on, g_shown;
static void load_once(void) {
    const char *ini = getenv("RECOMP_MOD_INI") ? getenv("RECOMP_MOD_INI") : "smm2mods.ini";
    FILE *f = fopen(ini, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "quick_retry=", 12)) g_on = atoi(line + 12) != 0;
        fclose(f);
    }
    if (getenv("RECOMP_QUICK_RETRY")) g_on = atoi(getenv("RECOMP_QUICK_RETRY")) != 0;
    LOG("[mod] quick level return %s\n", g_on ? "on" : "off");
}
static bool retry_on(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, load_once);
    return g_on;
}

int quick_retry_toggles(GameModToggle *out, int room) {  // (settings.c appends it to the Mods tab)
    static const GameModToggle t = {
        "Quick level return",
        "After a miss the course starts again (from its start or the checkpoint) much sooner: the miss, the lives "
        "card and the restart are fast-forwarded, unseen. Lives still count as before (100 Mario, Endless) and "
        "clears don't change. Off: SMM2's own miss. Also in test play. Immediately.",
        &g_shown, false};
    if (room < 1) return 0;
    g_shown = retry_on();
    out[0] = t;
    return 1;
}
void quick_retry_toggles_changed(void) {
    if (g_shown == retry_on()) return;
    g_on = g_shown;
    mod_ini_set("quick_retry", g_on ? "1" : "0");
    LOG("[mod] quick level return %s\n", g_on ? "on" : "off");
}

// ---------------------------------------------------------------- the fast-forward
// (presented-frame clock) the miss; the last player update; the last frame of the Endless scene's play state
static bool g_ff, g_endless;
static uint64_t g_ff_from, g_player_at, g_play_at;

static void stop(const char *why) {
    atomic_store(&g_present_hidden, 0);
    g_ff = false;
    LOG("[quick-retry] %s after %llu frames\n", why, (unsigned long long)(renderer_present_count() - g_ff_from));
}

void quick_retry_miss(void) {  // mods/costume.c: the death fanfare starts
    if (!retry_on()) return;
    uint64_t now = renderer_present_count();
    // (Endless and 100 Mario: the play state ran until now; the course comes back to it after the lives card)
    g_endless = g_play_at && now - g_play_at <= GAP_FRAMES;
    g_ff = true, g_ff_from = g_player_at = now;
    // (Endless and 100 Mario: no player while the lives card is up, so no lease; the play state's return ends it)
    atomic_store(&g_present_hidden, g_endless ? CAP_ENDLESS : LEASE_FRAMES);
    LOG("[quick-retry] miss at frame %llu: fast-forward\n", (unsigned long long)now);
}

// Every player update (games/smm2/trace.c). Each one keeps the fast-forward going (LEASE_FRAMES more), up to the cap:
// where no player comes (a menu, the Course Maker) it runs out by itself. Course World, Coursebot, test play...: the
// course is built again (no player for a few frames), and the first update after that is the restarted course.
void quick_retry_player(uint64_t self) {
    (void)self;
    if (!g_ff) return;
    uint64_t now = renderer_present_count();
    if (g_endless) {
        if (!atomic_load(&g_present_hidden)) g_ff = false, LOG("[quick-retry] cap reached\n");
    } else if (now - g_player_at > GAP_FRAMES) {
        stop("restarted");
    } else if (now - g_ff_from >= CAP_FRAMES) {
        stop("cap reached");
    } else {
        atomic_store(&g_present_hidden, LEASE_FRAMES);
    }
    g_player_at = now;
}

// Every frame of the Endless scene's play state (mods/bridge.c, after the 100 Mario flow lets the course go): Endless
// and 100 Mario keep the player through the lives card, the play state's return is the restart.
void quick_retry_play_state(void) {
    uint64_t now = renderer_present_count();
    if (g_ff && g_endless && now - g_play_at > GAP_FRAMES) stop("restarted (Endless)");
    g_play_at = now;
}
