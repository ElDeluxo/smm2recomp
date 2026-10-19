# Frame Generation (120 fps output) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Video-tab setting "Frame generation: Off / 120 fps". It presents one motion-estimated in-between frame per real
60 Hz frame on 120 Hz displays. The game logic and its pacing stay as they are.

**Architecture:** A pure-C decision module (`framegen.c`) says, for each real frame, whether to generate one. One
shader source (`framegen_src.c`, with MSL and GLSL heads, the way SMAA is built) has seven full-screen passes: luma
pyramid, block motion search, median, cost reduction, warp. Each backend renders its existing present chain into an
off-screen game image, presents `interp(prev, cur, 0.5)` with the menu at once, and presents `cur` with the menu
1/120 s later. Metal uses `presentDrawable:afterMinimumDuration:`; Vulkan uses a present thread.

**Tech Stack:** C11 / Objective-C (Metal, MetalFX untouched), Vulkan 1.3 with shaderc (GLSL → SPIR-V at run time), SDL3, Dear ImGui.

**Spec:** `docs/superpowers/specs/2026-10-20-frame-generation-design.md`

## Global Constraints

- The game logic stays at 60 Hz. `display_pace` (`recomp/runtime/nvn/objects.c`) is not changed. Generated frames never reach the game.
- With `frame_gen = off`, every backend's present path behaves exactly as before. Same passes, same presents.
- Setting: `Settings.frame_gen`, `FRAMEGEN_OFF` (default) / `FRAMEGEN_120`; ini key `frame_gen = off|120` in `<GAME_ID>.ini`.
- Phase `t = 0.5`. Delayed present: 1/120 s (8 333 333 ns) after the generated one.
- Generate only when the gap between real frames is 12–22 ms, the size, format and crop are unchanged, the game is
  not held (`backend_represent` or the iOS background), and the display refresh is ≥ 100 Hz.
- Our menu (ImGui) is never interpolated. Its frame is built once per real frame (`menu_render`) and drawn again
  (`menu_redraw`).
- Deployment targets stay as they are (macOS 15.0 app, iOS 17.0). Nothing new needs a newer OS.
- Every new comment and log line follows the surrounding style: `// (...)` notes, `LOG("[framegen] ...")`.
- Agents running the game: **at most one smm2recomp instance at a time**, short targeted runs, nothing left running.
- Commits are local. Push only in a batch at the end (CI minutes). `scripts/check.sh` runs as the pre-commit gate.

## Review Focus

1. **Turning the setting on or off mid-level.** No stale previous frame flashes on screen. The first frame after
   turning it on is real only. → Task 1 `test_enable_resets`.
2. **A 60 Hz display with the setting on.** The game fps must not drop, because nothing is generated there. → Task 1
   `test_low_refresh_skips`, plus a check in Task 3 step 6.
3. **Resizing the window, toggling fullscreen or rotating the iPad.** No garbage or black frames; history resets.
   → Task 1 `test_size_change_resets`, plus a check in Task 3 step 6.
4. **The menu open with the setting on.** The menu stays sharp, and one key press is handled once (ImGui built once
   per real frame). → Task 3 step 6.
5. **Vulkan swapchain recreation or shutdown while the real frame's present is still waiting.** No present to a
   destroyed swapchain, and the frame order is never inverted. → Task 5 step 6.

---

### Task 1: Setting and per-frame decision module

**Files:**
- Modify: `recomp/runtime/settings.h` (enum next to `UPSCALE_*`, field after `antialias`), `recomp/runtime/settings.c` (default, parse, save next to `upscaler`)
- Create: `recomp/runtime/nvn/framegen.h`, `recomp/runtime/nvn/framegen.c`
- Create: `recomp/tools/test_framegen_logic.c`
- Modify: `recomp/Makefile` (targets `build/test_framegen_logic`, `test-framegen`; `.PHONY`), `scripts/check.sh` (run `make -s -C recomp test-framegen` on Darwin, next to `runtime-objs`)

**Interfaces:**
- Produces:
  - `enum { FRAMEGEN_OFF, FRAMEGEN_120 };` and `int frame_gen;` in `Settings`
  - in `framegen.h`:
    ```c
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
    float framegen_display_hz(SDL_Window *w);   // SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(w))->refresh_rate, 0 if none
    extern uint64_t g_fg_real, g_fg_generated;  // counters for the fps log
    #define FG_HALF_SLOT_NS 8333333ull
    ```
  - `make test-framegen` (from `recomp/`): builds and runs the tests from this task and Task 2.

- [ ] **Step 1: Write `recomp/tools/test_framegen_logic.c`.** A plain `main` with `CHECK(cond)` (it prints the failing
  line and exits 1). Base frame: 1920×1080, fmt 80, crop {0,0,1,1}, display_hz 120, enabled. Frames are 16.67 ms apart.
  - `test_first_frame_real`: the first `framegen_step` → false. The second, 16.67 ms later → true.
  - `test_enable_resets`: on, on (→ true), then `enabled=false` (→ false), then on again (→ false), then on (→ true).
  - `test_size_change_resets`: a change to w, then fmt, then crop[2] → false on the frame that changes, true on the next.
  - `test_gap_bounds`: gaps of 11.9 ms and 22.1 ms → false. 12.0 ms and 22.0 ms → true. After a 40 ms hitch the next 16.67 ms frame → true.
  - `test_held_resets`: `held=true` → false. The next frame → false. The one after → true.
  - `test_low_refresh_skips`: display_hz 60 → false on every frame. display_hz 0 (unknown) → false. 119.88 → true.
- [ ] **Step 2: Add the Makefile rule.**
  - `build/test_framegen_logic: tools/test_framegen_logic.c runtime/nvn/framegen.c runtime/nvn/framegen.h` builds with `clang -O1 -I runtime`.
  - `framegen_display_hz` lives in `framegen.c` behind `#ifndef FG_NO_SDL`, and the test passes `-DFG_NO_SDL`.
  - `test-framegen` runs the binary.
  - Run `make -C recomp test-framegen`. Expected: link failure (no `framegen_step`).
- [ ] **Step 3: Implement `framegen_step` in `recomp/runtime/nvn/framegen.c`.** The rules are the Global Constraints
  line about when to generate, with bounds inclusive at 12 000 000 and 22 000 000 ns and refresh ≥ 100. The function
  increments `g_fg_real` every call and `g_fg_generated` when it returns true.
- [ ] **Step 4: Add the setting.**
  - `settings.c`: default `.frame_gen = FRAMEGEN_OFF`. Parse `frame_gen`: `"120"` → `FRAMEGEN_120`, anything else → off.
  - Save it as `frame_gen = %s` (`120` / `off`) after the `upscaler` line.
- [ ] **Step 5: Run the gates.** Run `make -C recomp test-framegen && make -s -C recomp GAME=smm2 -j8 runtime-objs`.
  Expected: every test reports PASS, and the objects build.
- [ ] **Step 6: Commit.** `Frame generation: setting and per-frame decision (framegen.c) with tests`

### Task 2: Shared interpolation shaders and the Metal pass runner

**Files:**
- Create: `recomp/runtime/nvn/framegen_src.c` (shader source + per-backend assembly)
- Create: `recomp/runtime/nvn/framegen_metal.m` (Metal resources and encoding of the passes)
- Create: `recomp/tools/test_framegen.m`
- Modify: `recomp/Makefile` (`build/test_framegen`, add it to `test-framegen`)

**Interfaces:**
- Produces:
  - in `framegen.h`:
    ```c
    char *framegen_msl(void);                      // malloc'd MSL library: all passes as fragment functions + fg_vs
    const char *framegen_glsl(const char *pass);   // GLSL 450 fragment source for "fg_luma.frag" ... "fg_warp.frag", NULL if unknown (cached)
    ```
  - in `framegen_metal.m` (declared in `framegen.h` under `#ifdef __OBJC__`):
    ```objc
    // The in-between image of prev and cur (same size and format) at phase t, drawn into out (any colour format, same size).
    // reset: drop the kept luma pyramid and motion (the next call builds prev's pyramid from prev). false on a shader /
    // pipeline error (logged once; frame generation then stays off).
    bool fgm_encode(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> prev, id<MTLTexture> cur, float t, bool reset,
                    id<MTLTexture> out);
    ```
- **The passes.** Every pass is a full-screen triangle with two `float4` parameters `a`, `b` (Metal: fragment bytes at
  buffer 0; Vulkan: push constants) and up to four inputs `t0..t3` (bilinear, clamp). All passes sample texel centres.
  Sizes are rounded up.

  | pass | inputs | output | params |
  |---|---|---|---|
  | `fg_luma` | t0 colour (W×H) | L4: RGBA8, R = Rec.601 luma averaged over each 4×4 | a.xy = W,H |
  | `fg_down` | t0 Lk | L(2k): RGBA8, 2×2 average | a.xy = input size |
  | `fg_motion` | t0 cur level, t1 prev level, t2 coarser motion, t3 last frame's final motion | RGBA16F, one texel per 4×4-texel block: rg = vector prev→cur in full-res px, b = mean abs luma diff per texel | a = (L, radius, has_t2, has_t3), b = (level W,H, full W,H) |
  | `fg_median` | t0 motion | same size, per-component 3×3 median of rg, b kept | – |
  | `fg_cost_rows` | t0 motion (w×h) | 1×h RGBA16F, R = mean of b over the row | a.x = w |
  | `fg_cost_total` | t0 rows (1×h) | 1×1 RGBA16F, R = mean | a.x = h |
  | `fg_warp` | t0 prev, t1 cur, t2 final motion (bilinear), t3 total cost | the output image | a = (t, W, H, 0) |

  The motion search runs at levels L = 16 (radius 4, candidates: zero, and t3 if present), then L = 8 and L = 4
  (radius 2 around each of zero and the upsampled t2 vector). The cost of candidate `m` is the mean of
  `|lcur(x) − lprev(x − m/L)|` over the block. The zero vector's cost is reduced by `FG_ZERO_BIAS`, and ties go to
  zero.

  The warp, per output pixel `p`:
  ```
  m = t2(p);  a = prev(p - t*m);  b = cur(p + (1-t)*m);  z0 = prev(p);  z1 = cur(p)
  if (t3.r > FG_CUT_COST) return z0                               // whole-frame fallback: scene cut / fade / wipe
  dm = maxabs(a - b);  dz = maxabs(z0 - z1)
  if (dz <= dm && dz < FG_MATCH) return mix(z0, z1, t)            // static here
  if (dm < FG_MATCH) return mix(a, b, t)                          // moved
  return t < 0.5 ? a : b                                          // revealed / covered: one real image, not a mix
  ```
  `maxabs` is the max over rgb, and alpha is 1. The constants at the top of the source are `FG_ZERO_BIAS 0.02`,
  `FG_MATCH 0.08` and `FG_CUT_COST 0.10`.
- **Shared source dialect.** One `framegen_src[]` uses `float2/float3/float4/int2`, `FG_TEX` (the parameter type),
  `FG_LOAD(t, int2)`, `FG_SAMPLE(t, float2 uv)`, `FG_SIZE(t)` (int2), `mix`, `clamp`, `abs`, `min`, `max`, `dot`,
  local arrays, and no `select` or swizzle assignment.
  - The MSL head maps these to `texture2d<float>`, `.read`, `.sample` with a constexpr linear clamp sampler, and
    `get_width/height`.
  - The GLSL head `#define`s the float types to `vec*`/`ivec2`, and the texture macros to `sampler2D`, `texelFetch`
    clamped to `textureSize - 1`, and `textureLod(…, 0.0)`.
  - Per-backend entry wrappers come after the shared source, as `smaa_msl` and `shaders.c`'s SMAA `mains` do.

- [ ] **Step 1: Write `recomp/tools/test_framegen.m`.**
  - It uses `MTLCreateSystemDefaultDevice`. Textures are 512×256 BGRA8, shared storage, with render target, shader
    read and shader write usage. Each image is made with `fgm_encode(…, t = 0.5, reset = true, out)` in one command
    buffer, then `waitUntilCompleted` and a readback.
  - `pattern(x, y)` is a deterministic value noise of 8×8-px cells (hash of the cell), with bilinear smoothing inside
    each cell. It is in 0..255 per channel, and each channel uses a different seed.
  - `test_scroll`: prev = pattern, cur = pattern shifted by (+16, 0). The output vs the pattern shifted by (+8, 0) has
    a PSNR ≥ 30 dB over the interior (32 px border excluded).
  - `test_static`: prev = cur = pattern → out == cur byte for byte.
  - `test_cut`: prev = pattern (seed 1), cur = pattern (seed 2) → out == prev byte for byte.
  - `test_hud`: both images get the same solid 96×32 box at (40, 40) with a 4-px checker inside it, over the
    scrolling pattern → max abs diff ≤ 2 inside the box.
  - The test prints `PASS name` / `FAIL name (value)` and exits non-zero on any failure.
- [ ] **Step 2: Add the Makefile rule.**
  - `build/test_framegen` from `tools/test_framegen.m runtime/nvn/framegen_metal.m runtime/nvn/framegen_src.c`, with
    `-fobjc-arc -framework Metal -framework Foundation -I runtime -DFG_NO_SDL`.
  - Plus a one-line `rt_log` stub in the test file (`LOG` is `rt_log`).
  - Run `make -C recomp test-framegen`. Expected: a build failure (no `fgm_encode`).
- [ ] **Step 3: Implement `framegen_src.c`.** It holds the shared source, the MSL head and wrappers (`framegen_msl`),
  and the GLSL head and per-pass `main`s (`framegen_glsl`, `layout(push_constant) uniform P { vec4 a; vec4 b; } pc;`,
  bindings 0..3).
- [ ] **Step 4: Implement `fgm_encode` in `framegen_metal.m`.**
  - The library and pipelines are compiled once; warp pipelines are cached per output pixel format.
  - Private textures are recreated on a size change.
  - Two luma pyramids (L4, L8, L16) and the final motion of the last call swap each call.
  - A 1×1 dummy texture is used when t2 or t3 is absent.
- [ ] **Step 5: Run the tests.** Run `make -C recomp test-framegen`. Expected: PASS for all four Metal tests and all
  logic tests. If `test_scroll` misses 30 dB, tune only the three constants, and record the final values in the
  source's comment.
- [ ] **Step 6: Commit.** `Frame generation: shared interpolation shaders + Metal pass runner, with tests`

### Task 3: Metal presentation (macOS) and the Video-tab setting

**Files:**
- Modify: `recomp/runtime/nvn/metal.m`:
  - `backend_present` (around lines 1141–1374)
  - `backend_represent` (around 1085)
  - `ui_window_init` (around 1407: `maximumDrawableCount = 3`)
  - the fps log (around 1360)
- Modify: `recomp/runtime/ui/menu.mm` (`menu_redraw`; the "Frame generation" row in `tab_display` after "Upscaler"), `recomp/runtime/ui/menu.h`

**Interfaces:**
- Consumes: `framegen_step`, `FgFrame`, `framegen_display_hz`, `fgm_encode`, `g_fg_real`/`g_fg_generated`, `FG_HALF_SLOT_NS` (Tasks 1–2).
- Produces:
  - `void menu_redraw(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> target);` (Apple) and
    `void menu_redraw(void *cmd, uint32_t target_w, uint32_t target_h);` (Vulkan).
  - Each draws the draw data of the last `menu_render` again, with no `NewFrame` and no input drain. It does nothing
    when the last `menu_render` returned early.
  - `LARECOMP_FRAMEGEN_SHOT=<frame>` (+ `LARECOMP_SHOT_PATH`, default `build/framegen.png`) saves prev | generated |
    cur side by side as one PNG, at that real frame.

- [ ] **Step 1: Add `menu_redraw` in `menu.mm`.** Keep a flag for whether the last `menu_render` drew. On Metal, open
  a load/store pass on the target and call `ImGui_ImplMetal_RenderDrawData` with the same `ImGui::GetDrawData()`.
  On Vulkan, call `ImGui_ImplVulkan_RenderDrawData`.
- [ ] **Step 2: Split `backend_present` when frame generation is on.**
  - Build `FgFrame` from the drawable size and format, the crop after `widescreen_present_crop`, `plat_now_ns()`,
    `framegen_display_hz(window)` and `held` (true when called from `backend_represent`; also set after
    `wait_while_in_background` actually waited).
  - With `g_settings.frame_gen` on, the existing clear, SMAA, MetalFX and present draw target an off-screen
    `fg_cur` (drawable size and format, private, render target + shader read) instead of `drawable.texture`.
  - Then, in the same command buffer:
    1. If `framegen_step` returned true: `fgm_encode(dev, cb, fg_prev, fg_cur, 0.5f, reset, d1.texture)`, then
       `menu_render` into `d1`, then `[cb presentDrawable:d1]`. `reset` is true on the first generated frame after
       any false return.
    2. Get `d2` (`nextDrawable`), blit `fg_cur` into it, and draw the menu with `menu_redraw` if one was already
       built this frame, otherwise `menu_render`. Then `[cb presentDrawable:d2 afterMinimumDuration:1.0/120]` when
       a generated frame was presented, else a plain `presentDrawable:`.
    3. Swap `fg_prev` and `fg_cur`.
  - With the setting off, the code path is the existing one, unchanged.
  - The screenshot hooks (`LARECOMP_WINDOW_SHOT`, `LARECOMP_CG_SHOT`, hookshot) read `d2`, the real frame.
- [ ] **Step 3: Add the `LARECOMP_FRAMEGEN_SHOT` hook and log counts.**
  - The hook blits `fg_prev`, `d1` and `fg_cur` into a 3W×H shared texture and saves it as PNG with the existing
    texture-save helper.
  - The fps log line adds ` gen %llu/%llu` (generated / real since the last line).
  - On the first generated frame, log `[framegen] on WxH display %.0f Hz`.
- [ ] **Step 4: Add the Video-tab row.**
  - `opt_choice("Frame generation", &g_settings.frame_gen, {"Off", "120 fps"}, FRAMEGEN_OFF, desc)` after "Upscaler",
    following that row's `opt_choice` call shape.
  - `desc`: "Adds a generated frame between each pair of real frames for 120 fps on 120 Hz displays (ProMotion,
    high-refresh monitors). The game itself still runs at 60. Adds a little input lag; fast motion may show small
    artifacts. Does nothing on a 60 Hz display."
- [ ] **Step 5: Build and run the gates.** Run `make -C recomp test-framegen && make -s -C recomp GAME=smm2 -j8 all`.
  Expected: the tests pass and the app builds.
- [ ] **Step 6: Run checks on the Mac** (one instance; `frame_gen = 120` in `smm2.ini`; a level with scrolling:
  `make -C recomp GAME=smm2 run` with the usual test-course env).
  1. On a 120 Hz display (ProMotion or external), the `[metal]` fps log shows about 120 fps and `gen` ≈ real, with
     game frames still about 60/s (`frame` advancing about 60 per second).
  2. `LARECOMP_FRAMEGEN_SHOT` during a scroll and during a run: the generated panel shows the scroll and Mario
     halfway, with no smearing around Mario or the HUD.
  3. Turn the setting off and on in the menu mid-level: no flash of an old frame.
  4. Resize the window and toggle fullscreen: no garbage frames, and generation resumes.
  5. Open the menu: it is sharp, and one key press moves the selection by one.
  6. Run with `LARECOMP_FORCE_DISPLAY_HZ=60` (a debug override read by `framegen_display_hz`; add it):
     `gen 0/…`, fps the same as with the setting off.
  7. Run the existing replay test (`tools/replay_sync.py`, as `recomp/docs/smm2-bringup.md` describes) with the
     setting on and off: identical results.
- [ ] **Step 7: Commit.** `Frame generation on Metal: generated + delayed real present, menu drawn on both, Video-tab setting`

### Task 4: iPad (iOS) at 120 Hz

**Files:**
- Modify (only if step 2 shows 60 Hz): `recomp/runtime/nvn/metal.m` (`ui_window_init`, iOS block)

- [ ] **Step 1: Build and install.**
  - Build the iOS app as in memory `ipad-device-testing`: `make -C recomp TARGET=ios-arm64 GAME=smm2 app` with the
    `RECOMP_IOS_*` vars, after `embed-server`.
  - Install with `ios-install-tailscale … --device 00008142-001A698236C2401C --host ipad173`.
  - Push `smm2.ini` with `frame_gen = 120` using `ios-app push`.
- [ ] **Step 2: Launch it, enter a course, and read `/Documents/smm2recomp.log` (`ios-app pull`).** Expected: about
  120 fps with `gen` ≈ real. If it holds at 60 with `gen` > 0 (presents throttled), go to step 3; otherwise skip it.
- [ ] **Step 3: Add the frame-rate hint (iOS only, while frame generation is on).**
  - Add a `CADisplayLink` on the main run loop with an empty target method and
    `preferredFrameRateRange = CAFrameRateRangeMake(80, 120, 120)`.
  - Pause it when the setting is off. Rebuild and repeat step 2. Expected: about 120 fps.
- [ ] **Step 4: Take a screenshot and a framegen shot during a scroll, and check the MetalFX upscale path.** The iPad
  upscales to 2752×2064, and the generated frame comes after it. Expected: steady 120 with no drop in game frames.
- [ ] **Step 5: Commit** (if code changed). `Frame generation on iOS: frame-rate hint for ProMotion`

### Task 5: Vulkan (Windows, Linux, Android)

**Files:**
- Modify: `recomp/runtime/nvn/vulkan/shaders.c` (`fg_*.frag` names → `framegen_glsl`, next to the `smaa_` branch)
- Modify: `recomp/runtime/nvn/vulkan/exec.c`:
  - `post_pipeline(fs)` → `post_pipeline(fs, VkFormat fmt)`, cached by (fs, fmt)
  - `post_image(i, w, h)` → `post_image(i, w, h, VkFormat fmt)` with more slots
  - post layout bindings 3 → 4
  - new `vk_framegen_encode`
- Modify: `recomp/runtime/nvn/vulkan/device.c`:
  - `backend_present`, `backend_represent`
  - `vk_frame_end_at`
  - the present thread
  - a queue mutex around `vkQueueSubmit2` (exec.c:388), `vkQueuePresentKHR` and the `vkDeviceWaitIdle` calls
  - `recreate_swapchain` and shutdown flush the thread
  - Android `ANativeWindow_setFrameRate`
- Create: `recomp/tools/test_framegen_glsl.c` (compiles every `fg_*.frag` with shaderc; run in a container)

**Interfaces:**
- Consumes: `framegen_step`, `FgFrame`, `framegen_glsl`, `menu_redraw(void*, w, h)`, `FG_HALF_SLOT_NS`.
- Produces (internal to the Vulkan backend):
  - `void vk_framegen_encode(VkCommandBuffer cb, TexBackend *prev, TexBackend *cur, float t, bool reset);` draws
    into the current window image (between `vk_frame_begin` and `vk_frame_end`).
  - `void vk_frame_end_at(uint64_t present_at_ns);` is `vk_frame_end(NULL)`, except the present runs on the present
    thread at `present_at_ns`.
  - `void vk_present_flush(void);` waits until the deferred present has been issued.
  - `void vk_queue_lock(void); void vk_queue_unlock(void);`

- [ ] **Step 1: Write `recomp/tools/test_framegen_glsl.c`.** It runs `shaderc_compile_into_spv` on
  `framegen_glsl(name)` for all seven names and prints `PASS fg_x.frag` or the compiler's error, exiting non-zero on
  any error.
  - Build and run it in `debian:trixie` (arm64) with `libshaderc-dev`, mounting the repo read-only and the build dir
    writable: `cc -I recomp/runtime tools/test_framegen_glsl.c runtime/nvn/framegen_src.c -lshaderc`.
  - It must fail before `framegen_glsl` exists, then pass after Task 2's source. Fix GLSL-only errors in the GLSL
    head and wrappers, not in the shared source's logic.
- [ ] **Step 2: Hook up the shaders and post passes.**
  - In `shaders.c`, names starting with `fg_` return `framegen_glsl(name)` with `STAGE_FRAGMENT`.
  - Extend `post_pipeline`, `post_image` and the post layout as listed. Existing callers pass `VK_FORMAT_R8G8B8A8_UNORM`.
  - Implement `vk_framegen_encode` with the same pass order, sizes, formats and parameters as `fgm_encode`. The luma
    is `R8G8B8A8_UNORM`; motion and costs are `R16G16B16A16_SFLOAT`. `fg_warp` uses `post_pipeline("fg_warp.frag", g_vk.sc_format)`.
- [ ] **Step 3: Add the present thread and the queue lock in `device.c`.**
  - One pending slot `{img, done_sem, present_at_ns}` with a mutex and condvar.
  - The thread sleeps until `present_at_ns` (`plat_sleep_until_ns`), presents under the queue lock, and records an
    `OUT_OF_DATE`/`SUBOPTIMAL`/`SURFACE_LOST` result in a flag.
  - `vk_frame_begin` checks the flag and does the recreation on the game thread.
  - `vk_frame_end` (the immediate present) calls `vk_present_flush()` first, so the generated frame N+1 can never
    overtake the real frame N.
  - `recreate_swapchain` and shutdown call `vk_present_flush()` and then join or stop the thread.
  - `create_swapchain` requests at least 3 images (`max(minImageCount + 1, 3)`, capped by `maxImageCount`), so
    two presents per real frame never block the acquire.
- [ ] **Step 4: Split `backend_present`, mirroring Task 3 step 2.**
  - The present chain (`vk_present_draw`) renders into an off-screen `fg_cur` (`post_image` slot, swapchain extent,
    `R8G8B8A8_UNORM`, its own rendering begin/end).
  - Then:
    1. If generating: `vk_frame_begin`, `vk_framegen_encode(prev, cur, 0.5)`, `menu_render`, `vk_frame_end`.
    2. Then `vk_frame_begin`, a full-window present draw of `fg_cur` (crop {0,0,1,1}), `menu_redraw`, and
       `vk_frame_end_at(t_generated + FG_HALF_SLOT_NS)` when a frame was generated, else `vk_frame_end`.
  - `vk_pace_frame`'s busy-time measurement must exclude the deferred present's wait. It measures the game thread,
    which no longer waits.
  - `LARECOMP_WINDOW_SHOT` captures the real image (the second `vk_frame_end*`).
- [ ] **Step 5: Android.** In `#ifdef __ANDROID__`, when the setting changes and at window creation, call
  `ANativeWindow_setFrameRate(win, frame_gen ? 120.0f : 0.0f, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT)`.
  Get the window from `SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER`, guarded by `__builtin_available(android 30, *)`.
- [ ] **Step 6: Build and check.**
  1. Run `make -C recomp TARGET=linux-aarch64 GAME=smm2 DEPS=… all` (zig cross-build) and
     `make … TARGET=android-arm64 … runtime-objs`. Expected: both build.
  2. Run the build in a `debian:trixie` arm64 container with Mesa lavapipe and Xvfb (one instance; it runs slowly,
     so check correctness, not timing).
     - With `frame_gen = 120` and `LARECOMP_FORCE_DISPLAY_HZ=120`, the log shows `[framegen] on`, no shader errors,
       and a `LARECOMP_FRAMEGEN_SHOT` triplet that looks right.
     - Resize the Xvfb window with `xdotool` mid-run: there is no crash and no validation error
       (`VK_LAYER_KHRONOS_validation` when available).
     - With the setting off, `LARECOMP_WINDOW_SHOT` at a fixed frame matches the same shot from before this task,
       byte for byte.
- [ ] **Step 7: Commit.** `Frame generation on Vulkan: shared passes, present thread for the delayed real frame, Android frame rate`

### Task 6: Docs and project records

**Files:**
- Modify: `PROGRESS.md` (a frame-generation row: Mac / iPad / Vulkan status from Tasks 3–5), `DECISIONS.md` (an
  entry: why it's own interpolation and not MetalFX's, why the game stays at 60, the 100 Hz floor), `recomp/docs/smm2-bringup.md`
  (the `LARECOMP_FRAMEGEN_SHOT` and `LARECOMP_FORCE_DISPLAY_HZ` debug variables, next to the other `LARECOMP_*`)
- Modify: memory `future-120fps-interpolation.md` (it's now done; the plan changed to own interpolation; where it lives)

- [ ] **Step 1: Write the entries** in each file's existing style.
- [ ] **Step 2: Run `scripts/check.sh`.** Expected: `check: all gates passed`.
- [ ] **Step 3: Commit, then push the batch** (`git push origin main`) once the owner OKs the push.
  `Frame generation: progress, decision record, debug variables`
