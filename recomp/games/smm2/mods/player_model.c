// The player's model holder: one override each for its builder and its visual slot setter, shared by the mods that
// put their own model into a spare form slot of the SMB1 row (an address takes one override: hle_override keeps the
// first, runtime/dispatch.c). Each mod says which name-table entries to point elsewhere for the builder's call, and
// which slot to show in place of the one the game asks for:
//   - costumes (costume.c): slot 12 (Balloon, size class 1) holds the course's Mystery Mushroom costume, shown for 0;
//   - Lanky Mario (lanky.c): slot 8 (Builder, size class 2, Super's animations) holds Slim Mario, shown for 1.
//
// Builder 0x710155fcd0 (holder, style, character, heap): once per player per course, it builds the 16 form slots of
// the style's row from the model name table (0x7102c17638, SafeString {vtable, char *} [style * 16 + slot]: the file,
// with the style's prefix and the character's suffix) and the FMDL name table (0x7102c17b38, the same layout).
// Setter 0x710155fcc0 (holder, int *slot): holder +0x478 = *slot, the only writer of the slot the holder draws.
#include "mod.h"

#define BUILDER MAIN_ADDR(0x155fcd0)
#define SETTER MAIN_ADDR(0x155fcc0)

int costume_model_swaps(uint64_t holder, uint32_t style, uint32_t character, PmSwap *out);  // costume.c
int32_t costume_visual_slot(uint64_t holder, int32_t slot);
int lanky_model_swaps(uint64_t holder, uint32_t style, uint32_t character, PmSwap *out);  // lanky.c
int32_t lanky_visual_slot(uint64_t holder, int32_t slot);

static void builder(Cpu *c) {
    uint64_t holder = c->x[0];
    uint32_t style = (uint32_t)c->x[1], character = (uint32_t)c->x[2];
    PmSwap s[8];
    int n = costume_model_swaps(holder, style, character, s);
    n += lanky_model_swaps(holder, style, character, s + n);
    for (int i = 0; i < n; i++) s[i].was = g64(s[i].addr), s64(s[i].addr, s[i].val);
    guest_call_orig(c, BUILDER);
    for (int i = n - 1; i >= 0; i--) s64(s[i].addr, s[i].was);
}

static void setter(Cpu *c) {
    uint64_t holder = c->x[0];
    int32_t slot = (int32_t)g32(c->x[1]);
    slot = costume_visual_slot(holder, slot);
    slot = lanky_visual_slot(holder, slot);
    s32(holder + 0x478, (uint32_t)slot);
}

__attribute__((constructor)) static void player_model_register(void) {
    hle_override(BUILDER, builder, "mods: player model builder (costume, lanky)");
    hle_override(SETTER, setter, "mods: player visual slot (costume, lanky)");
}
