// Course World's Super Worlds against our server (games/smm2/mods/mod.h; research/specs/super-worlds-v303.md).
//
// The game's Super World requests (DataStore SMM2 158-165, names from the game's own states) are answered from
// core/crates/server's /api/super_worlds routes, which hold the worlds TGRCode lists and the ones our players upload.
// A world's map travels as the game's own encrypted 0x500-byte blob: the game's converters decrypt it with their own
// key table, as for a NEX answer. The answers are written through the game's converters: a guest-side WorldMapInfo
// (the NEX record, 0x140 bytes) is filled from the server's JSON and handed to the list-entry converter
// (main+0x14d54d0) or the detail converter (main+0x14d48a0: reset, entry, decrypted map, data ids, ninjis). The
// world's courses are ordinary courses by data id (the course download, endless.c GetObject).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mod.h"
#include "../../../runtime/net/json.h"

#define NO_STATUS 0x60
#define NO_SW_LIST 0x1a9ad8      // world list ring: entries (0x248 bytes), +8 capacity, +0xc head, +0x10 count
#define SW_ENTRY 0x248
#define NO_SW_DETAIL 0x1b0d00    // the world detail record (0xa40 bytes)
#define NO_SW_PLAY 0x1b1740      // the play info record: id string object (char* +8, capacity +0x10), fields below
#define NO_SW_ID 0x1b17a8        // the world id to fetch: string object (char* +8, capacity +0x10)
#define NO_SW_SAVE_STATUS 0x1b17e8  // u32: the progress save's kind (1 a course cleared, 2 the world cleared)
#define NO_SW_UP_IDS 0x1b25c0    // upload: the courses' data ids (u64 ring: entries, +8 cap, +0xc head, +0x10 count)
#define NO_SW_UP_SLOT 0x1b2718   // upload: the Worldbot slot (map +0, thumbnail +0x500, +0xcd25 one byte)
#define NO_SW_UP_BUF 0x1b2720    // upload: 0x500-byte buffer for the encrypted map
#define NO_SW_WHICH 0x1b2728     // u8: the list asked for (0: My Info's, 160; 1: Course World's, 161)
#define CONVERT_ENTRY MAIN_ADDR(0x14d54d0)   // (entry 0x248, WorldMapInfo)
#define CONVERT_DETAIL MAIN_ADDR(0x14d48a0)  // (detail record 0xa40, WorldMapInfo)
#define ENCRYPT_FN MAIN_ADDR(0x8b3070)       // (key object, crypto block out, size, out, in): endless.c's
#define WORLD_KEY_TABLE MAIN_ADDR(0x23d71d0) // the world map's key table (courses: 0x23d68c0)
#define MAP_SIZE 0x500
#define MAP_PLAIN 0x4d0
#define THUMB_SIZE 0xc804
#define MAX_LEVELS 40

uint64_t mod_maker_pid(const char *maker_id, const char *name);
uint64_t mod_player_pid(void);
const char *mod_own_maker_id(void);

static void finish(Cpu *c, uint64_t no, bool ok, uint32_t status) {
    s32(no + NO_STATUS, ok ? 0 : status);
    c->x[0] = ok ? wrapper_ok() : wrapper_fail();
}

#define SW_WRAPPER(name, off)                                                                    \
    static void name##_impl(Cpu *c, uint64_t no);                                                \
    static void name(Cpu *c) {                                                                   \
        if (!mod_hundred()) { guest_call_orig(c, MAIN_ADDR(off)); return; }                      \
        name##_impl(c, c->x[0]);                                                                 \
    }                                                                                            \
    static void name##_impl(Cpu *c, uint64_t no)

// A guest string (char*), at most cap - 1 characters.
static void guest_str(uint64_t p, char *out, size_t cap) {
    size_t n = 0;
    for (; p && n + 1 < cap; n++) {
        char ch = (char)g8(p + n);
        if (!ch) break;
        out[n] = ch;
    }
    out[n] = 0;
}

// Write into a string object's own buffer (char* at +0, capacity at +8), as the converters do.
static void put_str(uint64_t buf, uint32_t cap, const char *s) {
    if (!buf || !cap) return;
    size_t n = strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy((void *)(uintptr_t)buf, s, n);
    s8(buf + n, 0);
}

static void put_utf16(uint64_t buf, uint32_t cap, const char *s) {  // (UTF-8 in; BMP only, else '?')
    uint32_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && n + 1 < cap;) {
        uint32_t cp = *p++;
        if (cp >= 0xC0) {
            int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : 1;
            cp &= 0x3F >> extra;
            while (extra-- && (*p & 0xC0) == 0x80) cp = cp << 6 | (*p++ & 0x3F);
            if (cp > 0xFFFF) cp = '?';
        }
        s16(buf + 2 * n++, (uint16_t)cp);
    }
    s16(buf + 2 * n, 0);
}

// A world id fit for a path (hex, '_', digits).
static bool safe_id(const char *id) {
    if (!*id || strlen(id) > 63) return false;
    for (const char *p = id; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_')) return false;
    return true;
}

// ---- the guest-side WorldMapInfo (the NEX record the converters read; offsets from main+0x14d54d0 / 0x14d48a0):
// +0x28 char* id, +0x38 owner pid, +0x58 / +0x60 map blob begin / end, +0xa0 char* thumbnail URL, +0xb0 u8, +0xb4 u32
// thumbnail size, +0xe8 char* thumbnail file name, +0xf8 worlds, +0xf9 levels, +0xfa planet type, +0x100 NEX
// DateTime (created), +0x108 / +0x110 data ids (u64) begin / end, +0x120 std::map<u8, u32> (ninjis; empty here:
// begin = the end node at +0x128, root +0x128 = 0, size +0x130 = 0), +0x138 u32 unk5, +0x13c u8 unk6, +0x13d u8 unk7.
// One at a time: the requests run on the network worker.
static uint64_t g_info, g_blob, g_ids, g_id_str, g_empty_str;

static void info_alloc(void) {
    if (g_info) return;
    g_info = (uint64_t)(uintptr_t)hle_alloc(0x140);
    g_blob = (uint64_t)(uintptr_t)hle_alloc(MAP_SIZE);
    g_ids = (uint64_t)(uintptr_t)hle_alloc(8 * MAX_LEVELS);
    g_id_str = (uint64_t)(uintptr_t)hle_alloc(64);
    g_empty_str = (uint64_t)(uintptr_t)hle_alloc(16);
}

// NEX DateTime: second | minute << 6 | hour << 12 | day << 17 | month << 22 | year << 26 (UTC)
static uint64_t nex_datetime(uint64_t t) {
    time_t tt = (time_t)t;
    struct tm tm;
    if (!gmtime_r(&tt, &tm)) return 0;
    return (uint64_t)tm.tm_sec | (uint64_t)tm.tm_min << 6 | (uint64_t)tm.tm_hour << 12 | (uint64_t)tm.tm_mday << 17 |
           (uint64_t)(tm.tm_mon + 1) << 22 | (uint64_t)(tm.tm_year + 1900) << 26;
}

// The record of world `w` (server JSON), its owner `pid`, its map (NULL: none) and courses (NULL: none).
static void info_fill(Json *w, uint64_t pid, const uint8_t *map, Json *courses) {
    info_alloc();
    uint64_t i = g_info;
    memset((void *)(uintptr_t)i, 0, 0x140);
    memset((void *)(uintptr_t)g_id_str, 0, 64);
    snprintf((char *)(uintptr_t)g_id_str, 64, "%s", json_str(json_get(w, "id"), ""));
    s64(i + 0x28, g_id_str);
    s64(i + 0x38, pid);
    if (map) memcpy((void *)(uintptr_t)g_blob, map, MAP_SIZE);
    s64(i + 0x58, g_blob);
    s64(i + 0x60, g_blob + (map ? MAP_SIZE : 0));
    s64(i + 0xa0, g_empty_str);  // (the thumbnail's address: Nintendo's CDN for TGRCode's worlds, never asked)
    s32(i + 0xb4, THUMB_SIZE);
    s64(i + 0xe8, g_empty_str);
    s8(i + 0xf8, (uint8_t)json_num(json_get(w, "worlds"), 1));
    s8(i + 0xf9, (uint8_t)json_num(json_get(w, "levels"), 0));
    s8(i + 0xfa, (uint8_t)json_num(json_get(w, "planet_type"), 0));
    s64(i + 0x100, nex_datetime((uint64_t)json_num(json_get(w, "created"), 0)));
    uint32_t n = 0;
    for (size_t k = 0; k < json_len(courses) && n < MAX_LEVELS; k++)
        s64(g_ids + 8ull * n++, (uint64_t)json_num(json_get(json_at(courses, k), "data_id"), 0));
    s64(i + 0x108, g_ids);
    s64(i + 0x110, g_ids + 8ull * n);
    s64(i + 0x120, i + 0x128);
    s32(i + 0x138, (uint32_t)json_num(json_get(w, "unk5"), 3));
    s8(i + 0x13c, (uint8_t)json_num(json_get(w, "unk6"), 1));
    s8(i + 0x13d, (uint8_t)json_num(json_get(w, "unk7"), 1));
}

static void call2(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1) {
    uint64_t sx[8];
    for (int k = 0; k < 8; k++) sx[k] = c->x[k];
    c->x[0] = a0;
    c->x[1] = a1;
    guest_call(c, fn);
    for (int k = 0; k < 8; k++) c->x[k] = sx[k];
}

static uint64_t owner_pid(Json *w) {
    return mod_maker_pid(json_str(json_get(w, "maker_id"), ""), json_str(json_get(w, "maker_name"), ""));
}

// ---- SearchWorldMap (160 / 161, main+0x14f79a0): flag +0x1b2728 1 -> Course World > Super Worlds (our pool), 0 ->
// My Info's list (the worlds the player has played). Into the list ring; the game asks for the owners next (GetUsers,
// world.c), whose names the player ids below remember.
SW_WRAPPER(sw_list, 0x14f79a0) {
    bool pickup = g8(no + NO_SW_WHICH) != 0;
    char path[96];
    Json *j = NULL;
    if (pickup) {
        j = srv_call_slow("GET", "/api/super_worlds?limit=50&wait_ms=20000", NULL, NULL, 26000);
    } else {
        mod_player_pid();  // (the own maker id, from /api/me once)
        snprintf(path, sizeof path, "/api/makers/%s/super_worlds", mod_own_maker_id());
        if (*mod_own_maker_id()) j = srv_call("GET", path, NULL, NULL);
    }
    Json *list = json_get(j, "super_worlds");
    size_t n = json_len(list);
    uint64_t pids[64];
    for (size_t k = 0; k < n && k < 64; k++) pids[k] = owner_pid(json_at(list, k));
    netobj_lock(c);
    uint64_t ring = no + NO_SW_LIST, entries = g64(ring);
    uint32_t cap = g32(ring + 8), count = 0;
    s64(ring + 0xc, 0);  // head 0, count 0
    for (size_t k = 0; k < n && k < 64 && count < cap && entries; k++) {
        uint64_t e = entries + (uint64_t)SW_ENTRY * count;
        info_fill(json_at(list, k), pids[k], NULL, NULL);
        s32(ring + 0x10, ++count);  // (as the original: the count goes up before the entry is converted)
        call2(c, CONVERT_ENTRY, e, g_info);
        s64(e + 0x230, (uint64_t)json_num(json_get(json_at(list, k), "created"), 0));
    }
    netobj_unlock(c);
    LOG("[mod] Super Worlds list (%s): %u worlds\n", pickup ? "Course World" : "My Info", count);
    bool ok = j != NULL || !pickup;
    json_free(j);
    finish(c, no, ok, 2);
}

// The world `id` (server JSON with its courses) and its map; false: not found.
static bool fetch_world(const char *id, Json **w, uint8_t **map) {
    char path[160];
    *w = NULL, *map = NULL;
    if (!safe_id(id)) return false;
    snprintf(path, sizeof path, "/api/super_worlds/%s?wait_ms=20000", id);
    *w = srv_call_slow("GET", path, NULL, NULL, 26000);
    if (!*w) return false;
    snprintf(path, sizeof path, "/api/super_worlds/%s/map", id);
    size_t len = 0;
    *map = srv_get_bytes(path, &len);
    if (*map && len != MAP_SIZE) free(*map), *map = NULL;
    if (!*map) LOG("[mod] Super World %s: no map\n", id);
    return *map != NULL;
}

// The detail record (NetObj +0x1b0d00) of world `id`, through the game's converter: the summary, the decrypted map,
// the courses' data ids, and the ninjis by course index. False: not found (the record is left as it was).
bool superworld_fill_detail(Cpu *c, uint64_t no, const char *id) {
    Json *w;
    uint8_t *map;
    if (!fetch_world(id, &w, &map)) {
        json_free(w);
        free(map);
        return false;
    }
    uint64_t pid = owner_pid(w);
    Json *courses = json_get(w, "courses"), *ninjis = json_get(w, "ninjis");
    uint32_t levels = (uint32_t)json_num(json_get(w, "levels"), 0);
    netobj_lock(c);
    uint64_t rec = no + NO_SW_DETAIL;
    info_fill(w, pid, map, courses);
    call2(c, CONVERT_DETAIL, rec, g_info);
    s64(rec + 0x230, (uint64_t)json_num(json_get(w, "created"), 0));
    // (+0x940..+0xa38 is the owner's user block: the game fills it itself when it copies the record into the profile,
    // main+0x14ba520, from the Course World list's entry of this world, whose owner GetUsers named; see title_setup)
    // ninjis: one per course (the source's list, when it has one per course)
    if (json_len(ninjis) == levels)
        for (uint32_t k = 0; k < levels && k < MAX_LEVELS; k++)
            s32(rec + 0x8a0 + 4ull * k, (uint32_t)json_num(json_at(ninjis, k), 0));
    netobj_unlock(c);
    LOG("[mod] Super World %s: %u worlds, %u courses, map %02x/%02x/%02x\n", id, g8(rec + 0x228), g32(rec + 0x758),
        g8(rec + 0x248 + 0x75), g8(rec + 0x248 + 0x76), g8(rec + 0x248 + 0x77));
    json_free(w);
    free(map);
    return true;
}

static void requested_id(Cpu *c, uint64_t no, uint64_t strobj, char *id, size_t cap) {
    netobj_lock(c);
    guest_str(g64(no + strobj + 8), id, cap);
    netobj_unlock(c);
}

// ---- GetWorldMap (159) into the detail record (main+0x14f6600): the world the player selected (id at +0x1b17a8)
SW_WRAPPER(sw_detail, 0x14f6600) {
    char id[64];
    requested_id(c, no, NO_SW_ID, id, sizeof id);
    bool ok = superworld_fill_detail(c, no, id);
    finish(c, no, ok, 2);
}

// ---- GetWorldMap (159) into the list ring (main+0x14f6100): one entry
SW_WRAPPER(sw_one, 0x14f6100) {
    char id[64], path[160];
    requested_id(c, no, NO_SW_ID, id, sizeof id);
    Json *w = NULL;
    if (safe_id(id)) {
        snprintf(path, sizeof path, "/api/super_worlds/%s?wait_ms=20000", id);
        w = srv_call_slow("GET", path, NULL, NULL, 26000);
    }
    uint64_t pid = w ? owner_pid(w) : 0;
    netobj_lock(c);
    uint64_t ring = no + NO_SW_LIST;
    s64(ring + 0xc, 0);
    if (w && g32(ring + 8) && g64(ring)) {
        info_fill(w, pid, NULL, NULL);
        s32(ring + 0x10, 1);
        call2(c, CONVERT_ENTRY, g64(ring), g_info);
        s64(g64(ring) + 0x230, (uint64_t)json_num(json_get(w, "created"), 0));
    }
    netobj_unlock(c);
    MLOG("Super World %s (list entry): %s\n", id, w ? "found" : "not found");
    finish(c, no, w != NULL, 2);
    json_free(w);
}

// ---- online progress (162 get, 164 reset, 165 save): the play record at +0x1b1740 (id string object, fields
// +0x1b1780..). Our server keeps it per player and world. The game resumes a world from it: the 162 answer is copied
// to the profile (+0x226110, main+0x14bb710) and the world's loader (main+0x148eae0) puts the score, lives, world,
// Mario's place and the clear bits back on the map when its status is 2; the title then offers Continue.
static const struct { const char *key; uint32_t off; int size; } PLAY_FIELDS[] = {
    {"counter_a", 0x1b1784, 1}, {"counter_b", 0x1b1785, 1}, {"score", 0x1b1788, 4}, {"world", 0x1b178c, 1},
    {"x", 0x1b178d, 1}, {"y", 0x1b178e, 1}, {"extra", 0x1b17a0, 4},
};

SW_WRAPPER(sw_progress_get, 0x14f7d10) {
    char id[64], path[160];
    requested_id(c, no, NO_SW_PLAY, id, sizeof id);
    Json *j = NULL;
    if (safe_id(id)) {
        snprintf(path, sizeof path, "/api/super_worlds/%s/progress", id);
        j = srv_call("GET", path, NULL, NULL);
    }
    netobj_lock(c);
    s32(no + 0x1b1780, (uint32_t)json_num(json_get(j, "status"), 0));
    for (size_t k = 0; k < sizeof PLAY_FIELDS / sizeof *PLAY_FIELDS; k++) {
        uint64_t v = (uint64_t)json_num(json_get(j, PLAY_FIELDS[k].key), 0);
        if (PLAY_FIELDS[k].size == 1) s8(no + PLAY_FIELDS[k].off, (uint8_t)v);
        else s32(no + PLAY_FIELDS[k].off, (uint32_t)v);
    }
    s64(no + 0x1b1790, strtoull(json_str(json_get(j, "cleared"), "0"), NULL, 10));
    s64(no + 0x1b1798, strtoull(json_str(json_get(j, "flags"), "0"), NULL, 10));
    s8(no + 0x1b17a4, 1);  // valid
    netobj_unlock(c);
    MLOG("Super World %s progress: %s, status %u\n", id, json_bool(json_get(j, "held"), false) ? "held" : "none",
         (uint32_t)json_num(json_get(j, "status"), 0));
    finish(c, no, j != NULL, 2);
    json_free(j);
}

SW_WRAPPER(sw_progress_save, 0x14f84d0) {
    char id[64], path[160], body[512];
    netobj_lock(c);
    guest_str(g64(no + NO_SW_PLAY + 8), id, sizeof id);
    // (the record's own status, set by the game's course end: 2 progress to continue, 1 none (the world cleared);
    // the save's kind beside it: 1 a course cleared, 2 the world cleared [obs main+0x1493950, the callback main+0x1524850])
    int n = snprintf(body, sizeof body, "{\"status\":%u,\"clear\":%u", g32(no + 0x1b1780), g32(no + NO_SW_SAVE_STATUS));
    for (size_t k = 0; k < sizeof PLAY_FIELDS / sizeof *PLAY_FIELDS; k++)
        n += snprintf(body + n, sizeof body - n, ",\"%s\":%u", PLAY_FIELDS[k].key,
                      PLAY_FIELDS[k].size == 1 ? g8(no + PLAY_FIELDS[k].off) : g32(no + PLAY_FIELDS[k].off));
    snprintf(body + n, sizeof body - n, ",\"cleared\":\"%llu\",\"flags\":\"%llu\"}",
             (unsigned long long)g64(no + 0x1b1790), (unsigned long long)g64(no + 0x1b1798));
    netobj_unlock(c);
    Json *j = NULL;
    if (safe_id(id)) {
        snprintf(path, sizeof path, "/api/super_worlds/%s/progress", id);
        j = srv_call("POST", path, body, NULL);
    }
    LOG("[mod] Super World %s progress saved: %s\n", id, j ? body : "failed (kept by the game)");
    json_free(j);
    finish(c, no, true, 0);  // (the game keeps its own copy either way)
}

SW_WRAPPER(sw_progress_reset, 0x14f8260) {
    char id[64], path[160], body[64];
    netobj_lock(c);
    guest_str(g64(no + NO_SW_PLAY + 8), id, sizeof id);
    snprintf(body, sizeof body, "{\"extra\":%u}", g32(no + 0x1b17a0));
    netobj_unlock(c);
    Json *j = NULL;
    if (safe_id(id)) {
        snprintf(path, sizeof path, "/api/super_worlds/%s/progress/reset", id);
        j = srv_call("POST", path, body, NULL);
    }
    LOG("[mod] Super World %s progress reset: %s\n", id, j ? "done" : "failed");
    json_free(j);
    finish(c, no, true, 0);
}

// ---- world thumbnails (87 + HTTP, main+0x14f8850): TGRCode's are on Nintendo's CDN only, never asked for. The
// request fails: the list shows the game's placeholder [obs: the callback's failure path, main+0x1525090].
SW_WRAPPER(sw_thumbnails, 0x14f8850) {
    MLOG("Super World thumbnails: none (placeholder)\n");
    finish(c, no, false, 2);
}

// ---- RegisterWorldMap (158, main+0x14f5940): the player's world to our server. The map is encrypted as the game
// does it (its own function and the world key table, a fresh IV and state), then sent with the thumbnail block and
// the courses' data ids (the player's own uploads on our server; the server checks).
SW_WRAPPER(sw_upload, 0x14f5940) {
    static uint64_t keyobj;
    if (!keyobj) keyobj = (uint64_t)(uintptr_t)hle_alloc(16);
    uint64_t slot = g64(no + NO_SW_UP_SLOT), buf = g64(no + NO_SW_UP_BUF);
    if (!slot || !buf) { finish(c, no, false, 2); return; }
    char query[600];
    netobj_lock(c);
    uint64_t ring = no + NO_SW_UP_IDS;
    uint32_t cap = g32(ring + 8), head = g32(ring + 0xc), count = g32(ring + 0x10);
    // (the game's RegisterWorldMap sends the world count (map byte 0x76), the slot's byte +0xcd25 and the map format
    // (0x75), in the order of a WorldMapInfo's worlds / planet type / unk5 [inf: +0xcd25 the planet type])
    int n = snprintf(query, sizeof query, "/api/super_worlds?worlds=%u&planet_type=%u&version=%u&data_ids=",
                     g8(slot + 0x76), g8(slot + 0xcd25), g8(slot + 0x75));
    for (uint32_t k = 0; k < count && k < MAX_LEVELS && cap; k++)
        n += snprintf(query + n, sizeof query - n, "%s%llu", k ? "," : "",
                      (unsigned long long)g64(g64(ring) + 8ull * ((head + k) % cap)));
    netobj_unlock(c);
    s64(keyobj, WORLD_KEY_TABLE);
    uint64_t sx[6];
    for (int k = 0; k < 6; k++) sx[k] = c->x[k];
    c->x[0] = keyobj;
    c->x[1] = buf + MAP_PLAIN;  // IV, state, CMAC
    c->x[2] = MAP_PLAIN;
    c->x[3] = buf;
    c->x[4] = slot;
    guest_call(c, ENCRYPT_FN);
    for (int k = 0; k < 6; k++) c->x[k] = sx[k];
    uint8_t *body = malloc(MAP_SIZE + THUMB_SIZE);
    memcpy(body, (const void *)(uintptr_t)buf, MAP_SIZE);
    memcpy(body + MAP_SIZE, (const void *)(uintptr_t)(slot + 0x500), THUMB_SIZE);
    int st = 0;
    Json *j = srv_post_bytes(query, body, MAP_SIZE + THUMB_SIZE, &st);
    free(body);
    LOG("[mod] Super World upload (%u courses): %s\n", count, j ? json_str(json_get(j, "id"), "?") : "refused");
    json_free(j);
    finish(c, no, j != NULL, 2);
}

// ---- DeleteWorldMap (163, main+0x14f7c30): the player's own world
SW_WRAPPER(sw_delete, 0x14f7c30) {
    Json *j = srv_call("DELETE", "/api/super_worlds/mine", NULL, NULL);
    LOG("[mod] Super World deleted: %s\n", j ? "yes" : "failed");
    finish(c, no, j != NULL, 2);
    json_free(j);
}

// ---- the world's title ("SUPER <name> WORLD", main+0x199fb60). The game names another maker's world after the owner
// block of its profile copy (profile +0x226010: name, +0x22606c: Mii, +0x226108: set), but first asks whether the
// save's own player name (save.dat +0x10, from the Mii the player picks for Course World) is still the default " "
// (main+0x168d300 against the constant at main+0x239f14a); if so it shows the account's nickname for every world.
// Our Mii database is empty (runtime/services/mii.c), so that name is never set and every world read "SUPER Player
// WORLD". For another maker's world with its owner known, the save name holds the account's nickname for the length
// of the call (as if the player had picked a Mii named so), then the default again: nothing else sees it.
#define WORLD_MGR MAIN_ADDR(0x2c0d558)   // the map's world manager: +0x48 the world played, entries (0x18) at +0x50
#define PROFILE MAIN_ADDR(0x2c115e0)
#define SAVE_HOLDER MAIN_ADDR(0x2a39088) // +0x20: save.dat's data (+0x10: the player's name, UTF-16, 11 units)
#define ACCOUNT MAIN_ADDR(0x2a395a8)     // +0x178: the account's nickname (string object, UTF-16 at +0x180)
static void title_setup(Cpu *c) {
    uint64_t w = g64(WORLD_MGR), prof = g64(PROFILE), holder = g64(SAVE_HOLDER), acct = g64(ACCOUNT);
    uint64_t save = holder ? g64(holder + 0x20) : 0, nick = acct ? g64(acct + 0x180) : 0;
    uint8_t keep[0x16];
    bool swap = false;
    if (mod_hundred() && w && prof && save && nick) {
        int32_t i = (int32_t)g32(w + 0x48);
        bool own = i == 1 && !g8(w + 0x7d);                  // (the Worldbot world being played: the save's name)
        bool other = i >= 0 && i < 8 && g8(w + 0x18ull * i + 0x65) && g8(prof + 0x226108);  // (a downloaded world)
        uint16_t c0 = *(volatile uint16_t *)(uintptr_t)(save + 0x10), c1 = *(volatile uint16_t *)(uintptr_t)(save + 0x12);
        if (!own && other && c0 == ' ' && c1 == 0) {
            memcpy(keep, (const void *)(uintptr_t)(save + 0x10), sizeof keep);
            uint32_t n = 0;
            for (; n < 10; n++) {
                uint16_t ch = *(volatile uint16_t *)(uintptr_t)(nick + 2 * n);
                if (!ch) break;
                s16(save + 0x10 + 2 * n, ch);
            }
            s16(save + 0x10 + 2 * n, 0);
            swap = n > 0;
            if (!swap) memcpy((void *)(uintptr_t)(save + 0x10), keep, sizeof keep);
        }
    }
    guest_call_orig(c, MAIN_ADDR(0x199fb60));
    if (swap) memcpy((void *)(uintptr_t)(save + 0x10), keep, sizeof keep);
    if (swap) MLOG("Super World title: the owner's name\n");
}

void superworld_register(void) {
    hle_override(MAIN_ADDR(0x199fb60), title_setup, "mod: Super World title (the owner's name)");
    hle_override(MAIN_ADDR(0x14f79a0), sw_list, "mod: SearchWorldMap (Super Worlds list)");
    hle_override(MAIN_ADDR(0x14f6600), sw_detail, "mod: GetWorldMap (Super World detail)");
    hle_override(MAIN_ADDR(0x14f6100), sw_one, "mod: GetWorldMap (Super World list entry)");
    hle_override(MAIN_ADDR(0x14f7d10), sw_progress_get, "mod: Super World play info");
    hle_override(MAIN_ADDR(0x14f84d0), sw_progress_save, "mod: Super World play info save");
    hle_override(MAIN_ADDR(0x14f8260), sw_progress_reset, "mod: Super World play info reset");
    hle_override(MAIN_ADDR(0x14f8850), sw_thumbnails, "mod: Super World thumbnails");
    hle_override(MAIN_ADDR(0x14f5940), sw_upload, "mod: RegisterWorldMap (Super World upload)");
    hle_override(MAIN_ADDR(0x14f7c30), sw_delete, "mod: DeleteWorldMap");
}
