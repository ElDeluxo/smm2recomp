// A request queue the game drains every frame on its worker thread (WorkerMgr, from the per-frame update at
// main+0x4e0b30, the queues of main+0x4d2d20): each entry {kind, state, object} calls its object's method at vtable
// +0x10 to start or check the request. An object destroyed while its request is still queued leaves its vtable at the
// abstract base (the C++ runtime's __cxa_pure_virtual), and the call stops the game: "Pure virtual function called!"
// (owner's iPad, 2026-10-24: right after a death in 100 Mario, as the course was set up again). The game's own code
// takes a request whose object is null as failed (state 2) on every path, so a destroyed object is cleared to null as
// the queue step starts (main+0x4d2d80, a hook point after its prologue): the request fails as one whose object went
// away, instead of calling into it.
//
// The queue itself has no lock: requests are added on the game's thread (a count increment, then the entry, from
// main+0x4d3720 / 0x4d4110 / 0x4d41a0 / 0x4d32f8 and the kind 3 handler's requeue at 0x4d3fe4) while the worker
// steps it (reads the head entry, pops it: a count decrement). Two threads changing the count at once lose one of the
// changes - a request that is never seen (its object waits for good: SMM2 sits on the course's start after a retry,
// the owner's black screen on the iPad, 2026-10-24) or a finished one that comes back - and a worker that sees the
// new count before the entry is written takes a stale one. So each of those spans runs under one host lock (hook points
// in mods/patches.py: taken where the count is loaded - and the count loaded again under it - released after the
// stores). None of the spans calls anything, so the lock is never taken twice and never waits on the game's own.
#include <pthread.h>
#include <stdint.h>

#include "mod.h"

static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread bool queue_held;
static void lock_queue(uint64_t addr) {
    if (queue_held) return;
    if (pthread_mutex_trylock(&queue_lock)) {  // (the other thread in its span: the race this closes; logged)
        static _Atomic int met;
        int n = ++met;
        if (n <= 10 || !(n & (n - 1)))
            LOG("[mod] worker queue: the game thread and the worker met at main+%#llx (%d times)\n",
                (unsigned long long)(addr - MAIN_ADDR(0)), n);
        pthread_mutex_lock(&queue_lock);
    }
    queue_held = true;
}
static void unlock_queue(void) {
    if (queue_held) queue_held = false, pthread_mutex_unlock(&queue_lock);
}

// (one queue: count +0x78, capacity +0x70, head +0x74, entries of 16 at +0x68; the hook point is after the function's
// prologue, mods/patches.py, before it reads the queue)

static uint64_t pure_virtual;  // __cxa_pure_virtual's guest address (0: not found)

static void worker_guard_check(Cpu *c) {
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

void worker_queue_after(Cpu *c, uint64_t addr) {
    switch (addr - MAIN_ADDR(0)) {
    case 0x4d2d90: lock_queue(addr), worker_guard_check(c); break;           // step: before the count's load
    case 0x4d2db4: if ((uint32_t)c->x[11] != 3) unlock_queue(); break;   // step: the head's kind read (w11)
    case 0x4d2f0c: case 0x4d2f8c: lock_queue(addr), c->x[8] = g32(c->x[19] + 0x78); break;  // step: the pop's count
    case 0x4d3fe4: lock_queue(addr), c->x[8] = g32(c->x[19] + 0x78); break;  // kind 3: requeue
    case 0x4d32f8: lock_queue(addr), c->x[9] = g32(c->x[27] + 0x78); break;  // adds
    case 0x4d3720: case 0x4d41a0: lock_queue(addr), c->x[9] = g32(c->x[0] + 0x78); break;
    case 0x4d4134: lock_queue(addr), c->x[9] = g32(c->x[19] + 0x78); break;
    default: unlock_queue(); break;  // (after the stores; at the exits)
    }
}
