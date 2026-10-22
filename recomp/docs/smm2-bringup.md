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
- Frame generation (Video tab, `runtime/nvn/framegen*`; DECISIONS.md 2026-10-20) adds an in-between frame per
  real one on displays of 100 Hz and more; the presents the game makes are still paced at 60. Debug variables:
  `LARECOMP_FRAMEGEN_SHOT=<frame>` (+ `LARECOMP_SHOT_PATH`) saves the previous game image, the generated one and the
  current one at the first generated frame from there (one side-by-side PNG on Metal, three files on Vulkan);
  `LARECOMP_FORCE_DISPLAY_HZ=<hz>` stands in for the display's refresh (the Mac's displays are 60 Hz);
  `LARECOMP_FRAMEGEN_ANY_PACE=1` drops the 12-22 ms rule (software renderers); `LARECOMP_FRAMEGEN_COST=1` logs
  each frame's match cost (Metal; tuning `FG_CUT_COST`). Tests: `make -C recomp test-framegen` (logic + passes on
  synthetic images, macOS), `tools/test_framegen_glsl.c` (the GLSL forms through shaderc, Linux).
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

**Linux arm64 on this Mac, in Docker (Mesa lavapipe), 2026-10-23:** `recomp/tools/linux_vk_docker.sh` (the earlier
Lima VM is gone). The runtime and game code are cross-built with zig as the release packages are, and run in a Debian 13
arm64 container on lavapipe under Xvfb (the image holds only Debian packages; the build and the data folder are
mounted at run time, read-only where they can be). The title runs at 13-15 fps, the logos at 60.

    tools/linux_vk_docker.sh image && tools/linux_vk_docker.sh deps     # the image (~1 GB), build/deps-linux-aarch64
    tools/linux_vk_docker.sh build -j10                                  # build/smm2-linux-aarch64 (game code ~10 min)
    tools/linux_vk_docker.sh rundir private/smm2 ~/smm-work/linux-vk/run # links to the game files, own save + smm2.ini
    tools/linux_vk_docker.sh run ~/smm-work/linux-vk/run 240 SMM2_SEED=4 LARECOMP_HOST_INPUT=2386:shot:title

- Runs are muted (`LARECOMP_NO_AUDIO=1`), log in `<run>/stdout.log`, shots in `<run>/build/`. `VK_LAYER=1` adds the
  validation layer; `B=<folder>` runs another build (e.g. an older runtime next to a hard link of the game library).
  `LARECOMP_HOST_INPUT`'s `shot` now works on Vulkan too.
- Space: the game code build is 1.7 GB (`build/smm2-linux-aarch64`; zig's cache, `build/zig-cache`, can go after it).
  With compile-ahead on, a run fills `<run>/cache/spirv` with ~470 MB (6,961 files); `LARECOMP_AHEAD=0` keeps test
  runs small. Each changed GLSL form adds a full set again: four test variants filled 1.8 GB.
- The container has no page write tracking (`[vk] no page write tracking`), so the vertex cache is off there.
- In-course frames: at the title, L + R, A (Make), then Minus (test play), then hold `dpad_right`
  (`LARECOMP_HOST_INPUT=2400:pad:lshoulder:20,2400:pad:rshoulder:20,3000:pad:east:10,3300:pad:back:10,3550:pad:dpad_right:700`).
  The A press lands on Play in some runs (timing).

**Linux arm64 in a VM (Mesa lavapipe), 2026-10-19:**
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
- Shader global memory bound the whole 3.2 GB heap as one storage buffer. That is over lavapipe's
  maxStorageBufferRange (128 MB; VUID-VkWriteDescriptorSet-descriptorType-00333), and clamping the range there broke
  the HUD text (its shaders read past 128 MB). Fixed 2026-10-23 (below, "Shader global memory"): it was also the
  white blob in place of 3D World Mario.
- Comparing captures needs care with colour:
  - Vulkan read-backs are raw sRGB; the Ryujinx references are window-server grabs.
  - Raw against grab is about 23 % of pixels apart on colour alone, for Metal too.
  - Compare raw with raw, or map the raw capture through the colour transform measured from a Metal raw / grab pair
    of the same frame.

**Shader global memory (2026-10-23; from the SM35 port, sm35recomp e292dce and 3fce300):**
- [obs] A storage buffer binding reaches only maxStorageBufferRange of the heap: 128 MB on lavapipe and Adreno, 1-2 GB
  on some desktop GPUs. Past that, lavapipe gave Mario's character draws wrong data: the white blob on the title
  (before: blob at frames 1500 and 2386; after: Mario drawn, and the validation layer's 20 range errors in a 150 s
  title run are gone).
- Now (`g_vk.bda`, device.c): the heap's import is reached through its device address (bufferDeviceAddress, part of
  Vulkan 1.3). Binding 17 holds the address and size; the GLSL reads it through a `buffer_reference` (translate.c,
  only in that mode). The address is fetched like the other 1.2 / 1.3 commands (`VK_LATE_FUNCS`, Windows' old loaders).
  `LARECOMP_VK_NO_BDA=1`: the old storage buffer binding.
- Without host memory import (Android's emulator, many phones; `LARECOMP_VK_NO_GMEM=1` forces it) there is no heap
  on the GPU: global memory read zero. Draw stages now read per-draw copies of their bound storage buffers
  (`gfx_gmap`, exec.c; the compute path's table scheme, filled by the CPU). [obs] On lavapipe without the import the
  title's HUD text was missing (coin and score counters, "Press L + R", the version); with the copies it is back.
  Their stores go to the copy and are lost (they were lost before too). Compute without the import still reads zero.
- Also from e292dce (textures.c): a texture written since the last check is hashed in full, and the periodic full hash
  can take one texture bigger than the frame's budget (a glyph cache that gains a few glyphs slipped past the sampled
  hash: SM35's course cards).
- Metal: unchanged. The 194 translated MSL sources of a title run are byte-identical before and after.
- [obs] What's left of the blob: while Mario faces the camera (his eyes in view: title frames from ~2400 on, in-course
  when he turns) a few texels of the character layer still become NaN after the eye draw (fragment program
  0x729accf900) and bloom white. It is not that shader's maths: with its colour forced to a constant and alpha 1
  (`LARECOMP_DEBUG_FS_OUT=729accf900:0:5`, `LARECOMP_DEBUG_FS_OUT_ALPHA=1`) the NaN texel is still there after it, and
  the scans after the earlier passes find none. Metal with IEEE maths (`LARECOMP_SAFE_MATH=1`) shows no blob. Tried:
  declaring SignedZeroInfNanPreserve in every SPIR-V module (so drivers keep the FMZ and isnan tests); on lavapipe the
  blob then came earlier (frame 2386), so it was not kept. Open: whether a real GPU shows it (the owner's testers);
  if it does, look at the blend inputs (the destination texel under the eyes) next.

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
| NSMBU (WU) | 20 | 9 / 10 (was 8 / 9) | 11 / 10 (underwater 32 / 54 px, max 3-4) | - (was: underwater 3,706 px up to 17 levels) |
| 3D World (3W) | 10 | 3 / 1 | 5 / 7 (castle 68 / 63 px, max 4) | ghost 10-13 px (max 15), water 43 / 50 px (max 77, one spot under Mario's shoe) (was: castle lava 2.28 % > 24, forest poison 0.20 %) |
| **all** | 90 | **72 / 71** (was 71 / 70) | | no course has a pixel > 24 levels off (was 2) |

(2026-10-23, after the fixes below; 0.00 % of pixels > 24 levels everywhere. The courses as the references were made:
`mmk ref-courses` with `R3F=63`, the Koopa and the owner-style header left out (`R3F=127` gives other liquid heights:
M1_castle 7 % off, and the 3D World forest's glints differ). Rerun after rebasing on main's pitch-linear texture
change: the 3D World and 12 SMB1 courses, same numbers within +-1-4-level flicker of a few pixels.)
Per-course tables (pixels differing, > 24 / > 48 / > 96, mean), side-by-sides and diff maps:
`private/smm2/evidence/frames-r3/` (before), `private/smm2/evidence/frames-r4/` (after: logs, tools, the Ryujinx patch).

- [obs] **3D World liquids: the bloom chain read the wrong mip level.** Found with render-target dumps of the same
  frame from both runtimes (below): every pass matched (0.00x % of texels, rounding) up to the bloom's downsample
  from 200x114 to 100x57 (14 % of texels off, then worse down the chain and into the tone-mapped image). That pass
  draws into level 2 of a 400x228 mip chain while sampling level 1 of the same texture (the sampler's LOD clamped to
  1..1). Sampling an attachment of the current pass goes through a snapshot copy (`resolve_feedback`), and the copy had
  only level 0: Metal clamped the LOD to it, so the glow came from the 4x larger level 0, point-like and aliased (a
  sparse, sparkly bloom under every bright highlight - lava, poison water). The snapshot now has every level, sampled
  through the texture's own view, swizzle and type (Metal; Vulkan's snapshots copy every level too). Castle 444,417 px
  (2.28 % > 24) -> 68 px max 4 levels; forest 187,317 px -> 15 px max 1. Other 3D World themes were exact before
  because nothing there is bright enough to reach the lower bloom levels in these frames.
- [obs] **NSMBU underwater: the per-frame clear.** Scene render targets were cleared at their first use in each frame
  (from LASRecomp, against trails where a level never draws). NSMBU's underwater background draws translucent light
  columns before the opaque backdrop, so they blend over last frame's pixels on the Switch and in Ryujinx, over black
  here (pass dumps: the light columns 1-4 units of 10 bits darker, the final frame up to 17 levels). The clear now
  applies to reshaped output only (a window that isn't 16:9, where pixels outside the game's drawing exist). WU_water
  3,706 px -> 32 (max 3), WU_water_night 286 -> 0.
- Not it (tested on the way, each frame left bit for bit as it was): the lava shaders (Ryujinx's own SPIR-V for the
  lava's vertex and fragment programs, through spirv-cross, run in our pipeline with `LARECOMP_MSL_OVERRIDE`: the
  same frame to the pixel), sampler LOD bias, IEEE maths, polygon-offset scale, NSMBU's fragment shaders (all
  replaced by Ryujinx's translations: no change).
- Left: 3D World water, 41 px under Mario's shoe (max 77 levels, deterministic; new with these changes, which
  of the two not yet checked: next, a run with the per-frame clear forced on, then the pass dumps of that frame); 3D World ghost
  10-13 px (max 15); +-1-4-level rounding in a handful of pixels.
- [obs] Twice, on the first capture of a course, the recomp's twinkling stars / bubbles differed (WU snow night,
  WU water); twelve later runs (idle, CPU-loaded, GPU-loaded next to Ryujinx, cold shader compiles) never repeated it.
  Unknown [?]: watch for it.

### Render-target dumps of one frame from both runtimes

- Ryujinx: the trace build (frames-r3 TraceHook.cs) plus a patch (`private/smm2/evidence/frames-r4/ryujinx-dumps.patch`):
  `RYU_DUMP_DIR` (set at the trace shot, after the settle, until the captured present) makes the Vulkan backend write
  each pass's first colour target as raw texels when the targets change; `RYU_DUMP_DRAWS=W,H,x,y,w,h` adds a crop
  after every draw into a WxH target. Audio backend Dummy. The build reproduces the frames-r3 references bit for bit.
- recomp: `RECOMP_TRACE_SHOT_DEBUG=1` makes the trace shot's frame the debug frame; `LARECOMP_DEBUG_RAW=<dir>` writes
  each pass's own first target (the level drawn) as raw texels (`pass_NNN_WxH_<MTLPixelFormat>_<addr>.raw`), after
  every draw with `LARECOMP_DEBUG_EACH_DRAW=1`. `LARECOMP_MSL_OVERRIDE=<dir>` replaces a program's MSL by
  `<dir>/<code address>_vs|fs.metal`.
- Matching the two pass lists by size and content (`tools/match.py` in the evidence) finds the first pass that differs.

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

### Where the frame work stands (2026-10-23)

- Sampler border colours are honoured (`renderer.m` `sampler_for`; castle lava 2.28 % -> 0.22 % px > 24 on its own),
  depth clamp is applied, feedback snapshots carry every mip level, and the per-frame target clear is limited to
  reshaped output (above).
- Next: the 3D World water spot under Mario's shoe; a full 90-course rerun on main (the rerun after the rebase was
  stopped after 22 courses to save credits); menus in lockstep (`RECOMP_PAD_CLOCK=frame RECOMP_PAD_ORIGIN=player`,
  `RECOMP_FRAME_SHOT=k1,k2,...`; Ryujinx `RYU_TRACE_FRAMECLOCK=1 RYU_TRACE_FRAMESHOT`): built, not yet run.

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

- Files don't pile up: each archive file has its pipelines' descriptions next to it (`NNNN.txt`: entry-point names
  and fixed-function state), and once there are more than 8 files a start-up rebuilds all described pipelines into
  one new file on the background queue; the files it replaces are deleted at the next start-up (before loading).
  Archives can't be read back, hence the descriptions. (26 files had taken 1.6 s to load.) Files from before the
  descriptions are deleted.

iPad numbers: in the next section.

## Shader prefetch: first-use builds side by side (2026-10-20)

Design: `docs/superpowers/specs/2026-10-20-shader-prefetch-design.md`. Before a submission runs (`nvnQueueSubmitCommands`
runs its lists at once, on the game thread), `backend_prefetch` walks them on a copy of the executor state and starts
every program, pipeline and compute program their draws will need, on worker threads. A draw then waits only for its
own build, and is never skipped (the one-shot LUT / light-probe renders stay). Both backends; `buildsync.c` is shared.
Measured with seed-4 boots to the title course and the Make / Play menu (`recomp/tools/shader_ab.sh`), blocking
(`LARECOMP_NO_SHADER_PREFETCH=1`, the old default) against prefetch:

| | worst frame | game-thread build stall | reached |
|---|---|---|---|
| macOS M4 Pro, cold, blocking | 19,557 ms | 21,867 ms inline | frame 4200 at 105.8 s |
| macOS M4 Pro, cold, prefetch | 2,794 ms | 4,143 ms of waits | frame 4200 at 84.1 s |
| macOS warm, blocking / prefetch | 241 / 236 ms | 66 / 4 ms | 76.6 / 74.4 s |
| iPad Pro M5, cold, blocking | 6,944 ms | 7,607 ms inline | frame 2400 at 50.8 s |
| iPad Pro M5, cold, prefetch | 2,631 ms | 3,064 ms of waits | frame 2400 at 46.4 s |
| iPad warm, blocking / prefetch | 210 / 210 ms | - | - |

- [obs] Boots aren't byte-repeatable over these frames: two runs of one binary differ by up to 2 % of pixels in the
  intro, ~0.05 % on the title course and ~0.5 % in the menu (loading and the menu move with timing). Frames are
  compared within that noise (`shader_ab.sh --compare <ref> <cand> <noise runs>`; `tools/frame_diff.m`). Prefetch is
  within it everywhere: Metal warm / cold / mirror mode, Vulkan (lavapipe) frames 300-3000.
- [obs] The scan foresees every build: `unforeseen 0` on both backends, and `LARECOMP_PREFETCH_VERIFY=1` (its state
  against the executor's after each submission) stays silent. Scan cost: ~0.33 ms a frame on the Mac and ~0.28 ms on
  the iPad on the title course (~1,000 draws a frame), ~0.1 ms in the menu.
- [obs] Vulkan: `begin_pass` left the formats past the bound targets from earlier passes in `pass_color`, and pipeline
  keys carry all eight - one pipeline was cached as several, and the scan's keys missed (222 unforeseen builds, 9.6 s
  of waits on the title load in lavapipe). Cleared per pass now: 0 and 131 ms.
- The iPad's first launch after an install is cold too (the new binary misses Metal's system cache): 41.6 s of builds
  on the worker threads there, 0.9 s of game-thread waits.
- Ordering (from the branch review): a program's waiting pipeline requests go out before it shows ready, under the
  lock the scan's check-and-add takes, so a draw that sees it ready finds them and a reset's drain can't see zero in
  between; jobs count as in flight until they're done with their object; Metal's pending table is keyed by the
  pipeline's own key, so a pipeline the scan built from a wrong pass prediction is never a draw's. Test switches:
  `LARECOMP_DEBUG_CONT_DELAY_MS=<ms>` (a pause in that window: before the fix 73 / 31 builds the scan had started
  counted unforeseen on Metal / Vulkan, after 0) and `LARECOMP_DEBUG_MISPREDICT=1` (Metal: a wrong pass prediction;
  before, almost every pixel of every frame wrong; after, frames within noise, the draws building their own).
- `[shader] prefetch: ...` every 600 frames: draws / submits scanned and the time, builds started ahead, waits, and
  builds the scan didn't foresee. A wait over 2 s is logged once per object (`[shader] waited N ms for ...`).

## Shader compile-ahead (2026-10-23)

The first time a course of a style / theme shows on a machine, its shaders compiled as the course started: 50-100
sources (3-9 s of compiler time), draws waiting for them 1-2 s in all. Once seen they are in Metal's cache, but that
is every new player and every new version, and worst on iOS. The game sets a model's programs only shortly before
its first draw (2026-10-21: 0.1-1 s for most), so compiling from there halves the hitch at best. Now the shaders are
compiled ahead, from what the game will draw (`runtime/nvn/shader_ahead.h`, Metal in `renderer.m`, Vulkan in
`vulkan/ahead.c`; decision in DECISIONS.md, 2026-10-23):

- **Where the shaders are.** SMM2 has few shader archives of its own: each model (`Model/*.zs`, the models in
  `Pack/*_Model.pack`: zstd, then a SARC) carries a `Block_UBER.bfsha` with the variations it uses, the layouts carry
  BNSH, the effects a `.ptcl`, agl and gsys `.sharcb` archives (`Pack/Static.pack`). The scan reads the RomFS folder
  (`g_romfs_dir`), opens SARC / Yaz0 / zstd, and takes every 256-byte aligned NVN shader blob (`0x12345678`, zeros,
  a Maxwell SPH at +0x30), so the containers' own formats don't matter: 7,963 distinct vertex / fragment shaders and
  4,298 BNSH programs, about 6,900 distinct MSL sources (140 MB of MSL). 1,800 don't translate: ~920 fragment shaders
  in 3D World / NSMBU models use `VOTE`, which the translator lacks (no game run here ever compiled one), and some agl
  blobs have no padding before their constants. Reading and translating everything: 4 s warm, 10-25 s from a cold
  disk.
- **One shader alone.** A fragment shader's MSL depends on its vertex shader only where that one doesn't write an
  input it reads (`vs_out_mask`); alone it's translated as if all were written, which matches the game's own pairs
  almost always. The BNSH variations are also taken whole (models' exact pairs, geometry stages). The effects pair
  their shaders differently from their BNSH's variations, so they're only covered shader by shader.
- **The programs the game sets.** `nvnProgramSetShaders` (objects.c) copies each new program for the compile-ahead
  (`shader_ahead_program_set`): the game sets ~3,000 programs at boot (agl's post effects, the styles' models: a few
  hundred of them are drawn later) and a course's own as it loads. Those set in the last 10 s go first, newest first;
  the boot backlog oldest first, one for each shader of the scan's rank 0, then all of it.
- **Order.** SMM2's ranks (`games/smm2/shader_rank.c`): 0 the models every course draws (`Pack/StaticModel.pack`) and
  the course themes' backgrounds and ground (`Model/<style>_DV_*`, `_Field_*`, read by the game only a second or two
  before the first frame) with the layouts; 1 the styles' models and the effects; 2 the rest (story mode, Mii
  costumes); 3 agl / gsys. Ahead of its turn: a file the game reads (4 MB at most: a model, not a pack read at boot) and
  a game's hint - SMM2 names the next course's style and themes as soon as it knows them (the next course of an
  Endless / 100 Mario run, any course download: `smm2_shader_hint_course`), so its models are read first.
- **Not in the game's way.** 4 compiles at a time (2 on iOS) at utility priority, and none while the game builds or
  has built in the last 1.5 s (`build_idle_ms()`; the game's bursts at a scene change) or the device is hot / in low
  power mode. The translator is thread-safe now (its one static buffer ring is per thread).
- **Where it lands.** Metal: Metal's own shader cache (sources it already has cost ~0.3 ms). `cache/ahead.done` marks a
  finished pass (executable size and time, the key, the salt): later starts skip the files, and the programs the game
  sets still go through it. Vulkan: `cache/spirv/` (ours, it stays), `cache/ahead_vk.done`.

Measured (macOS M4 Pro, a machine shared with other agents' game runs: noisy). 100 Mario (`RECOMP_HUNDRED_AUTO=0`,
easy) and Course World (Courses, the first course, Play) against a local server with one test course, cold
(`LARECOMP_SHADER_SALT` new per run, `LARECOMP_NO_PRECOMPILED=1 LARECOMP_NO_PIPELINE_CACHE=1 LARECOMP_NO_SHADER_WARM=1`),
the course starting ~110 s after boot. "Ahead finished before": a long run of 100 Mario on an SMB1 course until the
pass finished (652 s), then a new run on that data folder and salt with the other style - a style / theme the game
never drew on this machine. Long frames (>= 100 ms) include the game's own course load: one ~540-750 ms frame as the
card gives way, and on NSMBU ~0.9 s of first-frame work, with no compiles and no waits, also when everything is warm.

| Course start | the game's compiles (compiler time) | draws waiting for builds | long frames, in all |
|---|---|---|---|
| 100 Mario, NSMBU: no compile-ahead | 105 (8.7 s) | 1.8 s | 2.6 s |
| - compile-ahead since boot (110 s) | 105 (1.1-3.1 s) | 1.0-1.4 s | 2.0-2.3 s |
| - compile-ahead finished before | 101 (0.11 s) | 0.09 s | 1.5 s |
| 100 Mario, 3D World: none | 48 (2.8 s) | 2.1 s | 2.1 s |
| - since boot | 48 (1.3 s) | 1.6 s | 1.5 s |
| - finished before | 46 (0.15 s) | 0.05 s | 0.5 s |
| Course World, NSMBU: none | 101 (7.4 s) | 1.0 s | 2.5 s |
| - since boot | 101 (1.6 s) | 0.8 s | 2.2 s |
| - finished before | 97 (0.03 s) | 0.01 s | 1.5 s |

(Compiles: the 14 s from the course start, or 20 s from the course download in Course World. Waits: the prefetch
stats lines of the 35 s from the download - a draw waiting for its program or pipeline. NSMBU and 3D World: one run
each for "none" and "finished", 1-6 for "since boot".)

- The title and menus while it compiles (30-95 s after boot): 58.9-59.5 fps on average against 58.7-59.1 without, the
  worst half second 25-31 fps against 10-20 (the boot's own cold compiles make both); the boot's long frames the same
  (4.2-5.0 s against 5.1-5.2 s). Memory: no measurable change (2.4-3.3 GB either way).
- A whole pass: 6,933 sources compiled in 652 s while the game ran (about 10 a second: they wait for compile slots
  most of the time; the game uses the CPU too); 20 s again when Metal has them all (a new version).
- Since boot, the first course gets part of it: by ~110 s the pass is at rank 0-1. Of the sources a fresh machine's
  first NSMBU course compiles, 81-86 were compiled ahead and 17-25 weren't yet (agl's and the effects' programs the
  game set at boot, still in the backlog).
- Left for the first use: pipelines (Metal's back end, per pipeline state; the pipeline cache keeps them after the
  first time), programs with integer vertex inputs or render targets or mirror mode (another key), and the game's
  own course load. Not measured yet: iOS, and the Vulkan path at all (it compiles for Windows; no Vulkan run here).
- Tools: `make build/test_shader_ahead` (`tools/test_shader_ahead.m`: scan, translate and compile a RomFS folder
  alone: `-n` translate only, `-j` jobs, `-s` salt, `-q` QoS; `AHEAD_PER_FILE=1` sources per file); in the game
  `LARECOMP_AHEAD=0 | force | map`, `LARECOMP_AHEAD_LOG=1` (each source the game compiles: compiled ahead or not, and
  where the scan read it), `LARECOMP_AHEAD_JOBS`, `LARECOMP_AHEAD_QUIET_MS`, `LARECOMP_LOG_GAPS=<ms>` (each frame that
  came that late), `LARECOMP_LOG_WAITS=<ms>` (each draw that waited that long for its program / pipeline).
