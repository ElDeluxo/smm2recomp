# Frame generation (120 fps output) — design

Date: 2026-10-20. Status: approved in chat, awaiting spec review.

## Goal

A Video-tab setting that doubles the displayed frame rate to 120 fps on 120 Hz displays (ProMotion Macs and
iPads, high-refresh PC monitors and phones) by generating one in-between frame per real frame.

## Constraints

- **Game logic stays at 60 Hz.** SMM2 advances one fixed step per present and is paced by `display_pace`
  (`recomp/runtime/nvn/objects.c`). Unlocking that pacing speeds the game up and breaks the Ryujinx input-replay
  match, so it is not touched. Generated frames are presentation-only; the game never sees them.
- **No engine data is available at present time.** `backend_present` receives only the finished colour image: no
  depth, no motion vectors, no camera matrices. Motion is estimated from consecutive frames.
- **All platforms:** Metal (macOS, iOS) and Vulkan (Windows, Linux, Android), from one shared shader source in the
  style of SMAA (`smaa_hlsl_src` turned into MSL in `metal.m` and GLSL/SPIR-V in `vulkan/exec.c`).
- Off by default. Costs, stated in the setting's description: about 8 ms of added input lag (the real frame is shown
  half a slot later) and possible artifacts on fast motion and on things appearing or disappearing.

## Rejected alternatives

- **Apple's MTLFXFrameInterpolator:** expects engine depth and motion vectors that this runtime doesn't have. It
  would still need our own motion estimate, and only covers Apple platforms.
- **Plain 50/50 frame blending:** ghosts every moving object; reads as blur, not smoothness.
- **True high-fps rendering** (hooking object and camera transforms): far too invasive for the gain.
- **Rates above 120 (3x / 4x, or "match display"):** not wanted now. The pass takes a phase `t` so it can grow later.

## Setting

- `Settings.frame_gen` (enum `FRAMEGEN_OFF`, `FRAMEGEN_120`) in `recomp/runtime/settings.h`. Default off.
- Saved as `frame_gen = off|120` in `<GAME_ID>.ini` (`smm2.ini`), parsed and written in `settings.c` next to
  `upscaler`.
- Video tab (`recomp/runtime/ui/menu.mm`, `tab_display`): `opt_choice` "Frame generation" with the choices
  Off / 120 fps. The description mentions the lag, the artifacts and that it needs a display above 60 Hz.
- Read by the backends every frame, like `g_settings.upscaler`. Switching it at runtime resets history.

## Presentation flow (per real frame N, arriving at time T after `display_pace`)

With frame generation off, behaviour is exactly as today.

With it on, `backend_present`:

1. Runs the existing chain unchanged (SMAA → MetalFX/FSR1 → widescreen crop → present shader), but into an
   off-screen **game image** texture at drawable size and format (`cur`) instead of the drawable. The menu is not
   drawn into it.
2. If there is a valid previous game image (`prev`) and history isn't reset, encodes the interpolation pass
   `interp(prev, cur, t = 0.5)` into the first drawable, draws the menu on top, and presents it immediately.
3. Blits `cur` into a second drawable, draws the menu on top, and presents it one half-slot (1/120 s) later.
4. Swaps `prev` and `cur`.

The game thread must not sleep for the half-slot (that would halve its CPU time per frame):

- **Metal:** the second drawable uses `presentDrawable:afterMinimumDuration:(1.0/120)`. `CAMetalLayer`
  `maximumDrawableCount` goes up to 3 if it was lower (it is 3 by default). On iOS, if the iPad stays at 60 Hz
  with presents at 120, add a `CADisplayLink` with `preferredFrameRateRange` 120 as the frame-rate hint (checked
  on the device).
- **Vulkan:** a present thread owns the delayed present. The game thread records and submits both images' work,
  presents the generated image, and hands the real image (swapchain index + semaphore) to the thread with a
  target time T + 8.33 ms. The thread sleeps until then and calls `vkQueuePresentKHR`. All queue
  submits and presents go through one mutex. The thread joins on swapchain recreation and shutdown. The swapchain
  needs at least 3 images while frame generation is on. On Android, `ANativeWindow_setFrameRate(120)` while on.

Shared by both backends: the menu is drawn onto each presented image, never interpolated. Its ImGui frame is built
once per real frame (`menu_render`, which also takes its input), and the same draw data is drawn again onto the
second image (`menu_redraw`), so menu input and animations stay on the 60 Hz frame.
`backend_represent` (the game held in a host dialog) presents the last real frame only, with no generation.

## Interpolation pass

All fragment/full-screen passes in a shared source (`recomp/runtime/nvn/framegen_src.c`, holding an HLSL-style
source string like `smaa_hlsl_src`, translated per backend as SMAA is). Inputs: `prev`, `cur` at drawable size.

1. **Luma pyramid.** `cur` is reduced to greyscale at 1/4 size and again at 1/8 and 1/16. The previous frame's pyramid is kept,
   so each real frame builds one.
2. **Block motion.** At 1/16 size, 8x8 blocks search ±4 texels (sum of absolute differences). The vector is
   refined at 1/8 and 1/4 with ±2 searches around the upsampled coarse vector. Candidates at every level: the
   zero vector (with a bias, so a static HUD and backgrounds stay put), the coarse vector, and (at the coarsest
   level) the previous frame's final vector at the same block, which carries the camera scroll from frame to frame
   without a CPU readback. Output:
   a motion texture (RG16F, prev → cur, in full-size pixels) and a per-block match cost.
3. **Clean-up.** A 3x3 median filter on the motion texture.
4. **Warp and blend** (at drawable size). For output pixel `p` with motion `m` looked up at `p`:
   `a = prev(p - t*m)`, `b = cur(p + (1-t)*m)`. If `|a - b|` is small, output `mix(a, b, t)`. If not, the pixel is
   being revealed or covered: output whichever of `a` or `cur(p)` / `prev(p)` matches its zero-motion counterpart
   better. This picks a real image over a mix of two different ones.
5. **Whole-frame fallback.** If the mean block match cost is over a threshold (a scene cut, a fade, a menu or
   wipe), the generated frame is `prev` unchanged.

Thresholds and the zero-motion bias are named constants at the top of the source, tuned against real scrolling
and Mario-running captures.

## History resets (no generated frame this time; the real frame is presented normally)

- the first frame after frame generation is turned on
- the drawable size, pixel format or widescreen crop rectangle changes
- the gap since the previous real frame isn't about 16.7 ms (under 12 ms or over 22 ms), which covers hitches,
  the game's own 30 fps mode, and Vulkan's `vk_pace_frame` slow mode
- after `backend_represent`, or after the iOS app returns from the background
- the window's display refreshes below 100 Hz (`SDL_GetCurrentDisplayMode(...)->refresh_rate`): two presents per
  60 Hz refresh would only make `nextDrawable` / acquire block and slow the game, so nothing is generated there

## Interaction with existing systems

- **Dynamic resolution** (`output_scale.c`) keeps its 60 fps target. The interpolation GPU time is in the frame's GPU
  time, so the render scale may drop slightly on weak GPUs. That is expected.
- **The fps overlay and log** count presents, so they read about 120. The log line adds "real" and "generated"
  counts.
- **Screenshot and test hooks** (`LARECOMP_WINDOW_SHOT`, `LARECOMP_CG_SHOT`, hookshot) capture the real frame.
  A new `LARECOMP_FRAMEGEN_SHOT=<frame>` saves `prev`, the generated image and `cur` side by side for review.
- **Replays and tests** compare by game frame and don't depend on wall time, so they are unaffected.

## Testing

- **Unit (shader, headless on Metal; the Vulkan backend runs the same source in CI's Linux build where a device
  exists, otherwise compile-only):**
  - a textured pattern scrolled by (16, 0) px: the generated midpoint matches the pattern shifted by (8, 0) within a
    PSNR threshold
  - a static image: the output equals the input exactly
  - a hard cut between two unrelated images: the output equals `prev`
- **Determinism:** the existing replay and render-diff checks give identical results with `frame_gen=120`.
- **Mac run:** fps log shows about 120 presents a second on a 120 Hz display, with no increase in game hitches.
  `LARECOMP_FRAMEGEN_SHOT` triplets while the camera scrolls and while Mario runs are reviewed.
- **iPad M5:** the build is installed over Tailscale, and 120 Hz output is confirmed from the log and by eye.
- **Vulkan:** the Linux/Windows build presents at about 120 with frame generation on and runs as before with it off.

## Out of scope

Rates above 120 Hz, non-integer refresh ratios (144 Hz shows 120 fps content with the compositor's own pacing),
MetalFX's interpolator, and engine-side motion vectors.
