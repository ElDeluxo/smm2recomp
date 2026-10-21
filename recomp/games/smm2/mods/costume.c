// Mod 2, Mystery Mushroom costumes (research/specs/recomp-mods.md §3): SMM1's costumes worn in SMB1-style courses.
//
// The player wearing a costume stays form 0 (small Mario) for the game's logic: size class, physics, hit boxes and the
// block and power-up rules are untouched, which is SMM1's costume behaviour. What changes:
//   - Look: the model builder (0x710155fcd0, once per player per course) loads, for the SMB1 style, the course's
//     costume into the spare form slot 12 (Balloon: unused in SMB1, size class 1 like small Mario) by pointing that
//     slot's model name at "Player_Chara_<res>" for the call; the visual slot setter (0x710155fcc0) shows slot 12 where
//     the game asks for slot 0 while the costume is worn. The models are generated from the player's dumps
//     (smm1/costume_gen.c) and laid over the romfs.
//   - Pickup: SMM1's Mystery Mushroom is the SMB1 skin of the style power-up (the Big Mushroom): taking it from small
//     Mario puts the course's costume on instead of growing (costume_item=super: the Super Mushroom instead, as
//     SMCraft did). Another power-up replaces the costume.
//   - Damage: a hit takes the costume off with the Super -> small power-down (the form is read as 1 for the game's
//     damage function, hook point 0x15a50a4), as a costume absorbs a hit in SMM1.
//   - Sound: the costume's own power-up, goal and death sounds (SMM1's Sound/CharaMario banks, host mixer); SMM2's goal
//     and death fanfares give way to them (their choosers, 0x71017a5020 / 0x71017a4bc0, see the state 6 / 7 request).
//   - Reward: one random costume not owned yet at the end of a 100 Mario run, from SMM1's pools by difficulty
//     (smm1-100mario-code.md §7); owned costumes are kept in costumes-owned.txt in the data folder.
// The course's costume is a random owned one (SMM1 draws one at each pickup; one a course here: a model is loaded
// with the player [inf: the pickup can't load a model mid-course]).
// Settings: costumes=1 in smm2mods.ini (or RECOMP_MOD_COSTUMES=1) turns it on (models generated at the next start);
// costume_item=big|super; RECOMP_COSTUME=<res> wears that costume from each course's start (tests).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mod.h"

int costume_count(void);
const char *costume_res(int i);
int costume_rare(const char *res);
void costume_list(void);
bool smm1_data_open(void);
void smm1_se_bank(const char *bank);
bool smm1_se(const char *name);

#define BUILDER MAIN_ADDR(0x155fcd0)      // model holder build (holder, style, character, heap)
#define SETTER MAIN_ADDR(0x155fcc0)       // visual slot setter (holder, int *slot): holder +0x478
#define NAME_TABLE MAIN_ADDR(0x2c17638)   // model base names: SafeString {vtable, char *} [style * 16 + form]
#define STYLE_PTR MAIN_ADDR(0x2c57d58)    // *(*(this + 0x30) + 0x1c): the course's style (0 SMB1)
#define GOAL_FANFARE MAIN_ADDR(0x17a5020) // course music state changes; state 6 = the goal fanfare
#define DOWN_FANFARE MAIN_ADDR(0x17a4bc0) // ... state 7 = the death fanfare
#define DAMAGE_FORM_LOAD 0x15a50a4        // hook point: ldr w8, [x19, #0x4a8] at the damage function's start
#define TRANSFORM_REQ 0x15a94a0           // hook point: the form change request's first instruction (player, int *form)
#define COSTUME_SLOT 12

// ---------------------------------------------------------------- settings (a block of its own in smm2mods.ini)
static int g_on = -1, g_item = 6;
static char g_forced[48], g_course[48], g_wear[48], g_model_name[96] = "Player_Chara_Mario";

static const char *ini_path(void) { return getenv("RECOMP_MOD_INI") ? getenv("RECOMP_MOD_INI") : "smm2mods.ini"; }
static int ini_int(const char *key, int def) {
    FILE *f = fopen(ini_path(), "r");
    if (!f) return def;
    char line[256];
    size_t kl = strlen(key);
    int v = def;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, kl) && line[kl] == '=') v = atoi(line + kl + 1);
    fclose(f);
    return v;
}
static bool ini_is(const char *key, const char *val) {
    FILE *f = fopen(ini_path(), "r");
    if (!f) return false;
    char line[256], want[128];
    snprintf(want, sizeof want, "%s=%s", key, val);
    bool r = false;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, want, strlen(want))) r = true;
    fclose(f);
    return r;
}
static void ini_put(const char *key, const char *val) {  // replace or append key=val, the rest kept
    char *text = NULL;
    size_t n = 0;
    FILE *f = fopen(ini_path(), "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        n = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        text = calloc(1, n + 1);
        if (fread(text, 1, n, f) != n) n = 0;
        fclose(f);
    }
    f = fopen(ini_path(), "w");
    if (!f) { free(text); return; }
    size_t kl = strlen(key);
    bool done = false;
    for (char *s = text, *e; s && *s; s = e ? e + 1 : NULL) {
        e = strchr(s, '\n');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (!strncmp(s, key, kl) && s[kl] == '=') {
            if (!done) fprintf(f, "%s=%s\n", key, val), done = true;
        } else
            fprintf(f, "%.*s\n", (int)l, s);
    }
    if (!done) fprintf(f, "%s=%s\n", key, val);
    fclose(f);
    free(text);
}
bool costumes_on(void) {
    if (g_on < 0) {
        const char *e = getenv("RECOMP_MOD_COSTUMES");
        g_on = e ? atoi(e) != 0 : ini_int("costumes", 0) != 0;
        const char *w = getenv("RECOMP_COSTUME"), *it = getenv("RECOMP_COSTUME_ITEM");
        if (w && *w) snprintf(g_forced, sizeof g_forced, "%s", w);
        g_item = (it ? !strcmp(it, "super") : ini_is("costume_item", "super")) ? 1 : 6;
        if (g_on)
            LOG("[costume] Mystery Mushroom costumes on (the %s Mushroom)%s%s\n", g_item == 1 ? "Super" : "Big",
                g_forced[0] ? ", wearing " : "", g_forced);
    }
    return g_on;
}
// the Mods tab's switch (the models are generated at the next start)
static int g_setting = -1;  // the switch as last set (the mod itself follows at the next start)
bool costumes_setting(void) { return g_setting >= 0 ? g_setting != 0 : costumes_on(); }
void costumes_set_setting(bool on) {
    if (on == costumes_setting()) return;
    g_setting = on;
    ini_put("costumes", on ? "1" : "0");
    LOG("[costume] Mystery Mushroom costumes %s (at the next start)\n", on ? "on" : "off");
}

// (the models: the shared RomFS overlay's "costumes" generator, smm1/costume_gen.c)

// ---------------------------------------------------------------- owned costumes
static char (*g_owned)[48];
static int g_nowned = -1;
static void load_owned(void) {
    if (g_nowned >= 0) return;
    g_nowned = 0;
    costume_list();
    int n = costume_count();
    g_owned = calloc((size_t)n + 8, sizeof *g_owned);
    for (int i = 0; i < n; i++)  // category 0: always owned (Goomba, ? Block, Trampoline)
        if (costume_rare(costume_res(i)) == 0) snprintf(g_owned[g_nowned++], 48, "%s", costume_res(i));
    FILE *f = fopen("costumes-owned.txt", "r");
    char line[96];
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        bool have = !line[0];
        for (int i = 0; i < g_nowned && !have; i++) have = !strcmp(g_owned[i], line);
        if (!have && g_nowned < n + 8) snprintf(g_owned[g_nowned++], 48, "%s", line);
    }
    if (f) fclose(f);
}
static bool owned(const char *res) {
    load_owned();
    for (int i = 0; i < g_nowned; i++)
        if (!strcmp(g_owned[i], res)) return true;
    return false;
}
static unsigned rnd(void) {
    static unsigned s;
    if (!s) s = (unsigned)time(NULL) * 2654435761u | 1;
    s ^= s << 13, s ^= s >> 17, s ^= s << 5;
    return s;
}

// a random owned costume for a course (not the last one when there's a choice)
static void pick_course(void) {
    load_owned();
    if (g_forced[0]) { snprintf(g_course, sizeof g_course, "%s", g_forced); return; }
    if (!g_nowned) { g_course[0] = 0; return; }
    int k = (int)(rnd() % (unsigned)g_nowned);
    if (g_nowned > 1 && !strcmp(g_owned[k], g_course)) k = (k + 1) % g_nowned;
    snprintf(g_course, sizeof g_course, "%s", g_owned[k]);
}

// the costume reward at the end of a won 100 Mario run (ending.c calls it after the credits): one costume not owned
// yet, uniformly, from the pools up to the difficulty's (Easy 1; Normal 1, 2; Expert 1-3; Super Expert 1-3, 6), saved;
// NULL when none is left
const char *costume_reward_pick(int difficulty) {
    if (!costumes_on()) return NULL;
    load_owned();
    static const int ceiling[4] = {1, 2, 3, 6};
    int d = difficulty < 0 ? 0 : difficulty > 3 ? 3 : difficulty;
    int n = costume_count(), cand[256], nc = 0;
    for (int i = 0; i < n && nc < 256; i++) {
        int r = costume_rare(costume_res(i));
        if (r == 0 || r == 4 || r == 5 || r == 7 || r < 0 || r > ceiling[d]) continue;
        if (!owned(costume_res(i))) cand[nc++] = i;
    }
    if (!nc) { LOG("[costume] reward: none left at this difficulty\n"); return NULL; }
    const char *res = costume_res(cand[rnd() % (unsigned)nc]);
    snprintf(g_owned[g_nowned++], 48, "%s", res);
    FILE *f = fopen("costumes-owned.txt", "a");
    if (f) fprintf(f, "%s\n", res), fclose(f);
    LOG("[costume] reward: %s (%d left)\n", res, nc - 1);
    return res;
}
// ending.c's weak hook: the run is won (the reward itself is drawn by the ending, which asks costume_reward_pick)
void hundred_costume_reward(int difficulty) { (void)difficulty; }

// ---------------------------------------------------------------- wearing
static int g_debug = -1;
static bool sound(const char *event) {  // false: this costume has no such sound
    if (!g_wear[0] || !smm1_data_open()) return false;
    char bank[96], name[128];
    snprintf(bank, sizeof bank, "CharaMario_%s", g_wear);
    smm1_se_bank(bank);
    snprintf(name, sizeof name, "%s/%s", bank, event);
    bool ok = smm1_se(name);
    bool often = !strncmp(event, "jump", 4) || !strcmp(event, "ground");  // (every jump: only when debugging)
    if (!often || g_debug > 0) LOG("[costume] %s sound (%s)%s\n", event, g_wear, ok ? "" : ": none");
    return ok;
}
static void wear(const char *res) {
    snprintf(g_wear, sizeof g_wear, "%s", res ? res : "");
    if (g_wear[0]) LOG("[costume] wearing %s\n", g_wear);
}
const char *costume_worn(void) { return g_wear; }

static bool smb1_course(void) {
    uint64_t g = g64(STYLE_PTR);
    return g && g64(g + 0x30) && g32(g64(g + 0x30) + 0x1c) == 0;
}

// ---------------------------------------------------------------- the hooks
static void builder(Cpu *c) {
    uint32_t style = (uint32_t)c->x[1];
    if (!costumes_on() || style != 0) {
        guest_call_orig(c, BUILDER);
        return;
    }
    pick_course();
    wear(g_forced[0] ? g_forced : NULL);  // (a new course starts as small Mario)
    if (!g_course[0]) { guest_call_orig(c, BUILDER); return; }
    snprintf(g_model_name, sizeof g_model_name, "Player_Chara_%s", g_course);
    uint64_t slot = NAME_TABLE + (uint64_t)(style * 16 + COSTUME_SLOT) * 16;
    uint64_t was = g64(slot + 8);
    s64(slot + 8, (uint64_t)(uintptr_t)g_model_name);
    guest_call_orig(c, BUILDER);
    s64(slot + 8, was);
    LOG("[costume] course costume %s (model slot %d)\n", g_course, COSTUME_SLOT);
}

static void setter(Cpu *c) {
    uint64_t holder = c->x[0], ps = c->x[1];
    int32_t slot = (int32_t)g32(ps);
    if (costumes_on() && g_wear[0] && slot == 0 && g32(holder + 0x470) == 0) slot = COSTUME_SLOT;
    s32(holder + 0x478, (uint32_t)slot);
}

// every frame of each player (games/smm2/trace.c smm2_player_hook): the pickup and losing the costume to a power-up
// SMM1's costume sounds from the player's motion (SMCraft's events): a jump (jump or jump2, half and half), the
// landing (ground), and the taunt (Up held 8 frames standing still: appeal). The game's own player sounds still play
// [inf: SMM2's are data-driven action triggers; muting them needs the sound links' play call].
#define P_GROUND 0x2308  // bit 0: on the ground
#define P_VEL_Y 0x240    // float, up positive [inf: the sign from the jump test]
#define P_SPEED 0x274    // float, forward speed
#define P_INPUT_UP 0x560 // input block +0x10: Up held [inf]
static void motion_sounds(uint64_t player) {
    static uint64_t who;
    static bool was_ground;
    static int up;
    bool ground = g32(player + P_GROUND) & 1;
    float vy, sp;
    uint32_t a = g32(player + P_VEL_Y), b = g32(player + P_SPEED);
    memcpy(&vy, &a, 4), memcpy(&sp, &b, 4);
    if (who != player) who = player, was_ground = ground, up = 0;
    if (g_wear[0]) {
        if (was_ground && !ground && vy > 0.5f) {
            if (!(rnd() & 1) || !sound("jump2")) sound("jump");
        } else if (!was_ground && ground)
            sound("ground");
        if (ground && g8(player + P_INPUT_UP) && sp > -0.01f && sp < 0.01f) {
            if (++up == 8) sound("appeal");
        } else
            up = 0;
    }
    was_ground = ground;
}

void costume_player_frame(uint64_t player) {
    if (!costumes_on() || !g_course[0] || !smb1_course()) return;
    uint32_t form = g32(player + 0x4a8), pending = g32(player + 0x4b0);
    if (g_debug < 0) g_debug = getenv("RECOMP_COSTUME_DEBUG") != NULL;
    static uint32_t last_form = ~0u, last_pending = ~0u;
    if (g_debug && (form != last_form || pending != last_pending))
        LOG("[costume] player %#llx form %u pending %u\n", (unsigned long long)player, form, pending);
    last_form = form, last_pending = pending;
    if (g_wear[0] && form != 0) wear(NULL);  // (another power-up took its place)
    motion_sounds(player);
}

// hook points (games/smm2/mods/hooks.c smm2_mod_after): true when handled
bool costume_after(Cpu *c, uint64_t addr) {
    if (addr - MAIN_ADDR(0) == TRANSFORM_REQ) {  // a power-up asks for a form: from small, the Mystery item dresses
        uint64_t player = c->x[0], target = c->x[1];
        uint32_t form = g32(player + 0x4a8), want = g32(target);
        if (g_debug > 0) LOG("[costume] change request: player %#llx form %u -> %u\n", (unsigned long long)player, form, want);
        if (costumes_on() && g_course[0] && smb1_course() && form == 0 && want == (uint32_t)g_item) {
            s32(target, 0);  // (a change to small: the demo plays and the setter shows the costume)
            bool was = g_wear[0];
            wear(g_course);
            if (!was) sound("powerup");
        }
        return true;
    }
    if (addr - MAIN_ADDR(0) != DAMAGE_FORM_LOAD) return false;
    if (costumes_on() && g_wear[0] && (uint32_t)c->x[8] == 0) {  // a hit in costume: the Super -> small path
        c->x[8] = 1;
        LOG("[costume] hit: %s comes off\n", g_wear);
        wear(NULL);
    }
    return true;
}

static void goal_fanfare(Cpu *c) {
    bool mine = costumes_on() && g_wear[0] && (uint32_t)c->x[2] == 6;
    if (mine) c->x[2] = 0;  // (the course music only fades out)
    guest_call_orig(c, GOAL_FANFARE);
    if (mine) sound("goal");
}
static void down_fanfare(Cpu *c) {
    extern void quick_retry_miss(void);  // (mods/quick_retry.c: the miss is fast-forwarded when it's on)
    if ((uint32_t)c->x[2] == 7) quick_retry_miss();
    bool mine = costumes_on() && g_wear[0] && (uint32_t)c->x[2] == 7;
    if (mine) c->x[2] = 0;
    guest_call_orig(c, DOWN_FANFARE);
    if (mine) sound("down"), wear(NULL);
}

__attribute__((constructor)) static void costume_register(void) {
    hle_override(BUILDER, builder, "costume: model builder");
    hle_override(SETTER, setter, "costume: visual slot");
    hle_override(GOAL_FANFARE, goal_fanfare, "costume: goal fanfare");
    hle_override(DOWN_FANFARE, down_fanfare, "costume: death fanfare");
}
