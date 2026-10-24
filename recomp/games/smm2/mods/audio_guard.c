// The game's sound effects on a channel (a reverb send, nn::audio's Aux) are added from the WorkerMgr worker: a
// command "attach effect to channel" (main+0x4eb760) adds the effect object's buffers to the channel's submix
// (main+0x4ebb40 -> nn::audio::AddAux). On the owner's iPad (v0.5.0, 100 Mario, the 8th attempt at one course after a
// checkpoint, 2026-10-24) one such command came for an effect whose buffers were already gone (its send / return
// buffer pointers, effect +0x58 / +0x60, back to null as its constructor leaves them): AddAux wrote the buffer's header
// through null and the game stopped. Here AddAux refuses an effect without buffers - an error result, which the game
// takes as an effect that couldn't be added (the command's done flag stays clear) - instead of writing through null.
//
// Diagnosis: the effects added / removed so far and the refusals are counted; bridge.c logs them at each retry (a
// leak shows as adds running ahead of removes, retry after retry).
#include <stdatomic.h>

#include "mod.h"

#define ADD_AUX_SUBMIX MAIN_ADDR(0x40814c0)  // (sdk+0x814c0: AddAux(config, aux, SubMixType *, send, return, size))
#define ADD_AUX_FINAL MAIN_ADDR(0x4081350)   // (sdk+0x81350: the FinalMixType one)
#define REMOVE_AUX_SUBMIX MAIN_ADDR(0x40816a0)
#define REMOVE_AUX_FINAL MAIN_ADDR(0x4081630)
#define AUX_REMOVABLE MAIN_ADDR(0x4081720)  // (sdk+0x81720: IsAuxRemovable(aux))
#define RESULT_REFUSED ((2u << 9) | 153u)   // (an nn::audio result: anything but 0 is a failed add)

static _Atomic int g_adds, g_removes, g_refused, g_not_removable;

static void add_aux(Cpu *c, uint64_t fn) {
    if (!c->x[3] || !c->x[4]) {
        int n = ++g_refused;
        if (n <= 20)
            LOG("[mod] audio: an effect without its buffers (send %#llx, return %#llx) not added (%d so far)\n",
                (unsigned long long)c->x[3], (unsigned long long)c->x[4], n);
        c->x[0] = RESULT_REFUSED;
        return;
    }
    guest_call_orig(c, fn);
    if (!(uint32_t)c->x[0]) g_adds++;
}
static void add_aux_submix(Cpu *c) { add_aux(c, ADD_AUX_SUBMIX); }
static void add_aux_final(Cpu *c) { add_aux(c, ADD_AUX_FINAL); }
static void remove_aux_submix(Cpu *c) { guest_call_orig(c, REMOVE_AUX_SUBMIX), g_removes++; }
static void remove_aux_final(Cpu *c) { guest_call_orig(c, REMOVE_AUX_FINAL), g_removes++; }
static void aux_removable(Cpu *c) {
    guest_call_orig(c, AUX_REMOVABLE);
    if (!(c->x[0] & 1)) g_not_removable++;
}

void audio_guard_report(const char *when) {
    LOG("[mod] audio: effects added %d, removed %d, not yet removable %d times, refused %d (%s)\n", g_adds, g_removes,
        g_not_removable, g_refused, when);
}

void audio_guard_register(void) {
    hle_override(ADD_AUX_SUBMIX, add_aux_submix, "mod: nn::audio::AddAux (SubMix), no effect without buffers");
    hle_override(ADD_AUX_FINAL, add_aux_final, "mod: nn::audio::AddAux (FinalMix), no effect without buffers");
    hle_override(REMOVE_AUX_SUBMIX, remove_aux_submix, "mod: nn::audio::RemoveAux (SubMix), counted");
    hle_override(REMOVE_AUX_FINAL, remove_aux_final, "mod: nn::audio::RemoveAux (FinalMix), counted");
    hle_override(AUX_REMOVABLE, aux_removable, "mod: nn::audio::IsAuxRemovable, counted");
}
