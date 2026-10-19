// Frame generation (Video tab "Frame generation"): one in-between frame per real 60 Hz frame for 120 Hz displays.
// The game still runs and is paced at 60 (objects.c display_pace); the generated frames exist only on the screen.
// framegen.c decides per real frame whether one is made; framegen_src.c holds the passes (MSL / GLSL);
// framegen_metal.m and vulkan/exec.c encode them; metal.m / vulkan/device.c present the two images.
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct SDL_Window;

typedef struct {
    uint32_t w, h, fmt;   // the game image's (= drawable's) size and pixel format
    float crop[4];        // widescreen_present_crop's uv rectangle
    uint64_t now_ns;      // plat_now_ns() at this real frame's present
    float display_hz;     // the window's display refresh (SDL), 0 = unknown
    bool enabled;         // g_settings.frame_gen != FRAMEGEN_OFF
    bool held;            // a re-present while the game is held (backend_represent / iOS background)
} FgFrame;
typedef struct { bool valid; uint32_t w, h, fmt; float crop[4]; uint64_t last_ns; } FgState;

// Per real frame: true = generate an in-between frame from the stored previous game image and this one.
// Afterwards this frame is the stored one (unless !enabled or held: then history is dropped).
bool framegen_step(FgState *s, const FgFrame *f);
// The window's display refresh in Hz (0 if unknown). LARECOMP_FORCE_DISPLAY_HZ=<hz> overrides it (tests).
float framegen_display_hz(struct SDL_Window *w);
extern uint64_t g_fg_real, g_fg_generated;  // counters for the fps log
#define FG_HALF_SLOT_NS 8333333ull           // the real frame is shown this long after the generated one
