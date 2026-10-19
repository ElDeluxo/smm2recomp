# Shader prefetch (parallel first-use compiles) — design

Date: 2026-10-20. Status: written from the chat ("do the design"), awaiting spec review. Assumptions not yet
confirmed by the owner are marked [assumed].

## Goal

Shorten the game-thread stalls caused by first-use shader and pipeline builds, on Metal (macOS, iOS) and Vulkan
(Windows, Linux, Android), **without ever skipping a draw**. Rendering stays bit-identical to today's blocking mode.

Today a submission that introduces N new programs/pipelines stalls for the sum of their build times, one after the
other, as each draw reaches them. With prefetch, all N start building in parallel when the submission arrives, and
each draw waits only for whatever of its own build is still unfinished. The stall approaches the longest single
build rather than the sum.

## Constraints

- **No skipped draws.** SMM2 draws some things exactly once (the colour-correction LUT, light probes). A draw
  skipped while its build runs is never redrawn, and the scene stays black (DECISIONS.md, 2026-10-19 "Recomp R1").
  Prefetch keeps that rule: a draw whose build is unfinished waits for it.
- **The scan is advisory.** Nothing that is drawn depends on the scan being right. If the scan computes a wrong key or
  misses a draw, that draw builds on demand and waits, exactly as it does today. This is what keeps the design safe
  while it duplicates some of the executor's logic.
- **Fast path untouched.** Steady-state draws hit `prog_tab` / `pipe_tab` on the game thread with no locks, and they
  keep doing so. Locks are taken only on a table miss.
- **Both backends**, following the existing rule that `renderer.m` and `vulkan/exec.c` mirror each other
  (`exec_list`, `get_program`, `sync_shaders`).
- **The caches stay as they are**: `cache/shaders`, the precompiled `metallib`s, Metal binary archives, the SPIR-V
  cache and the `VkPipelineCache`. Prefetch only changes when a build starts and which thread runs it.

## Approaches considered

1. **Scan at submit, with exact state (chosen).** `nvnQueueSubmitCommands` runs its lists synchronously on the game
   thread (`objects.c`: `backend_execute` for each handle). Just before that, the live executor state `st` is
   exactly the state at the start of the first list. Walking all the submitted lists on a copy of `st` gives the
   exact program and pipeline keys of every draw, with no guessing. The lead time is one submission, and the benefit
   comes from parallelism inside it.
2. **Scan at `EndRecording` (rejected for now).** This gives more lead time, since lists are recorded before they
   are submitted, but the state at list start is not known then and would have to be guessed. `EndRecording` may
   also run on threads other than the executor's, so reading `st` there would race. It can be added on top of (1)
   later as a speculative pass, if (1)'s measurements show that one submission of lead time is not enough.
3. **No scan; build every pipeline in the archive descriptions at start-up (separate).** This uses
   `cache/pipelines/*.txt` and helps only with pipelines seen in earlier runs. It complements (1) and is not part of
   this spec.

## Modes

`sync_shaders()` in both backends becomes a three-way mode, read once:

| Mode | Selected by | Builds | A draw whose build is unfinished |
|---|---|---|---|
| **prefetch** (new default) | — | scan at submit + async workers | waits |
| sync (today's default) | `LARECOMP_NO_SHADER_PREFETCH=1` | inline, on first use | n/a (built inline) |
| skip (LAS's way) | `LARECOMP_ASYNC_SHADERS=1` (unchanged) | async workers, no scan | is skipped |

The stale comment in `vulkan/exec.c` about `LARECOMP_SYNC_SHADERS=1` is corrected to name these variables.

## Design

### 1. State updates factored out of `exec_list`

In each backend, the cases of `exec_list` that only update binding state (`CMD_BIND_*`, `CMD_SET_TEXTURE_POOL`,
`CMD_SET_SAMPLER_POOL`, `CMD_SET_RENDER_TARGETS`, viewports, scissors, depth ranges, stencil, blend colour, depth
clamp) move into `static bool apply_state(ExecState *s, const CmdRec *r)`. It returns false for ops that are not pure
state updates. `exec_list` calls it first and keeps its own logging and debug hooks (`LARECOMP_LOG_RTS`,
`g_debug_frame`, the video-texture warning) around the call. The anonymous `static struct {...} st` gets a type name,
`ExecState`, so that shadow copies can be made.

### 2. Pure key functions shared by the draw and the scan

The keys must come out identical whether a draw or the scan computes them, so both call the same functions:

- **Pass layout.** `pass_layout(const ExecState *s, PassLayout *out)` gives the attachment formats, sample count and
  dropped targets: everything a pipeline key takes from the pass. It has no side effects (no `end_pass`, no
  `renderer_mark_gpu_owned`, no texture creation). On Metal it maps NVN formats to `MTLPixelFormat` without creating
  views, and moves `rt_dropped`'s tile-memory rule into it. `begin_pass` calls `pass_layout` and then does its side
  effects. On Vulkan, `pass_color` / `pass_depth` / `pass_stencil` / `pass_samples` are filled from it.
- **Program keys.** The `vkey` / `fkey` construction in `draw()` moves into `program_keys(const ExecState *s,
  ShaderKey *vkey, ShaderKey *fkey)`.
- **Pipeline key.** Vulkan: `pipe_key(const ExecState *s, const PassLayout *pl, const Prog *pr, int topo_class,
  PipeKey *k)`, holding the code from the `PipeKey k` fill in `draw_()`. Metal: `render_pipe_desc(const ExecState *s,
  const PassLayout *pl, Prog *pr, MTLRenderPipelineDescriptor **pd, NSString **pk)`. On Metal, the slow-path key
  `pk` names the `Prog` pointer instead of `pr->vs` / `pr->fs`, which exist only after compilation. A `Prog` maps
  to exactly one VS/FS pair, so the key's meaning is unchanged, and the scan can form it before the compile is done.

`draw()` keeps its order: program keys, `get_program`, mirror check, skip lists, `begin_pass`, pipeline key. It just
calls these functions.

### 3. The scan

`backend_prefetch(const uint64_t *handles, int n)` is called from `nvnQueueSubmitCommands` before its execute loop.
It returns immediately in sync and skip modes.

- It copies `st` into a static shadow `ExecState` and walks the handles in order, through `CMD_JUMP` and `CMD_CALL`
  (depth 8, like `exec_list`), applying `apply_state` to the shadow. The live `st` is never written.
- At `CMD_DRAW_ARRAYS` / `CMD_DRAW_ELEMENTS`, with a program bound and `poly[0] != 3`: it computes the program keys
  and calls `get_program`, which in prefetch mode starts the translate/compile if the program is new and returns at
  once. It also asks for the pipeline (section 4). Render-enable state is ignored, since a build for a draw that
  turns out disabled only costs CPU.
- At `CMD_DISPATCH`, it does the same for the compute program.
- It does not replicate the mirror variant, the hash skip lists or `pot_level_active`. Mirror-mode composites build
  on demand the first time.
- Only one shadow is needed: the scan runs on the game thread, finishes before execution starts, and the next
  submission starts from the live `st` again.

### 4. Builds and continuations

A pipeline cannot be built before its program is translated (keys read `pr->vi` / `pr->fi`: `attr_mask`,
`out_mask`) and compiled (`build_pipeline` uses `prog->vs/fs`). Workers never wait on each other, which rules out
deadlock. Instead, pipeline requests are attached to the program as continuations:

- `Prog` gains a lock-protected list of pending pipeline requests. Each request holds a copy of the shadow
  `ExecState` subset that the key reads, plus its `PassLayout` and topology class, by value. Copies are made only
  for draws whose pipeline isn't known yet, so this is rare.
- When the program finishes compiling (`compile_program`'s end, on a worker or Metal's completion handler), it drains
  the list. For each request it computes the key, then looks the key up in and inserts it into a **pending table**: a
  small mutex-protected hash map from pipeline hash to `Pipe`, with state building/ready/failed. A new entry gets its
  build job queued. If the program is already compiled at scan time, the scan does this directly.
- On Metal, the archive lookup (`pipeline_cache_find_render`, 3-5 ms each from the cache) moves into the build job
  as well, so warm runs gain too.
- **Draw-time lookup:** on a `pipe_tab` miss, the draw first looks in the pending table under its lock. If it finds an
  entry, it moves it into `pipe_tab` and waits for it if it is still building. If not, it creates the build itself,
  as today. `pipe_tab` stays game-thread-only.

### 5. Waiting

- Programs and pipelines finish through one completion condition: a global mutex plus condvar, broadcast on every
  completion. A draw that needs an unfinished program or pipeline waits on it, re-checking the object's `state`.
  Metal's `Prog.ready` becomes the same three-state `_Atomic int state` that Vulkan uses.
- **Wait-steal (Vulkan):** each job carries an atomic queued/running/done state. If a draw waits on a job that
  hasn't started yet, the game thread takes it out of the queue and runs it itself. That way the draw needed now
  never waits behind later prefetched work. Translation jobs can be stolen the same way: the game thread takes
  `translate_lock`, which compute translation already does. Metal's compiles run in Apple's compiler service and
  can't be reordered. The scan enqueues in draw order, which keeps FIFO close to the order of need.
- A wait longer than 2 s logs `[shader] waited N ms for …` once per object. There is no timeout: an unbounded wait is
  today's sync behaviour.
- Worker count is unchanged (Vulkan: logical cores − 4, clamped to 4..16). Metal programs keep their concurrent GCD
  queue.

### 6. Errors and reset

- A failed translate or compile sets state 2. Draws using it are skipped, as sync mode skips a failed pipeline
  (`NSNull`) today. Pending pipeline requests on a failed program are dropped.
- **In-place reboot** (`backend_reset`): before it drops `prog_tab`, `pipe_tab` and the caches, it waits until the
  in-flight build count (an atomic, decremented on completion) reaches zero. The pending table is cleared with them.
  On Metal, `g_boot_gen` stays in the key.

## Logging and measurement

`[shader] prefetch:` line, once per 600 presented frames and at exit:
`scanned D draws / S submits (T ms), started P programs + Q pipelines ahead, waits W (M ms total, worst X ms),
unforeseen U`. `U` counts draws that had to start their own build although the scan had run, which measures the
scan's accuracy. `LARECOMP_PREFETCH_VERIFY=1`: after each submission executes, the key-relevant fields of the shadow
are compared with the live `st` and mismatches are logged (this checks `apply_state`).

## Testing and success criteria

Runs follow the agent GPU budget (one game instance per agent).

- **Bit-identical frames:** an `SMM2_SEED` boot to the title course, presented frames 1-2750, prefetch against sync.
  Run both cold (`LARECOMP_SHADER_SALT`, no `cache/`) and warm. Every frame must match exactly, and prefetch must
  skip no draws for unfinished builds (`stat_skipped` from unready objects = 0).
- **Stall reduction (macOS, M4 Pro, same scenario as smm2-bringup.md's table):** sum of game-thread compile waits,
  and the worst 2 s window. Target: at least 3× less than sync when cold, and no worse when warm [assumed target].
- **Scan accuracy:** `U` = 0 on the title course and on entering a course from the editor. `LARECOMP_PREFETCH_VERIFY`
  shows no mismatches over the same runs.
- **Scan cost:** at most 0.5 ms per presented frame in steady state on the iPad M5 [assumed budget]. If it costs
  more, add a per-list memo (skip a list whose last scan found nothing, keyed by handle and an entry-state hash). A
  stale memo hit only loses a prefetch.
- **iPad:** the same cold and warm runs, reporting the `prefetch` and `slow program` lines. This also provides the
  iPad numbers smm2-bringup.md is still missing.
- **Vulkan:** the bit-identical and stall runs on macOS through MoltenVK. CI's Linux build compiles the code paths.

## Interaction with existing systems

- **Frame generation** (2026-10-20 spec) works in the present path, not in `exec_list`, so the two don't conflict.
  Prefetch's lower stalls help it hold its 100 Hz floor.
- **The shader warm-up** (`shader_cache_warm`) and **precompiled metallibs** are unchanged: a precompiled library
  just makes the program job fast.
- **The pipeline archive saves** (`pipeline_cache_add_*` on `g_archive_q`) are called from the build jobs as today.

## Out of scope

- Moving Metal's shader translation off the game thread. It stays in `get_program`, as today. Measured 2026-10-20
  (M4 Pro, seed-4 title course + Make/Play menu, game-thread time per presented frame): translation is about
  0.14 ms per shader. The worst frame is the title course's load frame, with 160 translations in 22 ms, next to
  100 ms of compiles and cache lookups when warm, or 21.2 s of compiles when cold. Menu frames spend 0.1-0.3 ms on
  translation. Revisit only if, after prefetch, that one 22 ms load frame matters.
- The record-time speculative scan (approach 2) and the start-up rebuild from archive descriptions (approach 3).
- Ubershaders and skipping draws for prefetched work.
