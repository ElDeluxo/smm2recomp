// The mods' shared RomFS overlay: files generated at the first launch that needs them, from the player's own game
// files, into <data folder>/mods-generated/romfs-v<N>/ (private, never in the repository), and served over the game's
// RomFS (game_romfs_overlay(), runtime/mods.c and runtime/services/fs.c: a file there replaces the game's).
//
// A feature registers a generator (a name, a version, a function); the set of names and versions is the overlay's
// identity: when it changes, everything is generated again (into a fresh folder) before the game opens its RomFS.
// Generators run one after the other on the boot thread; a later one sees what an earlier one wrote (rov_read, the
// archives), so two features may edit the same archive.
//
//   static bool gen(void) {
//       RovSarc *s = rov_sarc_load("Message/LayoutMsg_USen.szs");
//       bool ok = s && rov_msbt_set(rov_sarc_member(s, "Nwk_Title_00.msbt"), "L_EndlessMode_00-T_Btn_00", "...");
//       return rov_sarc_store(s, ok ? "Message/LayoutMsg_USen.szs" : NULL) && ok;
//   }
//   ROV_GENERATOR("challenge-modes", 1, gen)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { uint8_t *d; size_t n; } RovBuf;  // malloc'd bytes (d may be realloc'd by the editors)

typedef bool (*RovGenerator)(void);  // false: failed (logged; the others still run, the overlay still serves theirs)
void rov_register(const char *name, int version, RovGenerator fn);
#define ROV_GENERATOR(name, version, fn) \
    __attribute__((constructor)) static void rov_reg_##fn(void) { rov_register(name, version, fn); }

// ---- files (rel: a RomFS path, "Layout/Nwk_Title_00.Nin_NX_NVN.szs")
bool rov_read(const char *rel, RovBuf *out);         // the overlay's version if generated already, else the game's
bool rov_read_game(const char *rel, RovBuf *out);    // the game's own (extracted folder or romfs.bin)
bool rov_write(const char *rel, const void *d, size_t n);  // add or replace a file in the overlay
void rov_free(RovBuf *b);

// ---- archives: SARC, Yaz0-compressed or not, nested (a layout's .szs holds one .arc SARC)
typedef struct RovSarc RovSarc;
RovSarc *rov_sarc_load(const char *rel);           // rov_read + Yaz0 decode + parse; NULL: missing / damaged
RovSarc *rov_sarc_parse(const RovBuf *b);           // from bytes (a nested archive: a member's data)
RovBuf *rov_sarc_member(RovSarc *s, const char *name);  // its bytes, editable in place; NULL: no such member
const char *rov_sarc_name(RovSarc *s, int i);      // the i-th member's name (NULL past the end)
RovBuf rov_sarc_build(RovSarc *s);                  // SARC bytes again (members at their old alignment)
// write the archive to rel (Yaz0, uncompressed, if it was Yaz0) and free it; rel NULL: just free. False: write failed.
bool rov_sarc_store(RovSarc *s, const char *rel);

// ---- MSBT
// the text of an existing label replaced (UTF-8 in, UTF-16LE out); false: no such label / damaged
bool rov_msbt_set(RovBuf *msbt, const char *label, const char *text);

// ---- BFLYT: walk the sections (the edit-any-course helpers build on this: copy / move panes, set texts)
typedef struct { const char *magic; size_t off, size; } RovSection;  // magic "pan1", "prt1", "txt1", ...; off into b
int rov_flyt_sections(const RovBuf *b, RovSection *out, int cap);      // the count; -1: not a BFLYT
bool rov_flyt_insert(RovBuf *b, size_t at, const void *sec, size_t n);  // a section at byte offset at (header updated)
