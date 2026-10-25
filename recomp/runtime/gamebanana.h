// GameBanana in the Mods tab (gamebanana.c): the game's mods there, most downloaded first (or a search), each one's
// details and files, and installing one: downloaded onto the player's machine, unpacked (runtime/archive.h) and put
// in mods/ (runtime/usermods.h) with its author, licence and page. Uses GameBanana's public API
// (https://gamebanana.com/apiv11); all on a background thread, so the menu never waits. A game without a GameBanana
// id (game_gamebanana_id(): 0) has none of it.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define GB_MAX_FILES 8

typedef struct {
    int id;
    char name[128], author[96], category[64];
    // details (gb_details): loaded once asked for
    bool details;
    int downloads, likes;
    char license[96], version[32], text[1200];
    int nfiles;
    int file_id[GB_MAX_FILES];
    char file_name[GB_MAX_FILES][128], file_desc[GB_MAX_FILES][128];
    uint64_t file_size[GB_MAX_FILES];
} GbMod;

typedef enum { GB_IDLE, GB_BUSY, GB_ERROR } GbState;

int game_gamebanana_id(void);       // the game's id there (runtime/game_hooks.c: 0, none)
bool gb_available(void);
void gb_refresh(const char *search);  // the list again: the most downloaded (search NULL / ""), or a search
GbState gb_list_state(char *msg, int cap);
int gb_list(GbMod *out, int max);   // what the last refresh found
void gb_details(int id);            // fetch one's details (then gb_get shows them)
bool gb_get(int id, GbMod *out);
// Install file fi of mod id (on when enable). One at a time; gb_install_state says how it goes.
void gb_install(int id, int fi, bool enable);
GbState gb_install_state(int *id, float *progress, char *msg, int cap);  // progress 0..1 (-1: unknown)
void gb_install_cancel(void);
