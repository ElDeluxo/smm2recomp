// PC controls in the Course Maker (GitHub #26): a PC's mouse and keyboard on top of the touch screen the mouse already
// is (runtime/hle/touch.c), each mapped onto the Course Maker's own controls:
//
//   right mouse button       erase: L held (the Course Maker's "[L] / [R] Erase") with a finger where the pointer is;
//                            drag to erase along the way
//   middle mouse button      drag to scroll the course (the right stick, which scrolls the view)
//   mouse wheel              scroll left / right (Shift: up / down)
//   Ctrl+Z (Cmd+Z on macOS)  undo (B: the Undodog's button)
//   1 ... 9, 0, -, =         the palette's twelve slots, left to right (a tap on the slot)
//
// Only while the Course Maker is up (the play info's kind word: 0 = edit, research/specs/maker.md "Edit mode words")
// and only from a PC's keyboard and mouse: touch screens and controllers are untouched. In the Course Maker the
// number keys pick palette slots, so 1 and 3 (ZL / ZR in the default keyboard bindings) don't multigrab there.
// Copy and paste have no single button of their own in SMM2 (it copies through a held part's own menu): not mapped.
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include "mod.h"

extern uint64_t g_test_buttons, g_test_buttons_until;  // runtime/game_hooks.c, read by runtime/hle/input.c
extern int32_t g_test_rx, g_test_ry;
extern uint64_t g_test_rstick_until;
extern uint64_t renderer_present_count(void);
extern void touch_test(int32_t x, int32_t y, bool down);  // runtime/hle/touch.c: a finger at game pixel x, y

#define PLAY_INFO MAIN_ADDR(0x2c57d58)  // *(*+0x30): +0x10 kind (0 edit, 1 play), +0x14 edit sub-mode (1 solo, 2 co-op)
#define BTN_B (1ull << 1)
#define BTN_L (1ull << 6)

// SDL scancodes (runtime/hle/input.c passes them through; the game side doesn't include SDL)
enum { SC_1 = 30, SC_0 = 39, SC_MINUS = 45, SC_EQUALS = 46, SC_Z = 29 };

static bool in_editor(void) {
    uint64_t holder = g64(PLAY_INFO), info = holder ? g64(holder + 0x30) : 0;
    if (!info) return false;
    uint32_t kind = g32(info + 0x10), sub = g32(info + 0x14);
    static uint64_t seen = ~0ull;
    if (((uint64_t)kind << 32 | sub) != seen && getenv("RECOMP_PC_EDITOR_LOG")) {
        seen = (uint64_t)kind << 32 | sub;
        LOG("[pc-editor] play info: kind %u, sub-mode %u\n", kind, sub);
    }
    return kind == 0 && (sub == 1 || sub == 2);
}

static void hold_buttons(uint64_t mask, uint64_t frames) {
    g_test_buttons |= mask;
    g_test_buttons_until = renderer_present_count() + frames;
}

static void *tap_thread(void *arg) {  // a finger down, then up a few frames later (the palette needs a real tap)
    uintptr_t xy = (uintptr_t)arg;
    int32_t x = (int32_t)(xy >> 16), y = (int32_t)(xy & 0xffff);
    touch_test(x, y, true);
    usleep(70000);
    touch_test(x, y, false);
    return NULL;
}
static void tap(int32_t x, int32_t y) {
    pthread_t t;
    if (pthread_create(&t, NULL, tap_thread, (void *)(uintptr_t)((uintptr_t)x << 16 | (uint32_t)y)) == 0) pthread_detach(t);
}

bool game_pc_key(int scancode, bool cmd, bool shift, bool down, bool repeat) {
    (void)shift;
    if (!down) return true;  // (a release of a key taken when pressed: runtime/hle/input.c asks only then)
    if (!in_editor() || repeat) return false;
    if (cmd && scancode == SC_Z) {  // undo: the Undodog (B)
        hold_buttons(BTN_B, 4);
        return true;
    }
    if (cmd) return false;
    int slot = scancode >= SC_1 && scancode <= SC_0 ? scancode - SC_1  // (1..9 then 0: slots 0..9)
             : scancode == SC_MINUS ? 10 : scancode == SC_EQUALS ? 11 : -1;
    if (slot < 0) return false;
    // the palette's slots along the top of the screen (1280 x 720: centres every 87.6 px from x 160, y 60)
    tap((int32_t)(160 + 87.6 * slot + 0.5), 60);
    return true;
}

// The right button: L held (the eraser while a finger is down) and the finger at the pointer. The middle button: the
// pointer's movement scrolls the course through the right stick.
static _Atomic bool g_erasing;
static bool g_panning;
static float g_pan_x, g_pan_y;
static _Atomic int32_t g_er_x, g_er_y;
static _Atomic bool g_er_down;  // (the finger is down: L had a few frames to register first)

static void *erase_start(void *arg) {  // L first, then the finger: with both at once the editor saw a long press
    (void)arg;
    usleep(50000);
    if (g_erasing) touch_test(g_er_x, g_er_y, true), g_er_down = true;
    return NULL;
}

bool game_pc_mouse(int button, int act, float x, float y) {
    int32_t gx = (int32_t)(x * 1280), gy = (int32_t)(y * 720);
    if (button == 3) {
        if (act == 0) {
            if (!in_editor()) return false;
            g_erasing = true, g_er_down = false, g_er_x = gx, g_er_y = gy;
            hold_buttons(BTN_L, 1u << 30);  // (until released)
            pthread_t t;
            if (pthread_create(&t, NULL, erase_start, NULL) == 0) pthread_detach(t);
            return true;
        }
        if (!g_erasing) return false;
        g_er_x = gx, g_er_y = gy;
        if (act == 2) {
            if (g_er_down) touch_test(gx, gy, false);
            g_test_buttons &= ~BTN_L, g_erasing = false, g_er_down = false;
            return true;
        }
        if (g_er_down) touch_test(gx, gy, true);  // (the finger follows the pointer: erase along the drag)
        return true;
    }
    if (button == 2) {
        if (act == 0) {
            if (!in_editor()) return false;
            g_panning = true, g_pan_x = x, g_pan_y = y;
            return true;
        }
        if (!g_panning) return false;
        if (act == 2) {
            g_panning = false, g_test_rstick_until = 0;
            return true;
        }
        // drag the course: the view moves against the pointer, faster the further it went since the last move
        float dx = (x - g_pan_x) * 1280, dy = (y - g_pan_y) * 720;
        g_pan_x = x, g_pan_y = y;
        float k = 32767.0f / 24;  // (24 px of movement between samples: full tilt)
        g_test_rx = (int32_t)(-dx * k < -32767 ? -32767 : -dx * k > 32767 ? 32767 : -dx * k);
        g_test_ry = (int32_t)(dy * k < -32767 ? -32767 : dy * k > 32767 ? 32767 : dy * k);
        g_test_rstick_until = renderer_present_count() + 3;
        return true;
    }
    return false;
}

bool game_pc_wheel(float dx, float dy, bool shift) {
    if (!in_editor()) return false;
    // a notch: the right stick for a few frames (vertical wheel: left / right; Shift, or a sideways wheel: up / down)
    float h = shift ? 0 : -dy, v = shift ? dy : 0;
    if (dx) h = dx;
    g_test_rx = h > 0 ? 32767 : h < 0 ? -32767 : 0;
    g_test_ry = v > 0 ? 32767 : v < 0 ? -32767 : 0;
    g_test_rstick_until = renderer_present_count() + 6;
    return true;
}
