// Boot intro skip: skip the Mario logo presentation animation directly to the title screen.
#include <stdint.h>
#include <stdbool.h>

#include "mod.h"
#include "../../../runtime/settings.h"

#define STATE_CHANGE MAIN_ADDR(0x8b9320)

static void boot_cappear(Cpu *c) {
    if (!g_settings.skip_intro) {
        guest_call_orig(c, MAIN_ADDR(0x17E4340));
        return;
    }
    uint64_t seq = c->x[0];
    uint32_t step = g32(seq + 0x240);
    if (step == 0) {
        guest_call_orig(c, MAIN_ADDR(0x17E4340));
        s8(seq + 0x28d, 1);
        c->x[0] = seq + 0x238;
        c->x[1] = 4;
        guest_call(c, STATE_CHANGE);
    } else {
        c->x[0] = seq + 0x238;
        c->x[1] = 4;
        guest_call(c, STATE_CHANGE);
    }
    c->x[0] = 0;
}

static void boot_cinmain(Cpu *c) {
    if (!g_settings.skip_intro) {
        guest_call_orig(c, MAIN_ADDR(0x17E49F0));
        return;
    }
    uint64_t seq = c->x[0];
    uint32_t step = g32(seq + 0x240);
    if (step == 0) {
        guest_call_orig(c, MAIN_ADDR(0x17E49F0));
        s8(seq + 0x28d, 1);
        c->x[0] = seq + 0x238;
        c->x[1] = 4;
        guest_call(c, STATE_CHANGE);
    } else {
        c->x[0] = seq + 0x238;
        c->x[1] = 4;
        guest_call(c, STATE_CHANGE);
    }
    c->x[0] = 0;
}

__attribute__((constructor)) static void skip_intro_register(void) {
    hle_override(MAIN_ADDR(0x17E4340), boot_cappear, "mod: skip boot intro presentation (cAppear)");
    hle_override(MAIN_ADDR(0x17E49F0), boot_cinmain, "mod: skip boot intro presentation (cInMain)");
}

