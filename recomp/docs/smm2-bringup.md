# SMM2 v3.0.3 bring-up: findings

What Super Mario Maker 2 v3.0.3 needed from the runtime forked from LAS Recomp, in the order it came up, and how the
title screen was checked against Ryujinx. Names and behaviour only: no game code, disassembly or assets.

## Toolchain

- Both modules recompile with LAS's generator (`sdk` ~34,000 functions, `main` ~105,000). Two functions in `main`
  are giant `switch` dispatchers (~40k instructions each) that took over 20 minutes each at `-O1`; functions whose
  all-label switch exceeds 16k instructions are now emitted `optnone` (`tools/gen.py`, `HUGE_SWITCH_FN`).
- The game code builds into its own library (`smm2game.dylib`, entry `recomp_game_tables`) next to the runtime
  executable, as LAS does.

## Kernel

- Per-core scheduling. SMM2 pins its worker threads to cores (SetThreadCoreMask 1, 2, 4) and relies on threads of
  one core never running at the same time (job queues without locks). LAS ran every guest thread freely on host
  threads; random crashes and hangs went away once each guest core runs one thread at a time (`runtime/kernel.c`:
  `core_acquire` / `core_release` at each SVC; `RECOMP_NO_CORES=1` turns it off).
- GetInfo(RandomEntropy) is random, but the game's generators seed themselves from the system tick
  (`sead::Random::init()`); see "Repeatable boots" below.

## Services and system data

- `nifm` (offline: no network, as Ryujinx answers), `friend`, `mii` (empty database), `acc` (nickname from
  `RECOMP_USER_NAME`, default "Player"), `am` GetDisplayVersion (`GAME_DISPLAY_VERSION`, 3.0.3: shown on the title
  screen).
- Shared fonts (`pl:u` RequestSharedFontLoad and the font data, from the firmware's font archives; the `.bfttf`
  header is big-endian) and system data archives opened by data id (MiiModel 0802, NgWord 0806 / 0823, TimeZone
  080e). `games/smm2/sysdata.py` extracts them from the player's firmware with their keys, read in place.
- RomFS is served from a folder through an in-memory index (`runtime/romfs_build.c`): the update's files over the
  base game's, as links, nothing copied.

## NVN

- API version: SMM2 checks NVN 53.311 (device integer 1 = 311) and dereferences a null global otherwise.
- Command lists: nested recording in shared command memory and CopyCommands. Lists are recorded into host
  buffers and copied out at EndRecording (the handle is the host copy); the guest's command-memory use is estimated
  so a list's lifetime follows the memory the game reuses.
- Virtual memory pools: a 112 MB virtual pool mapped whole onto a physical pool aliases its storage.
- Textures sharing storage alias one Metal texture.
- Display pacing at 60 Hz from presents (the game would run at 120 fps).
- **Conditional rendering.** SMM2 clears a render target on its first bind each frame by guarding ClearColor with
  SetRenderEnableConditional on a per-target report, then resets the report (ReportCounter / ReportValue /
  ClearBuffer) so later binds skip the clear. LAS ignored the condition, so every later bind cleared the layout
  layers drawn before it: every frame was black. SetRenderEnable(Conditional), ReportValue and ClearBuffer now run
  in submission order; the condition compares the two 64-bit counters at the address and address + 16 (mode 0:
  render if equal, 1: if not equal), as Ryujinx does.
- **3D render targets.** The colour-correction LUT (an 8x8x8 3D texture) is filled with eight colour targets, one
  slice each (a view's layer is a depth plane in Metal).
- **First-use shader compiles block.** LAS compiled programs in the background and skipped their draws until ready;
  SMM2 renders the LUT and light probes once, so a skipped draw stays missing (black scene). Blocking compiles plus
  the warm-up cache; `LARECOMP_ASYNC_SHADERS=1` restores LAS's behaviour.
- **Narrow vertex strides.** Metal needs vertex strides in multiples of 4; streams with a 1- or 2-byte stride
  (SMM2's terrain stores small per-vertex indices that way) are repacked into padded records. Before that, 3D World
  terrain was drawn as screen-sized polygons.
- **UpdateUniformBuffer order.** The guest GPU runs it in command order; we apply it on the CPU while encoding, so
  it now first waits for encoded work that reads the same memory in place (vertex, index and storage buffers,
  compute uniforms; tracked in 64 KB pages).
- Cube-array views of 2D arrays (72-layer character reflection maps).
- Culling both faces (NVN Face FRONT_AND_BACK) draws no polygons; Metal has no such mode and the renderer had
  fallen back to no culling. The 3D World background switches a clear pipe's opaque yellow stand-in off that way.
- A texture rendered to before it was ever sampled first gets its memory's contents, as on the GPU. Each 2D style's
  tile sheet is loaded from a file and the animated tiles are then drawn into that same texture every frame; only
  the animated cells existed, so ground, blocks and castles were missing.
- Geometry shaders (Metal has none). The vertex shader becomes a `vs_main()` function and the geometry shader a
  code fragment of one combined vertex function; each input point is an instance drawing the emitted triangle
  strip, the n-th EmitVertex chosen by the vertex id (`maxwell/shader.h`). Point input with one strip per
  invocation, which is what SMM2's 2D tiles and some effects use.

## Maxwell shader translator

- `TLD4` (gather, with AOFFI offsets), `TMML` (level-of-detail query, 8.8 fixed point like Ryujinx's), and a fix
  for `LDC.64` whose address register is also its first destination (the second word was read from the address the
  first load had just overwritten: the layout vertex shader fetches each corner's texture coordinates that way, and
  the title logo, copyright line and coin icon were invisible).
- Compute results are flushed before draws that read them; textures sampled with `TLD4` in compute work.
- `LDC` offsets are signed when added to the register (as in Ryujinx's translator). SMM2's skinning shaders read
  bone matrix rows at `c[6][Rx - 0x10]`; taken as +0xfff0 that read past the bone palette and threw every skinned
  mesh off screen (Mario's body, the Koopa Car), leaving only rigid parts.

## Repeatable boots

The title screen's course comes from the game's global `sead::Random`, which seeds itself from the system tick at
start-up: slot `(n * random) >> 32` of `Course/title_course_data_NN` with n = 8, or 10 once the save unlocks the
last two. `SMM2_SEED=<n>` (`games/smm2/seed.c`) fixes that seed: frame 2400 is then bit-identical from boot to
boot. Seed -> slot: 4 -> 00 (3D World), 2 -> 01 (SMB1), 1 -> 02 (SMB1), 6 -> 05 (SMB3), 3 -> 06 (SMB3),
10 -> 07 (NSMBU); 03, 08 and 09 did not come up in 22 seeds with this save.

For Ryujinx the course is chosen with a LayeredFS mod making all ten slots one course (`titlemod.py --course N`).
The same mod in the recomp crashes while loading the title scene when the course is not the slot's own (open issue
below). An exefs patch seeding Ryujinx's generator instead made Ryujinx exit at start-up every time.

## Title screen against Ryujinx (2026-10-19)

Current numbers (after the second round of fixes; window-server captures of both windows, 1280x720 game area, the
recomp at render scale 1 like Ryujinx's docked 1080p, the best-matching recomp frame of 2300..2520 against one
Ryujinx grab; % of pixels differing by more than 24 / 48 / 96 in some channel):

| Course (style) | Recomp frame | > 24 | > 48 | > 96 | at frame 2400 (> 24) |
|---|---|---|---|---|---|
| 00 (3D World) | 2376 | 2.50 % | 0.66 % | 0.13 % | 2.72 % |
| 01 (SMB1) | 2460 | 4.17 % | 2.25 % | 0.99 % | 6.12 % |
| 02 (SMB1, underground) | 2480 | 4.80 % | 2.47 % | 1.05 % | 5.37 % |
| 05 (SMB3, desert) | 2488 | 4.71 % | 2.98 % | 1.15 % | 4.79 % |
| 07 (NSMBU, snow) | 2304 | 2.99 % | 0.94 % | 0.27 % | 3.58 % |

What is left in the difference maps: edges of pixel-art tiles and text (texel-boundary sampling / scaling, a pixel
here and there), the "Press L+R" button's blink and the moving characters (animation phase). Ryujinx against
itself, five boots of the same course: 0.3-1.8 %.

Animation phase cannot be lined up exactly: Ryujinx's grabs are timed by the wall clock and its boots differ in
length (the title timer read 476-481 across five identical boots at the same grab time), and the reference build
exposes no frame count. The recomp is frame-exact, so the nearest frame is found by searching a window of recomp
frames for the best match (`LARECOMP_CG_SHOT=<first>:<last>:<step>`).

Earlier (R1, first round):

- Reference: Ryujinx 1.3.2, the same dump (v1.0.0 + v3.0.3 update), private data root, window grabbed 48 s after
  launch on five boots (identical course; boot-to-boot difference 0.3-1.8 % of pixels by more than 24).
- Ours: `SMM2_SEED=4`, frame 2400, window captured the same way (`LARECOMP_CG_SHOT=2400`, window 1280x720): both go
  through the window server's colour management. (Comparing our raw frame with Ryujinx's window grab shows a
  uniform colour shift: 26.6 % of pixels by more than 24.)
- Result (1280x720, `imgcmp.py`): 6.96 % of pixels differ by more than 24 in some channel (3.80 % by more than 48,
  1.30 % by more than 96), mean difference 7.6.
- Remaining differences then: Mario's model drawn wrong (a blue block), the Koopa Car missing (a grey pole in its
  place), animation phase; all but the phase fixed since (above).
- Boot sequence: the yellow "SUPER MARIO MAKER" / "2" logo screen with the running SMB1 Mario, then the title, as on
  Ryujinx.

## Windows and Linux: the Vulkan backend (2026-10-19)

The ports are built on macOS with zig 0.17 (`make TARGET=windows-x86_64 | linux-x86_64 | linux-aarch64`). The
generated C needs no changes: arm64 shares `generated/smm2` with macOS, and x86-64 uses the portable-C lowering
(`generated/smm2-c`).

**Build fixes:**
- SDL 3.2 has no `SDL_LoadPNG` or `SDL_SavePNG`. A minimal PNG writer takes over when SDL is older than 3.4.
- `__builtin_cpu_supports` needs libgcc's `__cpu_model`. The AVX2 check now reads CPUID directly
  (`platform/x86_cpu.h`).
- zig's lld-link has no `--export-all-symbols`. Linker directives (`platform/windows_exports.c`) export the eight
  runtime symbols the game code calls, and `zig dlltool` makes the exe's import library from `windows_exports.def`.
- zig wants `X.lib` import-library names, not MinGW's `libX.dll.a`.
- SMM2 has no movie library, so FFmpeg is not linked (`GAME_MOVIES := 0`).

**What the Metal side had, ported to Vulkan (`runtime/nvn/vulkan`) and to the GLSL output of the translator:**
- Conditional rendering (`SetRenderEnableConditional`), `ReportValue` and `ClearBuffer`.
- Polygons culled on both faces are skipped.
- 3D colour targets: a depth slice is drawn through a 2D view of a `2D_ARRAY_COMPATIBLE` image.
- First-render uploads.
- Storage aliases: texture objects over the same memory share one image.
- Cube-array views of 2D arrays.
- In-order `UpdateUniformBuffer`: a flush when guest memory under recorded draws would change.
- A flush after storing compute dispatches.
- Blocking shader and pipeline builds.
- TLD4 (`textureGather`) and TMML (`textureQueryLod`).
- Geometry shaders as real Vulkan geometry stages, in descriptor set 2. They have point input, and the output topology
  and vertex count come from the shader header.

**Linux arm64 in a VM (Mesa lavapipe):**
- The build boots to the title screen.
- The Mario model in the 3D World title course (2026-10-19). How SMM2 draws him:
  - A pass with two targets draws him into a character layer: target 0 (1600x900 RG11B10F, blended) and target 1
    (RG16F, not blended; cleared to 1.0, his view depth in R and a rim glow in G).
  - A composite in the scene pass then draws a box around him. Its shader discards where target 1 is 1.0, compares the
    depth in target 1 with the scene depth, and copies the character layer into the scene.
- Two causes kept him out, both fixed:
  - [obs] The composite drew nothing at all. With its output forced to a constant and its discard removed, the scene
    did not change (`LARECOMP_DEBUG_FS_OUT`, `LARECOMP_DEBUG_FS_NODISCARD`, `LARECOMP_DEBUG_PROBE_DRAWS`). Its box
    vertices are in a per-frame buffer 2.6 GB into the guest heap, read in place from the imported heap. Lavapipe
    allows 2 GB allocations (maxMemoryAllocationSize); the 3.2 GB import still works there and shader global memory
    reads past 2 GB fine, but vertex and index fetches past 2 GB read other memory. Copying the vertices
    (`LARECOMP_VK_COPY_VERTICES=1`) made the box draw.
    - Fix (`guest_direct`, exec.c): vertex / index data is read in place only within the device's
      maxMemoryAllocationSize and maxBufferSize; past that it is copied, as anywhere outside the heap.
  - [obs] Target 1 was never written by his draw: a texel probe read 1.0 (0x3c00) at his pixels while target 0 had
    his colour. It was written whenever his draw started a new command buffer (a probe's flush before it), never when
    it followed the earlier draws in the same command buffer, with or without a pass restart in between
    (`LARECOMP_DEBUG_PROBE_LAZY`).
    - Not on our side, as far as can be checked: the validation layers report nothing in that pass, the barrier before
      every pass is a full one with every image in GENERAL layout, and the state filter (`LARECOMP_VK_STATE_FILTER=0`)
      and vertex copies change nothing. A standalone Vulkan program with the same targets, formats, blend split,
      clears and several pipelines writes target 1 correctly on the same lavapipe, so the trigger is narrower.
    - Workaround (`lavapipe_mrt_split`, exec.c), lavapipe only: in a pass with several targets, some blended and some
      not, a draw whose program differs from the previous draw's starts a new submission.
      `LARECOMP_VK_MRT_SPLIT=0 / 1` forces it off / on for any driver. Real drivers are not expected to need it.
  - [obs] With both fixes the probe reads his depth in target 1 and the composite copies him into the scene (pass
    images: he is there, as on Metal). The final frame still shows a white blur where he stands: a few texels of the
    character layer become NaN (`LARECOMP_DEBUG_NAN_SCAN=1`: 5-15 texels at his eyes, after the draw with fragment
    program 0x729accc000; its colour alpha comes out NaN), and the bloom spreads them. Whether real drivers produce
    them too is open.
- Shadow sampling: where there is no shadow map, SMM2 binds a 4x4 R32_FLOAT texture ("DepthShadow") to a shadow
  sampler. Vulkan compares only depth formats (validation: VUID-vkCmdDrawIndexed-None-06479); the Switch compares the
  red channel. Such a texture is now sampled through a D32_SFLOAT copy made in the submission's upload commands
  (`vk_shadow_view`, textures.c).
- Shader global memory binds the whole 3.2 GB heap as one storage buffer. That is over lavapipe's
  maxStorageBufferRange (128 MB; VUID-VkWriteDescriptorSet-descriptorType-00333), but lavapipe reads it correctly,
  and clamping the range there broke the HUD text (its shaders read past 128 MB). NVIDIA, AMD and Intel allow large
  storage buffers; left as it is for now.
- Comparing captures needs care with colour:
  - Vulkan read-backs are raw sRGB; the Ryujinx references are window-server grabs.
  - Raw against grab is about 23 % of pixels apart on colour alone, for Metal too.
  - Compare raw with raw, or map the raw capture through the colour transform measured from a Metal raw / grab pair
    of the same frame.

**Where this stands (2026-10-19, paused):**
- Vulkan Mario: both causes fixed (vertex data past the allocation limit; the lavapipe-only draw split). Open: the
  NaN texels at his eyes (fragment program 0x729accc000, alpha NaN) that bloom into a white blur, on lavapipe.
  Paused until the owner's Windows test on a real GPU shows whether it happens there. Next step if it does: find
  the instruction that makes the alpha NaN (an output-side tracer: the NaN checks on registers were folded away by
  lavapipe's compiler, so trace the outputs instead), and compare with the same values on Metal.
- Windows package: builds from main with zig. The first-launch build emulated from the package (its own generator,
  headers and patches, mods' patch points included; 908 files, linked against its import library) produces the game
  code DLL. Not run on Windows; no zip on the Desktop (the owner paused Windows testing).
- macOS title frame vs Ryujinx (seed 4, frame 2386; 2.35-2.5 % of pixels > 24 apart): the differences sit on edges
  everywhere (outlines of the logo, tiles, text), not in flat areas. [obs] Sampling our window grab 0.25-0.5 px
  higher halves them (2.69 % -> 1.27 % on a bilinear resample); 1 px shifts and blurring don't. So most of what's
  left is a sub-pixel vertical offset between the two grabs. Not yet known whether it's in the rendering (a half-pixel
  in the viewport / present) or in the capture (a Retina window grabbed at 1x, its 32 px title bar cropped). Next
  step: the same test on the raw read-back mapped through the colour transform (`cmtransfer.py`); if the offset
  stays, check the present pass's texel centres against Ryujinx's. Text and the blink timing are untouched.

## Open issues

- ~~A title course swapped into another slot crashes the recomp; Ryujinx runs the all-slots mod.~~ Not a recomp bug
  (R2, [obs]): Ryujinx does the same once it is made to pick the same slot. With the game's course 00 in every slot,
  Ryujinx with every `sead::Random` seeded 4 (slot 00, the course's own) runs, and seeded 1 (slot 02, like the
  recomp's `SMM2_SEED=1`) exits while the title scene loads, as the recomp crashes there; courses 03 and 06 in slot 00
  end both runtimes too. SMCraft's test courses load in any slot in both. Ryujinx usually picks slot 00 on its own
  (its boot tick), which is why the all-slots mod seemed to work there.
- Geometry shaders with other input primitives, several strips, or textures are not handled (draws skipped).

## Input replays against Ryujinx (R2, 2026-10-19)

### Input

- [obs] SMM2 reads its controllers with `nn::hid::GetNpadStates` (16 entries, FullKey for player 1). The SDK's own
  version reads hid shared memory, which the runtime never filled, so up to R1 the game saw no input at all (keyboard
  included). The runtime now answers `GetNpadStates` itself (`runtime/hle/input.c`): each call is one new sample, the
  older entries repeat it with earlier sampling numbers.
- Scripted pad (`runtime/padscript.h`, `RECOMP_PAD_SCRIPT`): the Ryujinx trace tooling's scripts as they are
  (`N BUTTONS` per line; JUMP=A RUN=Y SPIN=ZR), one script frame per tick of a game clock, never wall time; keyboard,
  mouse and controllers are left out while it runs; sticks at rest, the d-pad stays the d-pad (Ryujinx's override
  does the same).
- The clock and the trace point are the player's per-frame update. `games/smm2/patches.py` patches its first
  instruction after the prologue (main+0x158a5c4, `x19 = x0`) into a call to `smm2_player_hook()`
  (`games/smm2/trace.c`) followed by the original move: the same instruction the Ryujinx trace build turns into
  `svc #0x7E`. At each update the hook writes a record (TraceHook's format: the 0x3000-byte player object and the
  0x200-byte input block it points to at +0x550), then applies the next script frame.
- [obs] Latency matches Ryujinx's: the pad state set at update k is read by update k + 1, so script frame j shows from
  record j + 2 on, in both runtimes.
- Menus have no player: `RECOMP_PAD_CLOCK=frame` ticks per presented frame instead. On the title screen L + R opens
  the Make / Play menu; Make, then − (after the first-run dialogs) starts a test play of the Course Maker course
  [obs, screenshots in the evidence folder].

### Method

- Test courses: SMCraft's physics test courses (`mmk trace-courses`, in the full CLI on the `smcraft-archive`
  branch), `<style>_<kind>` for the five styles and flat / wall / slope / water / items / form.
- Same start in both runtimes: the test course is installed as all ten title courses (recomp `RECOMP_MOD_DIR`,
  Ryujinx LayeredFS), and the script starts at the first update of the title-course player (Ryujinx: the `run`
  command is armed before the player exists). One script per course plays every test of that course kind with a
  reset between tests (run left into the course's edge, settle, walk right to about x = 100, settle); the resets are
  sized once from a recomp pre-run and are then the same frames in both runtimes, so record k is the same game frame
  in both. `tools/replay_sync.py` builds the sessions, runs both runtimes and compares.
- Compared per update: x, y, vx, vy, facing, speedF, state id and frames in state (bit for bit), plus the whole
  0x3000-byte player object and the input block, with pointers compared by what they point into (the two runtimes'
  heaps are at different addresses).
- Randomness: the title screen picks one of ten courses and the game makes several `sead::Random` generators, each
  seeded from the system tick. The test-course runs need only the recomp's `SMM2_SEED=4` (all ten slots hold the same
  course). For the game's own title course, `SEEDALL=1` seeds every generator with 4 in both runtimes
  (`SMM2_SEED_ALL=1`; on Ryujinx an IPS mod turns the tick read in `sead::Random::init()` into the constant).
  Without that, Ryujinx picked a different title course and nothing could be compared.

### Results

| session | tests | updates compared | identical | test frames | identical | first divergence |
|---|---|---|---|---|---|---|
| M1 / M3 / MW / WU / 3W `_flat` | 34 each | 14,665 / 14,665 / 14,956 / 14,628 / 14,730 | all | 4,096 each | all | none |
| `_wall` (5 styles) | 4 each | 3,153-3,207 | all | 1,586 each | all | none |
| `_slope` (5 styles) | 4 each | 2,749-2,770 | all | 1,195 each | all | none |
| `_water` (5 styles) | 2 each | 1,066 each | all | 263 each | all | none |
| `_items` (5 styles, as Super Mario) | 34 each | 15,245-15,339 | all | 4,336 each | all | none |
| `_form` (M3 raccoon, MW cape, WU propeller, 3W cat) | 16 each | 7,646-7,671 | all | 2,281 each | all | none |
| stock title course to the goal (Koopa Car, enemies) | 1 | 1,911 | all | 1,909 | all | none |
| stock title course, varied inputs, ends in a death | 1 | 3,537 | all | 3,534 | all | none |
| **all 31 sessions** | **456** | **221,189** | **221,189** | **71,947** | **71,947** | **none** |

Per-session tables (each test, frames compared, identical, first divergence): `private/smm2/evidence/r2/results/`.

- [obs] No divergence in position, speed or state anywhere. The whole player object differs only in one 4-byte word
  (+0x644) during 4 updates of the title-course runs: it holds the low half of a heap address (0x0af69540 in Ryujinx,
  0x02a65540 here, next to two floats with the same values in both), so it differs by design [inf].
- [obs] The recomp repeats itself: a second run of the M1_wall session is identical record for record.
- [obs] Reaching the goal ends a trace in both: the title course starts again with a new player object and the
  script, which follows the first one, stops (stock_clear: after 1,911 updates in both). After a death on the title
  course (stock_varied) the same player object keeps being updated in its death state to the script's end.
- [obs] Before R2 the recomp had never been given input, so none of this had been exercised: no lifter, FP/SIMD or
  scheduling bug showed up in 221,189 compared updates, and none needed fixing for R2.
- [obs] A game title course in another slot than its own crashes on loading in both runtimes alike (recomp: a null
  read; Ryujinx: the emulator exits at the same point; see "Open issues"), so the title screen apparently expects
  each slot's own course [inf]; SMCraft's test courses load in any slot.

## In-course frames against Ryujinx (lockstep, 2026-10-19)

### Method

- Courses: SMCraft's renderer reference courses (`mmk ref-courses`, full CLI on the `smcraft-archive` branch), one per
  style x theme (ground, underground, castle, airship, water, ghost house, snow, desert, sky, forest) and night for the
  four 2D styles: 90 courses with the same first screen (terrain shapes, seven block kinds, coins, a pipe, a penned
  Goomba). [obs] As archived they all fell back to the blank course in both runtimes: their wrapper left the SARC data
  where the game doesn't expect it (the trace courses' wrapper moves it to 0x100), and a sized Koopa placement makes the
  game reject the course; both changed in a scratch build, outside the repository.
- Same game state: each course is installed as all ten title courses; both runtimes run an idle script from the first
  player update with every `sead::Random` seeded alike (`SMM2_SEED=4 SMM2_SEED_ALL=1`; Ryujinx: the IPS patch), and
  at player update k the game waits 0.5 s and the next presented frame is captured (`RECOMP_TRACE_SHOT`; Ryujinx's
  trace build `RYU_TRACE_SHOT`: the guest's presented texture). Raw RGB at 1920x1080 in both (`LARECOMP_SCALE=1`:
  with the default "Match display" the recomp renders at the window's pixel size, 720p in a small window, and the
  frames are then soft). Window-server grabs weren't possible (no screen-recording permission in the session) and
  raw-against-raw is like for like without the display's colour management.
- Checks: the capture is not off by a frame (the recomp's frames k-2..k+2 against Ryujinx's k: k is the closest by
  far); both runtimes repeat themselves (Ryujinx three runs, the recomp five to twelve).

### Results (updates 900 and 1200)

| style | courses | bit-identical | within 20 px (+-1-4 levels) | more |
|---|---|---|---|---|
| SMB1 (M1) | 20 | 20 / 20 | - | - |
| SMB3 (M3) | 20 | 20 / 20 | - | - |
| SMW (MW) | 20 | 20 / 20 | - | - |
| NSMBU (WU) | 20 | 8 / 9 | 10 / 9 | underwater 3,706 px (max 17 levels, 0.00 % > 24); underwater night ~300 px |
| 3D World (3W) | 10 | 3 / 1 | 5 / 7 | castle lava 2.28 % > 24, 0.03 % > 48; forest poison 0.20 % / 0.05 % |
| **all** | 90 | **71 / 70** | | |

Per-course tables (pixels differing, > 24 / > 48 / > 96, mean), side-by-sides and diff maps:
`private/smm2/evidence/frames-r3/`.

- [obs] The 3D World liquids differ deterministically (both runtimes repeat themselves exactly), so it's a real
  rendering difference. It is confined to the lava / poison surface: one draw (an uber shader with two scrolling
  layers, a shadow-map compare and light probes), later bloomed and colour-graded like everything else (other 3D World
  themes are exact through the same passes). Not: the sampler LOD bias (applying it, possible on Metal from macOS 26,
  moves away from Ryujinx: MoltenVK evidently doesn't either), fast math (IEEE math moves away too), a capture
  offset. Open [?].
- [obs] Twice, on the first capture of a course, the recomp's twinkling stars / bubbles differed (WU snow night,
  WU water); twelve later runs (idle, CPU-loaded, GPU-loaded next to Ryujinx, cold shader compiles) never repeated it.
  Unknown [?]: watch for it.
- The +-1-4 level differences in NSMBU / 3D World are rounding (a handful of pixels per frame).

### Audio (rough)

Each runtime's mixed output dumped (recomp `LARECOMP_AUDIO_WAV`; Ryujinx's trace build `RYU_AUDIO_WAV`, the renderer's
PCM before the host volume), the same session (the stock title course to the goal), aligned by the loudness envelope:
- [obs] Boot and logo audio match: levels within ~0.5 dB per half second, the same silences, waveforms correlating up
  to 0.97 in places.
- [obs] In the course the recomp was ~5 dB quieter overall and 8-12 dB quieter above 2 kHz: voices the game sends
  through a splitter were dropped. The renderer ignored the splitter section, and a voice's "no mix" is 0x7FFFFFFF
  (Ryujinx's `UnusedMixId`), not -1. Fixed (`runtime/services/audio.c`): splitter states and destinations parsed
  (version 1 / 2), voices route channel c to destinations c, c + channels, ..., sub mixes buffer k % buffers to
  destination k (both as Ryujinx's command generator does); the AuxiliaryBuffer and BiquadFilter effects SMM2 also
  uses (mix 7, mix 4) are implemented too.
- [obs] After the fix, over the 39 s of the course: overall level -27.5 dB against Ryujinx's -27.6, every octave band
  from 62 Hz to 16 kHz within 0.6 dB, 70 loud onsets against 69, loudness envelopes correlating 0.76. Sound events
  land at slightly different wall-clock times (audio runs on host time; loading and pacing differ), so the waveforms
  don't line up sample for sample.

### Where the frame work stands (paused 2026-10-19)

- Sampler border colours are now honoured (`renderer.m` `sampler_for`): 3D World castle lava 2.28 % -> 0.22 % px
  >24. Depth clamp (`nvnCommandBufferSetDepthClamp`) is recorded and applied (SMM2 leaves it off; Vulkan ignores it).
  The 90-course recheck with both stopped after 23 courses (all still identical): resume from M1_sky_night.
- Left: 3D World forest (0.20 %: glint quads on the poison water, an effect draw [inf]), NSMBU underwater (+-1-2
  levels, rounding [inf]).
- Menus in lockstep: a presented-frame clock from the first player update on both sides (`RECOMP_PAD_CLOCK=frame
  RECOMP_PAD_ORIGIN=player`, `RECOMP_FRAME_SHOT=k1,k2,...`; the Ryujinx trace build's `RYU_TRACE_FRAMECLOCK=1
  RYU_TRACE_FRAMESHOT`). Built, not yet run against Ryujinx.

## First-use shader compiles: the Metal shader caches (2026-10-19)

iPad build 24 stalled 3.5 s on the first use of a compute program, and every first draw with a new program waited
for its compile. Measured on macOS (M4 Pro, a heavily loaded machine: absolute times are noisy): the stock title
course to presented frame 2750 (both compute programs included), with `LARECOMP_SHADER_SALT` so that Metal's own
cache misses:

| Run | Compile time | Worst 2 s window of compiles | Compute programs built in |
|---|---|---|---|
| cold (nothing cached) | 55-64 s | 22-28 s | 290-455 ms each |
| precompiled shaders, no pipeline cache yet | 21-34 s (back-end only) | 3.6-15.7 s | 540-850 ms |
| precompiled shaders + pipeline cache (steady state) | 3.1 s | 0.6 s | 3-5 ms, from the cache |

- [obs] The front-end (MSL to AIR, `newLibraryWithSource`) was most of the cost (~70 of ~85 s in one cold run),
  pipeline creation (AIR to GPU code) the rest.
- [obs] Metal's own on-disk cache can't be relied on: it's shared by every process and size-limited, and a "second
  launch" an hour of other compiles later was fully cold again (138 s). An app update may clear it too [?].
- Three layers now, all in the data folder's `cache/` (Documents on iOS, which an app update keeps):
  `cache/shaders/` (the translated MSL, as before; warmed in the background at start-up unless precompiled);
  `cache/metallib/` and `metallib/` (precompiled with `tools/precompile_shaders.sh`, Metal's offline compiler, so
  Mac only: `make_ios_app.sh` precompiles `RECOMP_IOS_DATA`'s shaders, plus `RECOMP_IOS_SHADERS` folders, into the
  app); `cache/pipelines/NNNN.bin` (Metal binary archives of the pipelines built, written from a background queue).
- [obs] Saving an archive again after adding to it (or after loading it) failed now and then on macOS 27 ("Failed to
  generate machO ... expecting 'fragment' stage in pipeline no. N") and emptied it, while the same pipelines saved
  fine from a fresh archive: each save is a new write-once file.
- [obs] An archive serves only pipelines built from the same AIR: archives made with precompiled shaders don't serve
  source-compiled ones (0 hits), and the other way round.
- The VS and FS libraries compile side by side; entry points are named `v_/f_/c_<hash>` instead of `main0`.

Left: archive files accumulate (26 after many test runs, 1.6 s to load): merge them now and then. iPad numbers still
to be measured; the `[shader] slow program` and `[compute] program ... built in` log lines show what's left there.
