// Challenge Modes (owner, 2026-10-19): Course World's Endless Challenge button is relabelled "Challenge Modes";
// behind it the player switches between the 100 Mario Challenge (SMM1's select, map and card) and SMM2's own
// Endless Challenge with ZL / ZR or a tap on the "ZL < mode > ZR" header (bridge.c); L / R stay the difficulty tabs.
// The texts, through the shared RomFS overlay (romfs_overlay.h), in every language's Message/LayoutMsg_<language>.szs:
//   - Nwk_Title_00.msbt, L_EndlessMode_00-T_Btn_00 (the Course World button): "Challenge Modes";
//   - Nwk_EndlessModeMenu_00.msbt, T_Title_00 (the Endless menu's header): "ZL < Endless Challenge > ZR".
// (English everywhere for now; the game's font has no arrow glyphs in its texts, so < and > [inf].)
#include <stdio.h>

#include "mod.h"
#include "romfs_overlay.h"

static const struct { const char *msbt, *label, *text; } TEXTS[] = {
    {"Nwk_Title_00.msbt", "L_EndlessMode_00-T_Btn_00", "Challenge Modes"},
    {"Nwk_EndlessModeMenu_00.msbt", "T_Title_00", "ZL < Endless Challenge > ZR"},
};

static bool challenge_modes_texts(void) {
    static const char *const langs[] = {"USen", "USes", "USfr", "EUen", "EUde", "EUes", "EUfr", "EUit",
                                        "EUnl", "EUru", "JPja", "KRko", "CNzh", "TWzh"};
    if (!mod_hundred_offered()) return true;  // (SMM2's own texts)
    int n = 0;
    for (size_t i = 0; i < sizeof langs / sizeof *langs; i++) {
        char rel[96];
        snprintf(rel, sizeof rel, "Message/LayoutMsg_%s.szs", langs[i]);
        RovSarc *s = rov_sarc_load(rel);
        bool ok = s != NULL;
        for (size_t k = 0; ok && k < sizeof TEXTS / sizeof *TEXTS; k++)
            ok = rov_msbt_set(rov_sarc_member(s, TEXTS[k].msbt), TEXTS[k].label, TEXTS[k].text);
        n += rov_sarc_store(s, ok ? rel : NULL) && ok;
    }
    return n > 0;
}
ROV_GENERATOR("challenge-modes", 1, challenge_modes_texts)
