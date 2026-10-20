# Shader Prefetch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Start all of a submission's first-use program and pipeline builds in parallel before its lists run, and
have each draw wait only for its own unfinished build. No draw is ever skipped, and frames stay bit-identical to
today's blocking mode.

**Architecture:** A shared pure-C module (`buildsync.c`) holds the build states, the completion wait, the in-flight
drain, the pending-pipeline table, a job queue with wait-steal, the mode switch and the stats. Each backend first
moves its executor's state updates and key computation into side-effect-free functions (`apply_state`,
`program_keys`, `pass_layout`, the pipeline key). It then gains `backend_prefetch`, which `nvnQueueSubmitCommands`
calls before its execute loop: it walks the lists on a shadow copy of the state and starts builds. Pipeline requests
wait on their program as continuations. Metal goes first; Vulkan follows the same shape.

**Tech Stack:** C11 / Objective-C (Metal, GCD), Vulkan 1.3 + shaderc, pthreads.

**Spec:** `docs/superpowers/specs/2026-10-20-shader-prefetch-design.md`

## Global Constraints

- **No skipped draws for unfinished builds in prefetch mode.** Frames must be bit-identical to sync mode.
- **The scan is advisory.** Drawing must never depend on the scan being right. A miss builds on demand and waits.
- **The fast path stays lock-free:** `prog_tab` / `pipe_tab` are read and written on the game thread only. Locks are
  taken only on a miss.
- Modes: prefetch is the default; `LARECOMP_NO_SHADER_PREFETCH=1` selects sync and `LARECOMP_ASYNC_SHADERS=1` skip
  (unchanged). Skip wins if both are set.
- Wait log: `[shader] waited N ms for …` once per object when a wait exceeds 2000 ms. There is no timeout.
- Stats line `[shader] prefetch: scanned D draws / S submits (T ms), started P programs + Q pipelines ahead, waits W
  (M ms total, worst X ms), unforeseen U`, every 600 presented frames and at exit.
- Vulkan workers: logical cores − 4, clamped to 4..16 (unchanged). Metal programs keep their concurrent GCD queue.
- Translation stays where it is on both backends (Metal: in `get_program`; Vulkan: its translator thread).
- New comments and logs follow the surrounding style: `// (...)` notes, `LOG("[shader] ...")`.
- Agents running the game: **at most one smm2recomp instance at a time**, short targeted runs, nothing left running.
- Commits are local. Push only in one batch at the end (CI minutes). The pre-commit gate is `scripts/check.sh`.

## Review Focus

1. **One-shot renders (the colour-correction LUT, light probes) on a cold start.** They must appear: the scene is not
   black. → Task 2 step 1 (`shader_ab.sh` cold, frames 150–600) and the same check in Tasks 3, 4 and 6.
2. **The in-place reboot while builds are in flight** (returning to the title from a course). No crash and no stale
   pipeline. → Task 3 step 5 (reboot during a cold run) and Task 6 step 4.
3. **A program whose translation or compile fails.** Its draws skip as in sync mode, pending pipeline requests on it
   are dropped, and nothing waits forever. → Task 1 `test_wait_failed_returns`, plus a failure injected in
   Task 4 step 5.
4. **Mirror mode on** (`mirror_mode = 1`). The scan doesn't predict the mirrored variant, so it builds on demand and
   frames still match sync. → Task 4 step 5.
5. **Lists submitted again unchanged every frame** (steady state). The scan must stay within its 0.5 ms per presented
   frame budget on the iPad. → Task 7 step 2.

---

### Task 1: `buildsync` — states, waits, pending table, job queue, stats

**Files:**
- Create: `recomp/runtime/nvn/buildsync.h`, `recomp/runtime/nvn/buildsync.c` (picked up by `RT_SRC`'s `runtime/nvn/*.c` wildcard)
- Create: `recomp/tools/test_buildsync.c`
- Modify: `recomp/Makefile` (targets `build/test_buildsync`, `test-buildsync`, add both to `.PHONY`), `scripts/check.sh` (run `make -s -C "$root/recomp" test-buildsync` next to `runtime-objs`, every platform)

**Interfaces:**
- Produces (in `buildsync.h`):
  ```c
  enum { SHADERS_PREFETCH, SHADERS_SYNC, SHADERS_SKIP };
  int shader_mode_from(const char *no_prefetch, const char *async);  // pure: the two env values (NULL = unset)
  int shader_mode(void);                                              // read once from the environment
  enum { BUILD_PENDING = 0, BUILD_READY = 1, BUILD_FAILED = 2 };
  void build_begin(void);                                      // one more build in flight
  void build_finish(_Atomic int *state, int result);           // store result, one fewer in flight, wake waiters
  int  build_wait(_Atomic int *state, const char *what);       // until not PENDING; returns the state; counts the wait
  void build_drain(void);                                      // until nothing is in flight
  void *pend_get_or_put(uint64_t hash, void *fresh);           // the existing entry, or fresh (inserted)
  void *pend_take(uint64_t hash);                              // remove and return; NULL if absent
  void pend_clear(void (*release)(void *));
  typedef struct JobQueue JobQueue;
  typedef struct BuildJob { void (*fn)(void *); void *arg; struct BuildJob *next; _Atomic int state; } BuildJob;
  enum { JOB_IDLE, JOB_QUEUED, JOB_RUNNING, JOB_DONE };
  JobQueue *jobq_create(int nthreads);                         // threads start at once
  void jobq_submit(JobQueue *q, BuildJob *j, void (*fn)(void *), void *arg);  // j owned by the caller (embedded)
  void job_run_or_wait(JobQueue *q, BuildJob *j);              // still queued: run it on this thread; else wait for DONE
  typedef struct { uint64_t draws, submits, scan_ns, progs, pipes, waits, unforeseen; double wait_ms, worst_ms; } PrefetchStats;
  extern PrefetchStats g_prefetch;                             // game-thread counters (waits under the build lock)
  void prefetch_stats_tick(uint64_t presented);                // logs the stats line every 600, then resets
  void prefetch_stats_log(void);                               // the same line now (also registered with atexit)
  ```
- One global mutex plus condvar serves `build_finish` / `build_wait` / `build_drain` and the `JOB_DONE` waits. The
  pending table is a separate mutex around an open-addressing table of 4096 entries.

- [ ] **Step 1: Write the failing tests** in `recomp/tools/test_buildsync.c`, a plain `main` running each function
  and printing `ok`/`FAIL name` (style of `tools/test_translate.c`), exit status 1 on any failure:
  - `test_mode`: `shader_mode_from(NULL, NULL) == SHADERS_PREFETCH`, `("1", NULL) == SHADERS_SYNC`,
    `(NULL, "1") == SHADERS_SKIP`, `("1", "1") == SHADERS_SKIP`.
  - `test_wait_after_finish`: thread finishes the state with `BUILD_READY` after 50 ms; `build_wait` returns
    `BUILD_READY`, `g_prefetch.waits == 1`, `g_prefetch.wait_ms >= 40`.
  - `test_wait_failed_returns`: `build_finish(&s, BUILD_FAILED)` from a thread; `build_wait` returns `BUILD_FAILED`.
  - `test_wait_ready_no_count`: a state already `BUILD_READY` returns at once, and `g_prefetch.waits` is unchanged.
  - `test_steal_queued`: `jobq_create(1)`, submit a gate job that blocks on a semaphore, then job B. Calling
    `job_run_or_wait(q, &B)` runs B on the calling thread (B records `pthread_self()` and it equals the caller's), and
    B's state is `JOB_DONE`. After that, release the gate.
  - `test_wait_running`: a job sleeping 50 ms already `JOB_RUNNING` on the worker; `job_run_or_wait` returns after it,
    its state is `JOB_DONE`, and it ran on the worker thread.
  - `test_fifo`: on 1 thread, 10 jobs record their order 0..9.
  - `test_drain`: 3 `build_begin`s finished by threads after 20/40/60 ms; `build_drain` returns after the last.
  - `test_pend`: `pend_get_or_put(7, a) == a`, then `pend_get_or_put(7, b) == a`, `pend_take(7) == a`,
    `pend_take(7) == NULL`. With 8 threads × 1000 distinct hashes, every hash maps to exactly one entry.
- [ ] **Step 2: Run it to make sure it fails.** `make -C recomp test-buildsync`. Expected: it does not compile
  (`buildsync.h` missing).
- [ ] **Step 3: Implement `buildsync.c`.** `build_wait` logs `[shader] waited N ms for <what>` once per `state`
  address past 2000 ms (a small set of logged addresses is enough). `job_run_or_wait` steals by a
  `JOB_QUEUED → JOB_RUNNING` compare-exchange: the worker loop does the same compare-exchange and skips the job if it
  loses, so the job runs exactly once. It unlinks the job from the queue under the queue lock. The `LOG` macro comes
  from the runtime header the other `nvn/*.c` files use; the test build defines `LOG` as `printf`.
- [ ] **Step 4: Run the tests.** `make -C recomp test-buildsync`. Expected: every line `ok`, exit 0. Run it 20 times in
  a loop: no `FAIL`, no hang.
- [ ] **Step 5: Commit.** `buildsync: build states, waits with steal, pending table, prefetch stats (shader prefetch 1/7)`

### Task 2: Metal — factor state updates and keys out of the executor (no behaviour change)

**Files:**
- Modify: `recomp/runtime/nvn/renderer.m` (`st` at line ~59, `begin_pass` ~487, `rt_dropped` ~462, `draw` ~2173, the pipeline-key block ~2300–2400, `exec_list` ~3088)
- Create: `recomp/tools/shader_ab.sh`

**Interfaces:**
- Produces (static, in `renderer.m`):
  ```objc
  typedef struct { ... } ExecState;              // the current anonymous struct, named; `static ExecState st;`
  static bool apply_state(ExecState *s, const CmdRec *r);  // pure state ops; false for any other op
  static void program_keys(const ExecState *s, ShaderKey *vkey, ShaderKey *fkey);
  typedef struct { MTLPixelFormat color[8], depth, stencil; uint8_t dropped; NSUInteger samples; } PassLayout;
  static void pass_layout(const ExecState *s, PassLayout *out);   // no textures created, no pass begun
  static MTLRenderPipelineDescriptor *render_pipe_desc(const ExecState *s, const PassLayout *pl, Prog *pr,
                                                       NSString **key);  // functions left nil; key names pr
  static uint64_t pipeline_state_hash(const ExecState *s, Prog *pr);  // was (Prog *pr), reading st
  ```
- `tools/shader_ab.sh <binary> <tag> [cold]`: copies the data folder into `$TMPDIR/shader_ab/<tag>` (cold: salted, and
  no precompiled, pipeline cache or warm-up, as in the measurement run of 2026-10-20). It runs seed 4 with
  `LARECOMP_INPUT="2500:L:20,2500:R:20,2800:RIGHT:8,2900:A:8,3300:DOWN:8,3400:A:8"`,
  `LARECOMP_SCREENSHOT_EVERY=150`, exiting after frame 4200 (`LARECOMP_WINDOW_SHOT=4200 LARECOMP_EXIT_AFTER_SHOT=1`),
  through `tools/run_for.sh 300`. It waits first while any other `smm2recomp` runs. It leaves
  `$TMPDIR/shader_ab/<tag>/build/frame_*.png` and `run.log`. `tools/shader_ab.sh --compare <tagA> <tagB>` runs `cmp`
  on every frame and prints `identical N/N` or the first differing frame.

- [ ] **Step 1: Baseline.** Build `main` as is, then run `shader_ab.sh … base` and `shader_ab.sh … base-cold cold`.
  Expected: 28 frames each, and frames 150–600 of `base-cold` show the lit scene, not black (look at
  `frame_00600.png`).
- [ ] **Step 2: Name the state type** `ExecState`, and move the pure-state cases of `exec_list` into `apply_state`
  (the list is in the spec, section 1). `exec_list` calls `apply_state` first and keeps its logging (`LARECOMP_LOG_RTS`,
  `g_debug_frame`, the video-texture warning) around `CMD_SET_RENDER_TARGETS`.
- [ ] **Step 3: Extract `program_keys`, `pass_layout` and `render_pipe_desc`.**
  - `pass_layout` takes `rt_dropped`'s tile-memory rule and maps formats without `view_texture`. It uses the view's
    format when `has_cview`, the same mapping `view_texture` uses.
  - `begin_pass` calls `pass_layout`, then does its side effects.
  - In `draw`, the slow-path key string swaps `pr->vs pr->fs` for `pr`, and `render_pipe_desc` sets
    `vertexFunction` / `fragmentFunction` to nil. The draw sets them from `pr` before building.
- [ ] **Step 4: Check that nothing changed.** In debug builds only, an `assert` in `begin_pass` checks that each
  attachment's `texture.pixelFormat` equals `pass_layout`'s. Then run `shader_ab.sh … t2` and
  `shader_ab.sh --compare base t2`, and the same cold. Expected: `identical 28/28` both times, and no assert.
- [ ] **Step 5: Commit.** `Metal: executor state updates and pipeline keys as pure functions (shader prefetch 2/7)`

### Task 3: Metal — modes, builds off the thread, wait instead of skip

**Files:**
- Modify: `recomp/runtime/nvn/renderer.m` (`Prog` ~710, `sync_shaders` / `compile_program` / `get_program` ~1285–1452, the pipeline build in `draw`, `renderer_reset` ~3395, `renderer_stats`)

**Interfaces:**
- Consumes: Task 1 (`shader_mode`, `build_*`, `pend_*`), Task 2 (`render_pipe_desc`, `ExecState`).
- Produces:
  - `Prog` gets `_Atomic int state` (replacing `ready`/`failed`), plus `NSMutableArray *waiting` (pipeline
    requests, guarded by `@synchronized(pr)`) for Task 4.
  - `typedef struct MPipe { _Atomic int state; id<MTLRenderPipelineState> pipe; } MPipe;`, the pending table's entry.
  - `static MPipe *pipe_request(Prog *pr, MTLRenderPipelineDescriptor *pd, NSString *key, uint64_t phash);`:
    looks up or creates the `MPipe` in the pending table. A new entry gets `build_begin()` and a GCD job that sets
    the functions from `pr`, tries `pipeline_cache_find_render`, else builds and calls `pipeline_cache_add_render`,
    then `build_finish`. It must be called only when `pr` is `BUILD_READY`.

- [ ] **Step 1: Modes.** `sync_shaders()` is deleted. In `get_program`: `SHADERS_SYNC` compiles inline as today; the
  other two modes dispatch `compile_program` to the concurrent queue with `build_begin` / `build_finish`.
- [ ] **Step 2: Draw path.**
  - `SHADERS_SKIP` keeps the skip at `!ready` exactly as today.
  - `SHADERS_PREFETCH` calls `build_wait(&pr->state, "program")`, and skips only on `BUILD_FAILED`.
  - Pipelines: on a `pipe_tab` miss in prefetch mode, `pipe_request` then `build_wait`; on ready, move the entry into
    `cache[pk]` and `pipe_tab`.
  - Skip mode keeps the existing `pending` / `ready_pipes` path.
- [ ] **Step 3: Reset.** `renderer_reset` calls `build_drain()` first and `pend_clear` with a release that drops the
  `MPipe`.
- [ ] **Step 4: Check equality.** Run `shader_ab.sh … t3` and `t3-cold cold`, then compare them with `base` /
  `base-cold`. Expected: `identical 28/28` both times, and the `skipped` count in the fps lines no higher than
  `base`'s at the same frames.
- [ ] **Step 5: Reboot while building.** Run cold with `LARECOMP_INPUT` adding a return to the title right after the
  first course load. Use the in-place reboot test hook (`runtime/main.c` `reboot_test_frame`):
  `LARECOMP_REBOOT_AT=1230`, inside the 21 s compile burst. Expected: no crash, and the second boot reaches
  frame 4200.
- [ ] **Step 6: Commit.** `Metal: first-use builds off the game thread, draws wait for them (shader prefetch 3/7)`

### Task 4: Metal — the submit-time scan and continuations

**Files:**
- Modify: `recomp/runtime/nvn/nvn.h` (declare `void backend_prefetch(const uint64_t *handles, int n);`), `recomp/runtime/nvn/objects.c` (`nvnQueueSubmitCommands`: call it before the execute loop), `recomp/runtime/nvn/renderer.m`, `recomp/runtime/nvn/metal.m` (call `prefetch_stats_tick(total_frames)` next to `save_editor_frame`)
- Modify: `recomp/runtime/nvn/vulkan/exec.c` (an empty `backend_prefetch` so Vulkan links; Task 6 fills it)

**Interfaces:**
- Consumes: Tasks 1–3.
- Produces: `backend_prefetch` (Metal). A pipeline request held on a `Prog` is
  `@interface PipeReq : NSObject { @public MTLRenderPipelineDescriptor *pd; NSString *key; uint64_t phash; }`,
  built from the shadow state at scan time (`render_pipe_desc` reads only the state it is given). `compile_program`'s
  end drains `pr->waiting` into `pipe_request` when the result is ready, and drops the list on failure.

- [ ] **Step 1: Write the verify check first.** With `LARECOMP_PREFETCH_VERIFY=1`, `backend_prefetch` keeps its final
  shadow state. After the submit loop, `objects.c` calls `renderer_prefetch_verify()`, which compares the shadow's
  `program`, `attrib`/`nattrib`, `stream`/`nstream`, `rt`, `blend`, `color_state`, `channel_mask`, `ms` and `poly`
  with `st`, and logs `[shader] prefetch verify: <field> differs`. With the scan still empty, run `shader_ab.sh … t4v`
  with it on. Expected: mismatch lines (the empty scan's shadow never changes).
- [ ] **Step 2: Implement the scan** per spec section 3.
  - It returns at once unless `shader_mode() == SHADERS_PREFETCH`.
  - It copies `st` into a static shadow and walks the lists (`CMD_JUMP`, `CMD_CALL` to depth 8, `CMD_END`, with the
    same bad-record guard as `exec_list`), applying `apply_state` to the shadow.
  - At a draw with a program bound and `poly[0] != 3`: `program_keys`, then `get_program`. If the program is ready,
    `pass_layout` + `render_pipe_desc` + `pipe_request`; if not, a `PipeReq` goes onto `pr->waiting`. Skip both when
    `pipe_tab` already holds `pipeline_state_hash(&shadow, pr)`.
  - At `CMD_DISPATCH`: the compute program, built through the same queue.
  - Counters: `g_prefetch.draws`, `.submits`, `.scan_ns`, and `.progs` / `.pipes` for each build started.
- [ ] **Step 3: Unforeseen count.** A build that a draw (not the scan) creates in prefetch mode increments
  `g_prefetch.unforeseen`.
- [ ] **Step 4: Run the checks.**
  - `shader_ab.sh … t4` and `t4-cold cold` against `base` / `base-cold`: `identical 28/28` both times.
  - The same with `LARECOMP_PREFETCH_VERIFY=1`: no `prefetch verify` lines.
  - Both logs' last `[shader] prefetch:` line shows `unforeseen 0`. If not, the missed draws' keys show which input
    `pass_layout` or the scan gets wrong; fix it and rerun.
- [ ] **Step 5: Edge runs.**
  - Mirror mode (`mirror_mode = 1` in the copy's `smm2.ini`): `t4-mirror` against a sync run with mirror on
    (`LARECOMP_NO_SHADER_PREFETCH=1`): identical.
  - A failure injected for one VS by env (`LARECOMP_DEBUG_FAIL_VS=<code addr hex>`, debug only, makes
    `compile_program` fail that program): the run reaches frame 4200, the log shows the failure once and no
    `waited` line.
- [ ] **Step 6: Measure (macOS).** Run sync (`LARECOMP_NO_SHADER_PREFETCH=1`) against prefetch, cold and warm, with the
  2026-10-20 `[shadertime]`-style numbers: sum of game-thread waits plus inline builds, worst frame, frames over
  16/50 ms. Expected: cold stall at least 3× lower than sync; warm no worse. Put the table in the commit message.
- [ ] **Step 7: Commit.** `Metal: scan each submission and start its builds in parallel (shader prefetch 4/7)`

### Task 5: Vulkan — factor out, buildsync jobs with steal, wait instead of skip

**Files:**
- Modify: `recomp/runtime/nvn/vulkan/exec.c` (jobs and translator ~598–680, `get_program` ~1453, `build_pipeline` ~1686, `begin_pass` ~1029, `draw_` ~2840–2980, `build_compute` ~2715, `exec_list` ~3887, the reset path)

**Interfaces:**
- Consumes: Task 1.
- Produces:
  - `ExecState`, `apply_state`, `program_keys`: the same contracts as Task 2.
  - `typedef struct { VkFormat color[8], depth, stencil; uint32_t ncolor, samples; } PassLayout;` and
    `static void pass_layout(const ExecState *s, PassLayout *out);`. `begin_pass` fills `pass_color` / `pass_depth` /
    `pass_stencil` / `pass_ncolor` / `pass_samples` from it.
  - `static void pipe_key(const ExecState *s, const PassLayout *pl, const Prog *pr, int topo_class, PipeKey *k);`
  - `Prog` and `Pipe` each embed a `BuildJob job`; `Prog` gets a `waiting` list (mutex-guarded array of
    `{PipeKey k; uint64_t hash;}`) for Task 6.
  - Two `JobQueue`s: the translator (1 thread) and the workers (count unchanged). They replace the hand-written
    `Job` queues.

- [ ] **Step 1: Baseline in the container.** Cross-build `TARGET=linux-aarch64 GAME=smm2 DEPS=…` from `main`. Run in
  a `debian:trixie` arm64 container with Mesa lavapipe and Xvfb (as in the frame-generation plan, Task 5 step 6),
  seed 4, `LARECOMP_WINDOW_SHOT=300:1500:300`, `LARECOMP_SHOT_PATH=build/win_%05d.png`. Expected: 5 frames saved.
  It runs at about 7 fps, so keep the run to these frames.
- [ ] **Step 2: Refactor** as in Task 2 (state, keys, pass layout), and replace the `Job` queues with `JobQueue`.
  Remove `sync_shaders()`: sync mode runs translate/compile/build inline, the others submit. Fix the stale
  `LARECOMP_SYNC_SHADERS` comment.
- [ ] **Step 3: Wait.** In prefetch mode, a draw whose `Prog` or `Pipe` is unfinished calls `job_run_or_wait` on its
  job: the translate job, then the compile job, then the pipeline job. It then reads the state and skips only on
  failure. Skip mode keeps `SKIP_DRAW()`. Reset calls `build_drain()` and `pend_clear` before dropping the tables.
- [ ] **Step 4: Check.** The same container run in prefetch and in sync mode: all 5 frames byte-identical to the
  baseline, in both modes. Also `make TARGET=android-arm64 GAME=smm2 … runtime-objs` and
  `TARGET=windows-x86_64 … runtime-objs` build.
- [ ] **Step 5: Commit.** `Vulkan: executor keys as pure functions; builds on buildsync queues, draws wait with steal (shader prefetch 5/7)`

### Task 6: Vulkan — the scan and continuations

**Files:**
- Modify: `recomp/runtime/nvn/vulkan/exec.c`

**Interfaces:**
- Consumes: Tasks 1, 4 (the `backend_prefetch` declaration and the `renderer_prefetch_verify` call; on Vulkan it is
  named the same and lives in `exec.c`), 5.
- Produces: `backend_prefetch` (Vulkan). On a program not yet ready, the scan can't compute `pipe_key`, because the
  key reads `pr->fi->out_mask`. So the `waiting` entry holds a copy of the shadow `ExecState` + `PassLayout` +
  `topo_class` (heap-allocated, freed on drain), and the key is computed when `compile_program` finishes.
  Completed keys go into the pending table (`pend_get_or_put`) and are picked up by a draw on a `pipe_tab` miss, as on
  Metal.

- [ ] **Step 1: Implement** the scan per Task 4 step 2, with the Vulkan key functions (`pipe_tab` check: the
  `hash_words` of `pipe_key`'s result). Metal's verify and unforeseen hooks get their Vulkan counterparts at the same
  points, and `prefetch_stats_tick` is called once per presented frame from Vulkan's present path.
- [ ] **Step 2: Container run.** Prefetch with `LARECOMP_PREFETCH_VERIFY=1`: 5 frames identical to Task 5's
  baseline, no `verify` lines, `unforeseen 0`, and no Vulkan validation errors (`VK_LAYER_KHRONOS_validation` if the
  image has it).
- [ ] **Step 3: Cross-builds.** `linux-x86_64` (`generated/smm2-c`), `android-arm64` and `windows-x86_64`
  `runtime-objs` build.
- [ ] **Step 4: Reboot while building:** the same as Task 3 step 5, in the container. Expected: no crash.
- [ ] **Step 5: Commit.** `Vulkan: scan each submission and start its builds in parallel (shader prefetch 6/7)`

### Task 7: iPad numbers and project records

**Files:**
- Modify: `recomp/README.md` (env table: `LARECOMP_NO_SHADER_PREFETCH`, `LARECOMP_PREFETCH_VERIFY`, the
  `LARECOMP_ASYNC_SHADERS` row now naming prefetch as the default), `recomp/docs/smm2-bringup.md` (a section
  "Shader prefetch (2026-10-…)" with the measurement tables), `DECISIONS.md` (entry: prefetch default, no skipped
  draws kept; the R1 entry's "first-use compiles block" now reads "draws wait, builds are parallel"), `PROGRESS.md`

- [ ] **Step 1: iPad build and install.** Build the iOS app (`make -C recomp TARGET=ios-arm64 GAME=smm2 DEPS=… app`)
  and install it over Tailscale with `~/.claude/tools/ios-install-tailscale/ios-install-tailscale <app> --device ipad`.
  Run a sync launch and a prefetch launch (`recomp.env` in the app's Documents: `SMM2_SEED=4`, and
  `LARECOMP_NO_SHADER_PREFETCH=1` for sync), each to the title course and its first 2 minutes. Fetch both logs with
  the ios-app tooling.
- [ ] **Step 2: Read the numbers.** The `[shader] prefetch:` lines (scan cost per frame from `T` / frames), `slow
  program`, and the `pipeline cache` lines. Expected: steady-state scan ≤ 0.5 ms per presented frame. If it is
  over, write down the per-list memo (spec, "Scan cost") as the next task rather than doing it here.
- [ ] **Step 3: Records.** Write the README, smm2-bringup, DECISIONS and PROGRESS entries with the measured macOS,
  container and iPad numbers.
- [ ] **Step 4: Commit,** then push the whole batch once. `Shader prefetch: docs, measurements, decision record (shader prefetch 7/7)`
