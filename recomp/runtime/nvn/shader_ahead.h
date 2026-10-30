#pragma once
// Shader compile-ahead (both backends): the game's shaders, read from its own files in the RomFS, so that a backend
// can translate and compile them before the game first uses them (recomp/docs/smm2-bringup.md "Shader compile-ahead";
// DECISIONS.md).
//
// The scan walks the RomFS folder (files in the order shader_ahead_rank() gives, then by name; a file the game is
// reading right now first), opens SARC archives (Yaz0 or zstd compressed, nested), and finds the NVN shader code in
// them: every 256-byte aligned blob with the NVN code header (0x12345678, zeros, then a Maxwell SPH) is one vertex or
// fragment shader, exactly as the game hands it to nvnProgramSetShaders. That covers the BNSH archives in models and
// layouts as much as agl's .sharcb and the effects' .ptcl, whose program tables differ: shaders are taken alone, not
// as programs. A fragment shader's translation depends on its vertex shader only for inputs that one doesn't write,
// so alone it's translated as if all were written - as most of the game's programs pair them. The BNSH variations
// (models' shader archives) are also taken whole, as the programs they are (that covers the rest, and the programs
// with a geometry stage, which Metal draws as one vertex function); effects pair their shaders otherwise. Shaders are
// deduplicated by their bytes (most models carry the same few). Nothing is kept: each file is read, its shaders passed
// on, freed.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int stage;               // 0 vertex, 1 fragment (code[stage]), or 2: a whole program (code[0], code[1], and code[2]
                             // when it has a geometry stage)
    const uint8_t *code[3];  // vertex, fragment, geometry; each readable for cmax[i] bytes
    size_t cmax[3];          // the translation bound (up to the next shader or the compiler constants)
    const char *archive;     // where it was found first (RomFS-relative, members after ':')
    int rank;                // its file's shader_ahead_rank(); -1: a file the game was reading; -2: set by the game
} AheadShader;

// Called for each shader not seen before in this scan; returns false to stop the scan.
typedef bool (*AheadFn)(const AheadShader *s, void *ud);

typedef struct {
    int files, archives, shaders, programs, dups, preferred;
    double ms;
} AheadScanStats;

// romfs: the RomFS folder. stop (may be NULL): checked between archive members; the scan ends early when it reads
// non-zero. Returns true when it went through everything.
bool shader_ahead_scan(const char *romfs, AheadFn fn, void *ud, const volatile int *stop, AheadScanStats *st);

// A RomFS file the game has just read (the fs service, any thread): while a scan runs, it reads that file next if it
// hasn't yet (and it's 4 MB at most: a model the game is loading is drawn soon; a whole pack read at load isn't).
// With a '*' it's a game's hint, a pattern ("Model/WU_*plain*"): every file still to be read that matches, any size.
void shader_ahead_prefer(const char *rel);

// The order files are scanned in, lowest first (a game's hook; default 0 for all): what a game shows soonest first.
// A negative rank skips the file.
int shader_ahead_rank(const char *rel);

// Programs as the game sets their shaders (nvnProgramSetShaders, objects.c): the game sets up a model's programs when
// it loads it, before the first draw (seconds before, at a course start; minutes before for agl's post effects, set at
// boot), though it sets up thousands it never draws. Each new one is copied and kept (at most 4096, the oldest
// dropped) while a backend has this on, for the backend's compile-ahead.
void shader_ahead_set_enable(bool on);
void shader_ahead_program_set(const uint8_t *const code[3], const size_t cmax[3]);
// The newest kept (stage 2) if it was set at most max_age_ms ago, or with max_age_ms < 0 the oldest; NULL when there's
// none (after waiting up to wait_ms for one)
const AheadShader *shader_ahead_set_next(int wait_ms, double max_age_ms);
int shader_ahead_set_pending(void);
void shader_ahead_set_done(const AheadShader *s);       // (frees it)

// What the backend's compile-ahead is doing, for the menu (Video > Shaders, GitHub #37): state 0 off / not started, 1
// running, 2 the game's files done (only the programs it sets still go through); items: shaders and programs gone
// through, compiled: sources compiled now (the rest were cached). The backend sets it, the menu reads it.
typedef struct { int state, items, compiled; } AheadStatus;
void shader_ahead_status_set(int state, int items, int compiled);
AheadStatus shader_ahead_status(void);
// The player's "prepare at full speed" (settings: ahead_fast, from the next start): all CPU cores, and no waiting for
// a quiet moment between the game's own builds.
bool shader_ahead_fast(void);
