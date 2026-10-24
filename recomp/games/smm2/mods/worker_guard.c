// A request queue the game drains every frame on its worker thread (WorkerMgr, from the per-frame update at
// main+0x4e0b30, the queues of main+0x4d2d20): each entry {kind, state, object} calls its object's method at vtable
// +0x10 to start or check the request. An object destroyed while its request is still queued leaves its vtable at the
// abstract base (the C++ runtime's __cxa_pure_virtual), and the call stops the game: "Pure virtual function called!"
// (owner's iPad, 2026-10-24: right after a death in 100 Mario, as the course was set up again). The game's own code
// takes a request whose object is null as failed (state 2) on every path, so a destroyed object is cleared to null as
// the queue step starts (main+0x4d2d80, a hook point after its prologue): the request fails as one whose object went
// away, instead of calling into it.
#include <stdint.h>

#include "mod.h"

// (one queue: count +0x78, capacity +0x70, head +0x74, entries of 16 at +0x68; the hook point is after the function's
// prologue, mods/patches.py, before it reads the queue)

static uint64_t pure_virtual;  // __cxa_pure_virtual's guest address (0: not found)

void worker_guard_check(Cpu *c) {
    static bool looked;
    if (!looked) {  // (the game's modules are loaded by the time it runs)
        looked = true;
        pure_virtual = loader_find_symbol("__cxa_pure_virtual");
        LOG("[mod] worker guard: __cxa_pure_virtual at %#llx\n", (unsigned long long)pure_virtual);
    }
    uint64_t q = c->x[0];
    uint32_t n = g32(q + 0x78);
    if (n && pure_virtual) {
        int32_t cap = (int32_t)g32(q + 0x70), head = (int32_t)g32(q + 0x74);
        int32_t wrap = cap <= head ? cap : 0;
        uint64_t e = g64(q + 0x68) + (uint64_t)(int64_t)(head - wrap) * 0x10, obj = g64(e + 8);
        if (obj && g64(obj) && g64(g64(obj) + 0x10) == pure_virtual) {
            static int nlog;
            if (nlog++ < 20)
                LOG("[mod] worker queue %#llx: request (kind %u, state %u) for a destroyed object %#llx: failed, not called\n",
                    (unsigned long long)q, g32(e), g32(e + 4), (unsigned long long)obj);
            s64(e + 8, 0);
        }
    }
}
