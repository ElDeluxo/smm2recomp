// The player's own mods (LayeredFS, as Atmosphère and the emulators do it): each in the data folder's mods/<id>/ with
// its files in romfs/ (replacing or joining the game's), and mod.info (name, author, GameBanana page...). Which ones
// are on, and their order (a later one's file wins), is mods/mods.ini. The game's RomFS is put together from them at
// its start (the game's overlay list: runtime/services/fs.c), so a change applies at the next start.
// Nothing of a mod is ever part of the program: players install them themselves (by hand, or from GameBanana in the
// Mods tab: runtime/gamebanana.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USERMODS_DIR "mods"

typedef struct {
    char id[96];             // its folder name
    char name[128], author[128], category[64], license[64], version[32];
    char url[256];           // its page (GameBanana), or ""
    char description[1024];
    int gb_id;               // GameBanana's mod id (0: installed by hand)
    bool enabled;
    bool has_romfs;          // game files (romfs/): what is loaded
    bool has_exefs;          // code patches (exefs/, .ips): not loaded (yet); shown so
    int nfiles;              // in romfs/
    uint64_t bytes;
} UserMod;

void usermods_scan(void);                 // read mods/ again (the Mods tab after an install, removal or rename)
int usermods_count(void);
bool usermods_get(int i, UserMod *out);   // in priority order (the last one wins)
// On / off. Turning one on turns off the enabled mods that replace any of the same files (a game style skin over
// another one): their names go to turned_off (", "-separated; may be NULL).
void usermods_set_enabled(int i, bool on, char *turned_off, size_t cap);
void usermods_move(int i, int delta);     // in the order (later = wins)
bool usermods_remove(int i);              // deletes its folder
int usermods_conflicts(int i, char *names, size_t cap);  // enabled mods that share a file with it (count; names)
bool usermods_changed_since_start(void);  // the set differs from the one the running game was started with

// The game's side (at its RomFS's opening): the enabled mods' romfs folders, lowest priority first, one per line
// (each ending in '\n'); the length written.
size_t usermods_overlay(char *buf, size_t cap);
bool usermods_find(const char *rel, char *out, size_t cap);  // an enabled mod's file (the winning one)
void usermods_identity(char *buf, size_t cap);              // what's enabled, for caches built from the mods' files

// Install a mod from a folder (an unpacked download): its game files are found wherever they are in it (a romfs
// folder, Atmosphère's contents/<title>/romfs, or the game's own top folders), moved to mods/<id>/romfs, and info
// written. Several romfs folders in one download are installed as one mod each ("<name> (<folder>)"). Returns how
// many were installed; why says why not, when none.
typedef struct {
    char name[128], author[128], category[64], license[64], version[32], url[256], description[1024];
    int gb_id;
} UserModInfo;
int usermods_install_dir(const char *src, const UserModInfo *info, bool enable, char *why, size_t why_cap);
