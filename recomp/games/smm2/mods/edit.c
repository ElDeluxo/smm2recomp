// Edit any course (DECISIONS.md "Edit any course"): a downloaded course opens in SMM2's own Course Maker, as SMM1 let
// you edit the courses you downloaded. SMM1 offers "Edit" in Coursebot's window of a downloaded course (above
// "Play"); SMM2's window of a downloaded course has Play, Play Together and Delete only. This mod gives it SMM2's own
// "Make" button and sends it down the path My Courses' Make takes: the scene change to the Course Maker (reason 3,
// cRoboToEdit) with the slot's course number. The Course Maker loads a Coursebot slot the same way whichever list it
// is in (the loader copies the slot the Coursebot prepared), and its Save goes to My Courses as for any course, so
// the downloaded copy stays as it was.
//
// Addresses are SMM2 v3.0.3, offsets in main.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mod.h"
#include "../../../runtime/net/json.h"

// Coursebot scene, the Downloaded Courses list's machine (registered in main+0x16f2d70): state 5 cCourseInfoWindow
// (exec main+0x16e2db0), state 7 cToPlay (exec main+0x16e33c0, the scene change for Play: reason 5)
#define DL_INFO_EXEC MAIN_ADDR(0x16e2db0)
#define DL_TO_PLAY_EXEC MAIN_ADDR(0x16e33c0)
#define DL_TO_PLAY 7
#define STATE_CHANGE MAIN_ADDR(0x8b9320)      // (machine + 8, state)
#define SLOT_PREPARE MAIN_ADDR(0x16e44e0)     // (machine): the selected slot's course, thumbnail and replay -> the
                                              // save buffer's current-slot copy, which the course loader reads
#define SCENE_BUSY MAIN_ADDR(0x2a39080)       // *: +0x344 a scene change is under way
#define UI_INFO MAIN_ADDR(0x2e53b18)          // *: +0x70 -> +0x38 the selected slot's course number
#define UI_EVENTS MAIN_ADDR(0x2c52c20)        // *: this frame's events: +0x718 count, +0x720 -> {u32 id, u64 arg}*
#define FADE_BUSY MAIN_ADDR(0x8b4410)         // () -> bit 0: no scene change now
#define REQ_BEGIN MAIN_ADDR(0x1792070)        // (reason, &{u64 -1, u32 -1})
#define REQ_PLAY_STATS MAIN_ADDR(0x1790480)   // (reason): a downloaded course's play stats on leaving it
#define REQ_COURSE MAIN_ADDR(0x1792890)       // (reason, course number, &{...})
#define REQ_COMMIT MAIN_ADDR(0x1791020)       // (0)
#define REASON_TO_EDIT 3                      // cRoboToEdit
#define EV_MAKE 0x130                         // the info window's Make (L_Edit_00) [obs My Courses: 0x130 -> 0x1a2]

static bool edit_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_MOD_EDIT");
        on = e ? atoi(e) != 0 : 1;
    }
    return on;
}

static bool edit_pending;  // the Downloaded list's cToPlay is going to the Course Maker, not to play

static int32_t selected_slot(void) {
    uint64_t info = g64(UI_INFO);
    if (!info || !g64(info + 0x70)) return -1;
    return (int32_t)g32(g64(info + 0x70) + 0x38);
}

static bool ui_event(uint32_t id) {
    uint64_t ev = g64(UI_EVENTS);
    if (!ev) return false;
    int32_t n = (int32_t)g32(ev + 0x718);  // (the buttons' events; +0x3d8: the scenes' own [obs main+0x16e4880])
    uint64_t list = g64(ev + 0x720);
    for (int32_t i = 0; i < n && list; i++)
        if (g64(list + 8ull * i) && g32(g64(list + 8ull * i)) == id) return true;
    return false;
}

static void call1(Cpu *c, uint64_t fn, uint64_t a0) { c->x[0] = a0; guest_call(c, fn); }

// The scene change to the Course Maker with the selected slot, as My Courses' cToEdit does it (main+0x16e5c10).
static void request_edit(Cpu *c, int32_t slot) {
    uint64_t sp = c->sp;
    c->sp -= 0x20;
    uint64_t params = c->sp;
    s64(params, ~0ull), s32(params + 8, ~0u);
    c->x[0] = REASON_TO_EDIT, c->x[1] = params, guest_call(c, REQ_BEGIN);
    call1(c, REQ_PLAY_STATS, REASON_TO_EDIT);
    s64(params, ~0ull), s32(params + 8, ~0u);
    c->x[0] = REASON_TO_EDIT, c->x[1] = (uint32_t)slot, c->x[2] = params, guest_call(c, REQ_COURSE);
    call1(c, REQ_COMMIT, 0);
    c->sp = sp;
}

// ---- the copy the Course Maker gets, and where it came from
//
// The prepared slot (save buffer +0x6c000: the course file as the loader takes it, plain: u32 1, u16 0x10, CRC32 of
// the 0x5bfc0 bytes from +0x10 at +8, "SCDL", then the course) is the downloaded course itself, uploaded and maybe
// cleared by its maker. The copy for the player is made a course of their own, as the game does with its built-in
// courses (main+0x100e3ba90): upload id 0 and management flags bits 1 and 4 cleared (so it can be uploaded again,
// as a new course), no clear check yet. Its name and description stay.
//
// The copy gets a creation id of its own (header +0x24; the Course Maker keeps it through Save As), and the
// original's code and maker are kept in smm2edit.txt (the data folder) under it, so an upload of the edited copy
// can credit them (edit_upload_query). Nothing about the original changes on the server.
#define SAVE_MGR MAIN_ADDR(0x2a39088)      // *: +0x20 the save buffer
#define SLOT_COURSE 0x6c000                // save buffer: the prepared slot's course
#define LATER_RECORDS 0xc010               // save buffer: downloaded-course records (0x2a8): +0x08 data id, +0x38 name,
#define LATER_RECORD 0x2a8                 // +0x144 course ID, +0x16c maker name (UTF-16) [obs]
#define COURSE_BODY 0x5bfc0
#define EDIT_ORIGINS "smm2edit.txt"

static uint32_t crc32_of(const uint8_t *p, size_t n) {
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

static void utf16_ascii(uint64_t p, char *out, size_t cap) {  // (names: anything past ASCII becomes '?')
    size_t n = 0;
    for (uint32_t i = 0; n + 1 < cap; i++) {
        uint16_t ch = *(volatile uint16_t *)(uintptr_t)(p + 2 * i);
        if (!ch) break;
        out[n++] = ch >= 0x20 && ch < 0x7f && ch != '\t' ? (char)ch : '?';
    }
    out[n] = 0;
}

static void remember_origin(uint32_t creation, const char *code, const char *maker) {
    FILE *f = fopen(EDIT_ORIGINS, "a");
    if (!f) { LOG("[mod] edit: can't write %s\n", EDIT_ORIGINS); return; }
    fprintf(f, "%08x\t%s\t%s\n", creation, code, maker);  // creation id, original code, original maker
    fclose(f);
}

// The original of a course by its creation id (the last line for it wins); false: an original course.
bool edit_origin(uint32_t creation, char code[16], char maker[48]) {
    FILE *f = fopen(EDIT_ORIGINS, "r");
    if (!f) return false;
    char line[160], c[16], m[48];
    bool found = false;
    unsigned id;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t2 || sscanf(line, "%x", &id) != 1 || id != creation) continue;
        snprintf(c, sizeof c, "%.*s", (int)(t2 - t1 - 1), t1 + 1);
        snprintf(m, sizeof m, "%s", t2 + 1);
        memcpy(code, c, 16), memcpy(maker, m, 48), found = true;
    }
    fclose(f);
    return found;
}

// The query an upload of the course (plain body, header at +0) adds: "?based_on=<code>&based_on_maker=<name>" for an
// edited copy of someone else's course, "" otherwise (world.c, the course upload).
void edit_upload_query(const uint8_t *course, char *out, size_t cap) {
    char code[16], maker[48], esc[160];
    out[0] = 0;
    uint32_t creation;
    memcpy(&creation, course + 0x24, 4);
    if (!edit_origin(creation, code, maker)) return;
    size_t k = 0;
    for (const char *p = maker; *p && k + 4 < sizeof esc; p++) {
        unsigned char ch = (unsigned char)*p;
        if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) esc[k++] = (char)ch;
        else k += (size_t)snprintf(esc + k, sizeof esc - k, "%%%02X", ch);
    }
    esc[k] = 0;
    snprintf(out, cap, "?based_on=%s&based_on_maker=%s", code, esc);
}

// A course's description in Course World's lists and details: an edited copy (the server's based_on) gets the
// credit line under its own description (the game shows two lines; a long description pushes the credit out).
const char *edit_listing_description(Json *m, char *buf, size_t cap) {
    const char *desc = json_str(json_get(m, "description"), ""), *orig = json_str(json_get(m, "based_on"), "");
    if (!*orig) return desc;
    const char *by = json_str(json_get(m, "based_on_maker"), "");
    snprintf(buf, cap, "%s%sBased on %s%s%s", desc, *desc ? "\n" : "", orig, *by ? " by " : "", by);
    return buf;
}

// A course file (plain) made the player's own: a new creation id, management flags 1 and 4 cleared, no clear check,
// upload id 0, the CRC again.
static void make_own(uint64_t file, uint32_t creation) {
    uint64_t hdr = file + 0x10;
    s32(hdr + 0x24, creation);
    s32(hdr + 0x18, g32(hdr + 0x18) & ~0x12u);
    s32(hdr + 0x1c, 0), s32(hdr + 0x20, 0xffffffffu);
    s64(hdr + 0x28, 0);
    s32(file + 8, crc32_of((const uint8_t *)(uintptr_t)hdr, COURSE_BODY));
}

static void prepare_copy(int32_t slot) {
    uint64_t save = g64(SAVE_MGR) ? g64(g64(SAVE_MGR) + 0x20) : 0;
    if (!save) return;
    uint64_t file = save + SLOT_COURSE, hdr = file + 0x10, rec = save + LATER_RECORDS + (uint64_t)LATER_RECORD * (uint64_t)(slot - 0x78);
    if (g32(file) != 1 || memcmp((const void *)(uintptr_t)(file + 0xc), "SCDL", 4)) {
        LOG("[mod] edit: the prepared course isn't a course file\n");
        return;
    }
    char code[12] = "", maker[48] = "";
    uint32_t id = (uint32_t)g64(rec + 0x08);
    if (id) data_id_to_code(id, code);
    utf16_ascii(rec + 0x16c, maker, sizeof maker);
    // a creation id of its own (a new course gets a random one [obs blank course]; the copy keeps it, which makes
    // it the copy's key here)
    static uint32_t seq;
    uint32_t creation = (uint32_t)time(NULL) * 2654435761u ^ (++seq * 0x9e3779b9u) ^ (uint32_t)id;
    if (!creation) creation = 1;
    LOG("[mod] edit: %s by %s (creation id %08x -> %08x, upload id %llu, flags %#x) -> a course of your own\n",
        code[0] ? code : "?", maker[0] ? maker : "?", g32(hdr + 0x24), creation, (unsigned long long)g64(hdr + 0x28),
        g32(hdr + 0x18));
    make_own(file, creation);
    // the Coursebot also keeps the selected slot's whole data (course, thumbnail, replay: 0xe0000) at save manager
    // +0x58 for its "current course" (+0x60); the loader takes that copy when the number is the current one
    uint64_t mgr = g64(SAVE_MGR), cur = mgr ? g64(mgr + 0x58) : 0;
    bool current = cur && (int32_t)g32(mgr + 0x60) == slot && g32(cur) == 1 &&
                   !memcmp((const void *)(uintptr_t)(cur + 0xc), "SCDL", 4);
    if (current) make_own(cur, creation);
    MLOG("edit: current course %d%s\n", mgr ? (int32_t)g32(mgr + 0x60) : -1, current ? " (its copy too)" : "");
    if (code[0]) remember_origin(creation, code, maker);
}

static void dl_to_play(Cpu *c) {
    const char *test = getenv("RECOMP_EDIT_PLAY_IS_EDIT");  // (tests: the Downloaded list's Play opens the editor)
    if (!edit_on() || (!edit_pending && !(test && *test == '1'))) { guest_call_orig(c, c->pc); return; }
    uint64_t machine = c->x[0], lr = c->x[30];
    if (g64(SCENE_BUSY) && g8(g64(SCENE_BUSY) + 0x344)) return;
    call1(c, SLOT_PREPARE, machine);
    int32_t slot = selected_slot();
    c->x[0] = 0;
    guest_call(c, FADE_BUSY);
    if (!(c->x[0] & 1) && slot >= 0x78 && slot < 0xb4) {
        LOG("[mod] edit: downloaded course in slot %d -> Course Maker\n", slot - 0x78);
        prepare_copy(slot);
        request_edit(c, slot);
        edit_pending = false;
    }
    c->x[30] = lr;
}

static void dl_info(Cpu *c) {
    if (edit_on() && ui_event(EV_MAKE)) {
        uint64_t machine = c->x[0];
        LOG("[mod] edit: Make on a downloaded course (slot %d)\n", selected_slot() - 0x78);
        edit_pending = true;
        c->x[0] = machine + 8, c->x[1] = DL_TO_PLAY;
        guest_call(c, STATE_CHANGE);
        return;
    }
    if (getenv("RECOMP_EDIT_TRACE")) {  // (tests: the UI events the window sees)
        uint64_t ev = g64(UI_EVENTS);
        int32_t n = ev ? (int32_t)g32(ev + 0x718) : 0;
        for (int32_t i = 0; i < n; i++) LOG("[mod] edit: UI event %#x\n", g32(g64(g64(ev + 0x720) + 8ull * i)));
    }
    guest_call_orig(c, c->pc);
}

// ---- Course World's Download against our server. It fetches the course and then its thumbnail (a relation object:
// main+0x14e0e70, GetReqGetInfoHeadersInfo (134) and an HTTP GET per request in the ring at NetObj +0x74bc8, each
// into the buffer at *(NetObj +0x8e608)[i]; the job's callback main+0x1520800 puts each into the thumbnail cache,
// which checks it). The download waits for a valid cache entry (cDownload, main+0x17661f0) and stops at "A
// communication error occurred." without one.
//
// A stand-in thumbnail: SMM2 only keeps a course in the Coursebot with a thumbnail it can check (the thumbnail
// block: JPEG at +0, its size at +0x1bf9c (1 .. 0x1bf9c), HMAC-SHA256 of the JPEG at +0x1bfa0 under a key made from
// four seed words at +0x1bfc0; main+0x1698a80 checks it, and a failed check deletes the course: "Corrupt data was
// found so the course has been deleted."). Our server has no thumbnails, so a download gets a plain one: a
// 640 x 360 JPEG of one colour, encoded here (baseline, 4:4:4, every block DC only), signed with the game's own
// functions as its blank thumbnail is (main+0x1766830).
#define RNG_INIT MAIN_ADDR(0x29a050)       // (state)
#define RNG_SEEDS MAIN_ADDR(0x29a1c0)      // (state, &w0, &w1, &w2, &w3)
#define THUMB_KEY MAIN_ADDR(0x168c6d0)     // (&table, key out[16], state)
#define THUMB_KEY_TABLE MAIN_ADDR(0x23d6ed0)
#define HMAC_SHA256 MAIN_ADDR(0x1cba7f0)   // nn::crypto::GenerateHmacSha256Mac(mac, 32, data, size, key, 16)

typedef struct { uint8_t *p; size_t n, cap; uint32_t acc; int bits; } Bits;
static void put8(Bits *b, uint8_t v) { if (b->n < b->cap) b->p[b->n++] = v; }
static void put16(Bits *b, uint16_t v) { put8(b, v >> 8), put8(b, v & 0xff); }
static void putbits(Bits *b, uint32_t code, int len) {
    for (int i = len - 1; i >= 0; i--) {
        b->acc = b->acc << 1 | ((code >> i) & 1);
        if (++b->bits == 8) {
            put8(b, (uint8_t)b->acc);
            if ((b->acc & 0xff) == 0xff) put8(b, 0);  // (byte stuffing)
            b->acc = 0, b->bits = 0;
        }
    }
}

// JPEG of one colour (r, g, b) -> out (cap bytes); returns its size
static size_t solid_jpeg(uint8_t *out, size_t cap, int w, int h, uint8_t r, uint8_t g, uint8_t bl) {
    Bits b = {out, 0, cap, 0, 0};
    double yy = 0.299 * r + 0.587 * g + 0.114 * bl, cb = 128 - 0.168736 * r - 0.331264 * g + 0.5 * bl,
           cr = 128 + 0.5 * r - 0.418688 * g - 0.081312 * bl;
    int dc[3] = {(int)(yy + 0.5) - 128, (int)(cb + 0.5) - 128, (int)(cr + 0.5) - 128};  // (quantizer 8: DC = v - 128)
    put16(&b, 0xFFD8);
    static const uint8_t jfif[] = {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
    put16(&b, 0xFFE0), put16(&b, 2 + sizeof jfif);
    for (size_t i = 0; i < sizeof jfif; i++) put8(&b, jfif[i]);
    put16(&b, 0xFFDB), put16(&b, 67), put8(&b, 0);  // one quantization table, all 8
    for (int i = 0; i < 64; i++) put8(&b, 8);
    put16(&b, 0xFFC0), put16(&b, 17), put8(&b, 8), put16(&b, (uint16_t)h), put16(&b, (uint16_t)w), put8(&b, 3);
    for (int i = 1; i <= 3; i++) put8(&b, (uint8_t)i), put8(&b, 0x11), put8(&b, 0);
    // DC table: the standard luminance DC code (categories 0-11); AC table: only end-of-block, code "0"
    static const uint8_t dc_bits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
    put16(&b, 0xFFC4), put16(&b, 2 + 17 + 12 + 17 + 1), put8(&b, 0x00);
    for (int i = 0; i < 16; i++) put8(&b, dc_bits[i]);
    for (int i = 0; i < 12; i++) put8(&b, (uint8_t)i);
    put8(&b, 0x10), put8(&b, 1);
    for (int i = 1; i < 16; i++) put8(&b, 0);
    put8(&b, 0x00);
    put16(&b, 0xFFDA), put16(&b, 12), put8(&b, 3);
    for (int i = 1; i <= 3; i++) put8(&b, (uint8_t)i), put8(&b, 0x00);
    put8(&b, 0), put8(&b, 63), put8(&b, 0);
    static const uint16_t dc_code[12] = {0x0, 0x2, 0x3, 0x4, 0x5, 0x6, 0xe, 0x1e, 0x3e, 0x7e, 0xfe, 0x1fe};
    static const uint8_t dc_len[12] = {2, 3, 3, 3, 3, 3, 4, 5, 6, 7, 8, 9};
    int mcus = ((w + 7) / 8) * ((h + 7) / 8);
    for (int m = 0; m < mcus; m++)
        for (int k = 0; k < 3; k++) {
            int d = m ? 0 : dc[k], a = d < 0 ? -d : d, s = 0;
            while (a >> s) s++;
            putbits(&b, dc_code[s], dc_len[s]);
            if (s) putbits(&b, (uint32_t)(d < 0 ? d - 1 : d) & ((1u << s) - 1), s);
            putbits(&b, 0, 1);  // end of block
        }
    if (b.bits) putbits(&b, 0x7f, 8 - b.bits);  // (pad with ones)
    put16(&b, 0xFFD9);
    return b.n <= cap ? b.n : 0;
}

// A thumbnail block the game accepts at guest address dst (0x1c000 bytes), signed on this thread's guest stack.
static bool make_thumbnail(Cpu *c, uint64_t dst) {
    static uint8_t jpeg[0x4000];
    static size_t jpeg_len;
    if (!jpeg_len) jpeg_len = solid_jpeg(jpeg, sizeof jpeg, 640, 360, 0xb4, 0xc8, 0xdc);  // [inf] a grey-blue card
    if (!jpeg_len) return false;
    memset((void *)(uintptr_t)dst, 0, 0x1c000);
    memcpy((void *)(uintptr_t)dst, jpeg, jpeg_len);
    s32(dst + 0x1bf9c, (uint32_t)jpeg_len);
    uint64_t sp = c->sp, lr = c->x[30], save[6];
    for (int i = 0; i < 6; i++) save[i] = c->x[i];
    c->sp -= 0x80;
    uint64_t st = c->sp, key = c->sp + 0x40, table = c->sp + 0x60;
    c->x[0] = st, guest_call(c, RNG_INIT);
    c->x[0] = st, c->x[1] = dst + 0x1bfc0, c->x[2] = dst + 0x1bfc4, c->x[3] = dst + 0x1bfc8, c->x[4] = dst + 0x1bfcc;
    guest_call(c, RNG_SEEDS);
    s64(table, THUMB_KEY_TABLE);
    c->x[0] = table, c->x[1] = key, c->x[2] = st, guest_call(c, THUMB_KEY);
    c->x[0] = dst + 0x1bfa0, c->x[1] = 0x20, c->x[2] = dst, c->x[3] = jpeg_len, c->x[4] = key, c->x[5] = 0x10;
    guest_call(c, HMAC_SHA256);
    c->sp = sp, c->x[30] = lr;
    for (int i = 0; i < 6; i++) c->x[i] = save[i];
    return true;
}

static void relation_get(Cpu *c) {
    if (!mod_hundred()) { guest_call_orig(c, c->pc); return; }
    uint64_t no = c->x[0], thumb = 0;
    netobj_lock(c);
    uint64_t ring = no + 0x74bc8, entries = g64(ring), outs = g64(no + 0x8e608), downloading = g64(no + 0x8e5f0);
    uint32_t cap = g32(ring + 8), head = g32(ring + 0xc), count = g32(ring + 0x10), nout = g32(no + 0x8e600);
    for (uint32_t i = 0; i < count && cap && entries; i++) {
        uint64_t id = g64(entries + 0x1a8ull * ((head + i) % cap)), out = i < nout && outs ? g64(outs + 8ull * i) : 0;
        if (!out) continue;
        if (id == downloading && !thumb) thumb = out;  // the course being downloaded: the stand-in
        else s32(out + 0x1bf9c, 0);  // the lists' thumbnails: none (the game shows its placeholder)
    }
    s32(no + 0x60, 0);
    netobj_unlock(c);
    if (thumb) MLOG("thumbnail for course %llu: a stand-in (%s)\n", (unsigned long long)downloading,
                    make_thumbnail(c, thumb) ? "made" : "failed");
    c->x[0] = wrapper_ok();
}

// ---- the Make button in the window of a downloaded course
//
// The window (Lcl_CourseInfoWindow_00) is one layout for both lists: its TypeMode animation shows the My Courses
// buttons (N_MyDataBtn_00: Make, Upload, Name, Description, Delete, Play Together, Play) or the downloaded-course
// buttons and stats (N_DLData_00), and main+0x1891880 (window, list mode, on) switches off every button but the
// shown list's (mode 6, a downloaded course: Play, Delete, Play Together, ID, close). My Courses' Make (L_Edit_00) is
// bound in both, so in a downloaded course's window this shows N_MyDataBtn_00 with only L_Edit_00 in it, at the top
// of the button column where My Courses has it (between the window's header and the stats, a little smaller to fit
// there), switches it on (main+0x761190, as main+0x1891880 does), and its press (UI event 0x130) goes to the edit
// above: touch and the cursor reach it like the window's own buttons. Done after the window machine's own update
// each frame (the game switches the buttons again when it refreshes the window), undone for a My Courses slot.
//
// ui2d panes [obs]: +0x08 sibling link {prev, next}, +0x18 parent, +0x20 child list head, +0x30 translate x / y,
// +0x48 scale x / y, +0x58 flags (bit 0 visible), +0xb0 name. The window: +0x238 its 25 parts (the panes named in
// the table at main+0x2cbf080; 3 = L_Edit_00) -> part +0x58 its button -> +0x48 its pane; +0x268 its 13 buttons
// (the table at main+0x2cbf480; 1 = L_Edit_00).
#define INFO_APPEAR_EXEC MAIN_ADDR(0x188c980)  // the info window machine (main+0x18801c0): Appear, fills the window
#define INFO_DISP_EXEC MAIN_ADDR(0x188cb10)    // ... Disp, while it is up; this +0x228: the window
#define BUTTON_ENABLE MAIN_ADDR(0x761190)      // (button, 0, on)
#define PART_EDIT 3
#define BUTTON_EDIT 1
#define MAKE_Y 121.0f      // [inf] L_Edit_00's place in N_MyDataBtn_00 for a downloaded course (My Courses: 104)
#define MAKE_SCALE 0.72f   // [inf] 60 px tall on screen -> 43: fits between the header and the stats

static uint64_t part_pane(uint64_t win, int i, const char *name) {
    uint64_t parts = g64(win + 0x238);
    if (!parts || (uint32_t)i >= g32(win + 0x230)) return 0;
    uint64_t part = g64(parts + 8ull * i), btn = part ? g64(part + 0x58) : 0, pane = btn ? g64(btn + 0x48) : 0;
    if (!pane || strncmp((const char *)(uintptr_t)(pane + 0xb0), name, 24)) return 0;
    return pane;
}

static float gf(uint64_t a) { float f; memcpy(&f, (const void *)(uintptr_t)a, 4); return f; }
static void sf(uint64_t a, float f) { memcpy((void *)(uintptr_t)a, &f, 4); }
static void set_visible(uint64_t pane, bool on) { s8(pane + 0x58, (uint8_t)((g8(pane + 0x58) & ~1u) | (on ? 1u : 0u))); }

static struct { uint64_t pane; float y, sx, sy; uint64_t hidden[8]; int nhidden; bool moved; } make_saved;

static void make_button(Cpu *c, uint64_t win, bool downloaded) {
    uint64_t make = part_pane(win, PART_EDIT, "L_Edit_00"), group = make ? g64(make + 0x18) : 0;
    uint64_t buttons = g64(win + 0x268), button = buttons && g32(win + 0x260) > BUTTON_EDIT ? g64(buttons + 8 * BUTTON_EDIT) : 0;
    if (!make || !group || !button) return;
    if (make_saved.pane != make) {  // (a new window: its own values)
        memset(&make_saved, 0, sizeof make_saved);
        make_saved.pane = make;
        make_saved.y = gf(make + 0x34), make_saved.sx = gf(make + 0x48), make_saved.sy = gf(make + 0x4c);
    }
    if (!downloaded) {
        if (make_saved.moved) {
            sf(make + 0x34, make_saved.y), sf(make + 0x48, make_saved.sx), sf(make + 0x4c, make_saved.sy);
            for (int i = 0; i < make_saved.nhidden; i++) set_visible(make_saved.hidden[i], true);
            make_saved.nhidden = 0, make_saved.moved = false;
        }
        return;
    }
    if (!make_saved.moved) {  // the other My Courses buttons: hidden
        uint64_t head = group + 0x20;
        for (uint64_t n = g64(head + 8); n && n != head && make_saved.nhidden < 8; n = g64(n + 8)) {
            uint64_t pane = n - 8;
            if (pane != make && (g8(pane + 0x58) & 1)) make_saved.hidden[make_saved.nhidden++] = pane;
        }
        for (int i = 0; i < make_saved.nhidden; i++) set_visible(make_saved.hidden[i], false);
        make_saved.moved = true;
    }
    sf(make + 0x34, MAKE_Y), sf(make + 0x48, MAKE_SCALE), sf(make + 0x4c, MAKE_SCALE);
    set_visible(make, true);
    set_visible(group, true);
    static int flag = -1;
    if (flag < 0) flag = getenv("RECOMP_EDIT_ENABLE_FLAG") ? atoi(getenv("RECOMP_EDIT_ENABLE_FLAG")) : 1;
    if (flag < 2) {
        uint64_t lr = c->x[30];
        c->x[0] = button, c->x[1] = 0, c->x[2] = (uint64_t)flag;
        guest_call(c, BUTTON_ENABLE);
        c->x[30] = lr;
    }
}

static void info_window(Cpu *c) {
    uint64_t self = c->x[0];
    guest_call_orig(c, c->pc);
    uint64_t win = g64(self + 0x228);
    int32_t slot = selected_slot();
    if (win && edit_on() && slot >= 0 && slot < 0xb4) make_button(c, win, slot >= 0x78);
}

__attribute__((constructor)) static void edit_register(void) {
    hle_override(INFO_APPEAR_EXEC, info_window, "mod: Coursebot info window Appear (edit)");
    hle_override(INFO_DISP_EXEC, info_window, "mod: Coursebot info window Disp (edit)");
    hle_override(MAIN_ADDR(0x14e0e70), relation_get, "mod: relation object (thumbnail) download");
    hle_override(DL_INFO_EXEC, dl_info, "mod: Coursebot downloaded cCourseInfoWindow (edit)");
    hle_override(DL_TO_PLAY_EXEC, dl_to_play, "mod: Coursebot downloaded cToPlay (edit)");
}
