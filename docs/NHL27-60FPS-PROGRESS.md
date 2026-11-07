# NHL 27 PC — 60 FPS progress handoff

Last updated: 2026-11-07 (Europe/Berlin)

## Session 2026-11-07, part 12: per-draw cost in the draw-heavy intro

The user chose "per-draw cost work" after part 11 (goal: the 14–17 FPS intro/lineup at 5–7k draws
per frame).

- **Where a draw's ~6.6 µs went (heavy intro seconds, `pmiss-2` counters):** shader-resource
  walk/materialization ~2.7 µs, binding resolve (`PrepareBindings`: texture/sampler resolves) ~0.85,
  buffer rebinding/sync ~0.6, render/depth target lookup ~0.8, commit ~0.45. **GPU-thread
  sampler in the same scene (`dc-prof-1`, `-Sample '78,26'`):** JIT walker 17.5%, waits 14%,
  `ObtainBuffer` 7.5% (`SynchronizeBuffer` 5.0%), `FindImage` 6.7% (`FindImagesInRegion` 3.9%),
  `ResolveTexture` 6.2%, `NtProtectVirtualMemory` 3.8%, `RtlQueryPerformanceCounter` 2.9%.
- **Fix 1: the per-PM4-packet opcode timer (`graphicsRun.cpp`) used `steady_clock`**
  (QueryPerformanceCounter) twice per packet, ~1.3 M packets/s. Now `FastClock` (rdtsc), like the
  other stage timers.
- **Fix 2: texture-cache lookup memos (on, `KYTY_TEXTURE_MEMO=0` disables, follows `KYTY_AB_PERIOD`).**
  - `TextureCache` logs every registration, unregistration and stencil re-association (address
    range + generation, 256-entry ring). `FindImage` reports whether its result was a *direct
    match* (`SameBacking`, no overlap resolution, no block-alias candidate, no stencil association,
    no new image). `ReuseFoundImage(id, generation, desc, …)` accepts a memoized result only if no
    logged change overlaps the looked-up range and the image still holds the same range, then
    repeats `FindImage`'s side effects (access tick, LRU touch, `MaterializeColorClear`).
  - `ResolveTexture` memo: 1,024 direct-mapped entries keyed by the T# words plus every
    image-resource flag the resolve reads (heap resources excluded). The render-target memo now
    also reuses its image, and the depth target gets the same memo (description keyed by the raw
    depth registers).
  - `KYTY_TEXTURE_MEMO_VERIFY=1` runs the full lookup beside every hit: `tmv-1` 13.0 M texture
    hits, `tmv-2` 14.3 M texture + 15.0 M render/depth hits, **0 mismatches**. Hit rates 67%
    (textures) and 98% (targets). Rendering correct.
  - `abm-1` in-run A/B: render/depth target resolve 0.92 → 0.37 µs per draw, binding resolve
    2.49 → 1.40. The A/B's "off" phase also turned off the older `KYTY_RT_MEMO` (same A/B flag),
    so its −23% per draw overstates the gain; the before/after table below is the honest number.
- **Fix 3: cheaper reachability marks in SRT walkers (on, `KYTY_WALKER_FAST_MARKS=0` disables).**
  The walker is a lazily evaluated DAG; its block-reachability pass marked every successor
  with compare-branch-store on scratch flag bytes. A forward successor that no control Phi
  rechecks is now one store (setting a set flag is a no-op; only rechecks and retreating edges
  need the test). The verify-mode reference walker keeps the old form: `wfm-v1`
  (`KYTY_WALKER_VERIFY=1`) **9.4 M walks, 0 mismatches**. Intro walker statistics
  (`KYTY_WALK_STATS=96,66`, `wst12-2` → `wst12-3`): the big block walkers are 8–25% faster
  (VS `0xe352…` 2.23 → 1.97 µs, PS `0x860c…` 12.0 → 9.0 µs), time-weighted −8%.
- **Fix 4: nested walker thunks (on, `KYTY_WALKER_NEST=0` disables).** A value whose only use is
  one argument fetch in another thunk's body (consumer not a Phi, LogicalAnd/Or or folded value)
  is emitted inline at that fetch: slot, but no state byte, call or return. The consumer runs at
  most once per walk, so reads, captures and their order are the lazy walker's. `wnest-v1`
  (`KYTY_WALKER_VERIFY=1`): **10.5 M walks, 0 mismatches**. Small: in the big vertex walkers
  almost every thunk is a constant-buffer read shared by several block conditions (VS
  `0xe352…`: 128 blocks, nothing nested); compute `0xf7ac…` 6.7 → 4.1 µs.
- **Fix 5: partial walker prefetch (on, `KYTY_WALKER_PREFETCH_PARTIAL=0` restores none).** Walkers
  needing more than 32 prefetches got none; they now get their first 32. Big pixel walkers'
  cold cost fell (PS `0xb8fa…` 4.37 → 3.65 µs) with warm cost unchanged.
- **Walker result, same build, all three walker changes off vs on (`wof-1` vs `p12-2`):** walk per
  program lookup menus 2,140 → 1,975 ns, intro 1,208 → 1,128, early play 1,396 → 1,249
  (−7 to −11%, ~0.2 µs per draw). The walker-statistics runs suggested −14% (`wst12-2` →
  `wst12-6`); trust the same-build comparison.
- **Result, draw-heavy seconds (> 4,500 draws per frame, tool `tools/perf/intro.py`):**

  | | Previous build (`pmiss-2`, `pcs-2`, `dc-prof-1`, `dflt-3`) | This build (`gpt-1`, `p12-1`, `p12-2`, `wof-1`) |
  |---|---|---|
  | Draw cost (`DrawProcessNs` per draw) | 6.59–6.72 µs | 6.02–6.44 µs (**−7%**) |
  | GPU-thread time per draw of a frame | 9.8–10.1 µs | 9.2–9.9 µs (−5%) |
  | FPS | 14.4–15.3 | 14.0–17.4 |

  - **Intro FPS is not a reliable metric between runs:** the same build gives 17.8–23.9 FPS over
    t 70–106, depending on how the skip taps land (draws per frame there vary 3,300–4,500 between
    runs). An earlier draft of this section claimed "20 → 23.5 FPS" from two lucky runs; that was
    wrong. Compare per-draw cost, or use in-run A/Bs.
  - The `abm-1` A/B (memo "off" also disabling the older render-target memo) and the counters
    agree that target resolve and texture resolve got much cheaper; most of the rest of a draw
    (walker, buffer sync, commit) did not change.
- **The heavy intro is still GPU-thread bound, not GPU bound:** `gpt-1` (`KYTY_GPU_TIMING=1`) shows
  the GPU busy only ~42% there (26 ms per frame at 16 FPS) while PM4 processing stays ~880 ms/s.
- **The walker is compute bound now:** cache-hot repeats take 62% of walk time (3.3 of 5.3 s in
  the intro window). Its code is ~45% flag checks (`cmpb`/`jne` on node states and reachability
  bytes) plus a call/ret per node thunk.
- **Device losses:** 2 in 17 runs with these changes (`wst12-1` at the start-game load, `wst12-4`
  just after the arena; both the long-standing `0x0ab3a7c64ccce901` / `streamBuffer.cpp:183` /
  2348×1320 re-detile signature): within the known ~1-in-5 rate, nothing new.
- New: `KYTY_WALK_STATS=<report s>,<start s>` (window), `tools/perf/intro.py`, counters
  `TextureMemoHits/Misses/Mismatches`, `TargetMemoHits/Misses`.
- **Why an eager walker rewrite was not done:** evaluating values the lazy walker would skip
  (unchosen Select arms, reads in unreached blocks, words after an unknown descriptor word) adds
  guest reads and captured addresses, which feed the specialization-read overlap checks. An
  eager scheme needs a proof of which reads every walk executes, and the verify mode would have
  to compare captures as sets. The large vertex walkers are dominated by their reachability
  pass over many blocks whose conditions share constant-buffer reads.
- **Next steps, in order:**
  1. Walker, if continued: evaluate shared leaf reads (user data + constant offset) that the
     first block's condition or an unconditional root needs once in the prologue into
     registers or slots without state checks, and give block conditions of acyclic CFGs without
     restarts plain (non-memoized) code. Expect ~10–20% of walker time, not a halving; the walker
     is ~2 µs per draw.
  2. `ObtainBuffer`/`SynchronizeBuffer` per binding (~0.6 µs per draw): skip the tracker walk when
     a cheap "nothing CPU-dirty in range" check says so; avoid the per-call `std::vector` of copies.
  3. Hot write-fault pages (`NtProtectVirtualMemory` 3.8% on the GPU thread plus game-thread
     faults); part 10's caveats apply.
  4. Part 11's loading-screen items stay open.

## Session 2026-11-07, part 11: on-ice "regression", menu-to-game transition

The user also asked for a steady 30 during the menu-to-game transition ("breaks down quite a lot").

- **Part 10's slower end-of-session build is not a code regression.** New counters (`SideWaitSubmitted*`,
  `PresentWaitSubmitted*`, `PresentWaitTicksBehind`, `PresentCalls/PresentNs`, `BdaSliceSyncs/Ns`) and
  `KYTY_PRESENT_WAIT_SUBMITTED=0|ab` (skip the presenter's wait; diagnostic only). `pwab-1` (in-run A/B
  of the presenter wait, slice sync held on with the new `KYTY_BDA_SLICE_SYNC=fixed`): the presenter
  wait **never fires** on ice (0/s), side-queue waits cost 0.9 ms/s, and on-ice play ran **31 flips/s**
  in both phases, faster than both `asst` (29.5) and `sl30` (27.8). Treat `sl30` as batch variance.
  - Correction to part 10: slice sync costs `BdaSliceSyncNs` ≈ 43 ms/s (1,650 slices/s), which sits
    outside `Pm4ProcessNs`, so `bdas-1`'s "+11% draws per GPU-thread ms" is overstated. The net saving
    is ~33 ms/s (`BindBdaNs` 76 → 3 minus 43). Still a win; it stays on.
- **Transition timeline (tools `timeline.py`, `phasecost.py`, `cmpgroups.py`).** Perf time, warm cache:
  start-game screen ~t 38, loading screen ~t 40–44, loading scene t 45–57 (~5,000 draws/frame,
  21–25 FPS), arena load t 58–66 (~2,500 draws, 23–28 FPS, 6–9 GB/s texture uploads), intro after the
  skip taps t 72–96 (5,000–6,800 draws, **14–17 FPS**), lineup to ~t 104, then on-ice 30.
- **Fixed: ~1 s hitches from the Vulkan driver pipeline-cache save.** `PipelineCache::Save` ran on the
  GPU thread whenever any new pipeline appeared since the last save (every 30 s at most):
  `vkGetPipelineCacheData` of ~204 MB, XXH3, write and flush. Three per warm transition (`dflt-3`:
  `DrawPipelineNs` 1,012 ms at t 40 and 1,282 ms at t 89, a 698 ms dispatch at t 67), shown as 0–14
  FPS seconds. `PipelineCreateNs` never saw it (it times `CreatePipelineInternal` only). Periodic saves
  now run on a background thread (`m_driver_cache_saver`; `VkPipelineCache` is internally synchronized,
  and warm-up already creates pipelines from 8 threads); `Save()` (shutdown) joins it and saves
  synchronously; the dirty flag is cleared before the data is read. Counters `DriverCacheSaves/DriverCacheSaveNs`.
  Warm `pcs-2`, `pmiss-2`: worst `DrawPipelineNs` second 6 ms; saves take ~620 ms on their thread.
  Rendering correct.
- **Diagnostics added:** `ProgramMisses/ProgramMissNs` and a `Program miss: <stage> <hash>
  new-program|new-variant <ms>` line for misses ≥ 20 ms (`KYTY_LOG_PROGRAM_MISSES=0` silences). Warm
  `pmiss-2`: 38 misses, 259 ms total, the worst a 119 ms pixel shader. Not a hitch source when warm.
  `KYTY_WAIT_STACKS_MS=<ms>` prints the stack of every GPU-thread semaphore wait longer than that
  (first 80).
- **Loading screen (~3 s at 5–20 FPS, t ~40–44) is not fixed.** GPU-thread waits on the GPU
  (`GpuThreadWaitNs`) reach 250–430 ms/s against ~20 on ice; ~1,100 image creates/s; 800+ CPU-side
  texture uploads/s; guest faults 700 ms/s. `wst-2` wait stacks: most long waits are
  `RenderContext::UnmapMemory` → `CommandScheduler::Finish`. The game unmaps ~70 ranges/s while
  loading, and any cached buffer or image in the range (or any pending priority operation) drains the
  whole GPU.
  - Tried, opt-in: `KYTY_UNMAP_DRAIN=priority|ab`. It flushes, waits only until the asynchronous
    recorder has *submitted* (recording may still read the range), and waits for priority operations
    only if some are pending. Image/heap deletions are already deferred to tick completion. Load window
    `UnmapDrainNs` 762 → 381–601 ms, but flips there barely changed, and 1 of 4 runs crashed: the
    first version (no recorder wait) in `ud-prio-1`; `ud2-prio-2` died 2 s after boot in guest code,
    probably unrelated. Not default.
- **Crash class seen today:** `ntdll+0x20191`/`+0x1ff2a` near-null reads on a thread running on a
  guest stack (`rsp` 0xffa9…). In `dflt-1`/`wst-1` it follows the game's own "GPU Stopped Working"
  breadcrumb report (the game's GPU watchdog after long stalls). It looks like ntdll's exception
  dispatch walking a guest stack after a fault the emulator's handler did not claim; the original fault
  is lost. The thread sampler now limits *all* walks to the emulator image (it used to look up JIT
  addresses for the GPU thread; `gsamp-1` was the same pc from the sampler).
- **Next steps, in order:**
  1. The draw-heavy intro/lineup (25 s at 14–17 FPS, 5,000–6,800 draws per frame at the same
     ~7 µs/draw as on ice) is the main remaining transition gap. Steady 30 there needs per-draw cost
     roughly halved: the same work as the on-ice whistle/faceoff bursts (part 9: draw-preparation
     pipeline or GPU-side descriptors).
  2. Loading screen: find the rest of the GPU-thread GPU waits (`KYTY_WAIT_STACKS_MS=15`, warm
     run), and why 800+ CPU texture uploads/s happen. Before narrowing unmap drains further, capture
     the original fault behind the ntdll crash (log first-chance faults the handler declines on
     guest-stack threads).
  3. Part 10's steady-30 on-ice items stay open (eager publish of re-read GPU ranges, hot write-fault
     pages, image reuse).

## Session 2026-11-07, part 10: toward a steady 30

The user chose "steady 30 first" after part 9.

- **Where gameplay is below 30 (`asst-1..5`, per-second flips, tool `tools/perf/slowfast.py`):**
  - The first ~25 s after `-SkipIntro 8` are still the **pre-game lineup presentation** (player
    cards, arena lights; screenshot `gprof-slow/gameplay+15`) at 13–19 FPS with ~5,500–6,000
    draws per frame. Not on-ice play; exclude it (process time < ~115 s) from gameplay numbers.
  - On-ice play only (t 115–210, 475 s): mean 29.5, median 30, 25% of seconds below 28, 9% below
    25. The dips come in 4–6 s bursts (one or two per run) at 3,500–5,000 draws per frame,
    probably whistle/faceoff cameras. Per-draw cost is unchanged there; the draw count rises.
  - Pipeline compiles (up to 1.6 s in one second) only on the first runs after a rebuild.
- **Game-thread sampling (new `KYTY_SAMPLE_GUEST=<name>,…`, same window as `KYTY_SAMPLE_GPU`):**
  `RenderLoop` 96% and `GameSimulationLoop` 99.9% waiting on semaphores fed by the `Job0..12`
  workers; `GfxEopEventLoop` 94% waiting on EOP events. (`HalPs5 submit` is audio output.)
  Queue submits are already fine-grained (~52 per frame). The GPU thread's ~9% idle is coupling:
  job threads slowed by write faults and readbacks. No single cheap fix found there.
  - Sampler changes: guest threads are unwound only inside the emulator image (ntdll's lookup of
    code outside any module crashed the sampler, `gsamp-1`); reports append to
    `thread-samples.txt` and are serialized (dbghelp is not thread-safe, `gsamp-3` crashed);
    addresses outside any module are grouped as `[guest/jit code]`. Thread names come from the
    game (`Pthread run begin:` lines in the guest log).
- **Async-recording deadlock found and fixed (`gsamp-2`).** Hang during the load: the presenter
  sat in `NtDxgkSubmitPresentToHwQueue` holding an NVIDIA driver lock, and the GPU thread (side-queue
  readback submit) and the recorder worker (its `vkQueueSubmit`) both waited for that lock.
  Cause: with async recording a tick "submitted" by the GPU thread is only handed to the worker,
  so other queues' submits (side readback, presenter blit) could wait on timeline values not yet
  submitted (wait-before-signal), which this driver blocks inside its lock.
  - Fix: `MasterSemaphore::SubmittedTick/MarkSubmitted/WaitSubmitted` (the worker and the
    immediate path mark ticks after `vkQueueSubmit` returns). `DownloadOnSideQueue` waits for its
    tick to be submitted first; the presenter does too when its render tick was already handed
    over (a tick still being recorded keeps the old behavior, since the GPU thread may wait for
    the presenter).
- **BDA sync per submission slice (on, `KYTY_BDA_SLICE_SYNC=0` disables, follows
  `KYTY_AB_PERIOD`).** Before every DMA draw `PrepareBda` synced every buffer range the CPU had
  written since the previous draw (58k write faults/s feed it): `BindBdaNs` ~79 ms/s. CPU-write
  ranges (write faults, `ReadMemory` write path, new mappings) now go to a second queue
  (`QueueBdaSyncCpu`) that is drained once per guest submission slice (`PrepareBdaForSubmission`,
  from `GuestGpu::Process`). Per draw only newly registered buffers are synced. The guest can only
  rely on CPU writes made before it submitted, or before a label the command buffer waits on,
  which ends a slice.
  - `bdas-1` in-run A/B: `BindBdaNs` 79.0 → 2.9 ms/s, `DrawFindBuffersNs` 201 → 134 ms/s, draws/s
    62.2k → 66.1k with the GPU thread less busy (905 → 868 ms/s): ~+11% draws per GPU-thread ms.
    Screenshots in both phases correct.
- **Open: the end-of-session build measured slower, cause not found.** On-ice play only
  (process time 115–210 s):

  | Batch | Build | Mean | Median | p10 | Seconds < 25 | Device losses |
  |---|---|---|---|---|---|---|
  | `asst-1..5` | morning (async default) | 29.5 | 30 | 25 | 9% | 0/5 |
  | `sl30-1..5` | + submit-ordering fix, slice sync, diagnostics | 27.8 | 29 | 19 | 22% | 1/5 |
  | `nosl-1` | same, `KYTY_BDA_SLICE_SYNC=0` | 26.2 | 27 | 21 | 18% | 0/1 |

  - Slice sync is not the cause (`nosl-1` is as slow). In `sl30` the GPU thread spends *less*
    time in PM4 processing (849 vs 909 ms/s) with cheaper draws (7.9 vs 8.4 µs), so it is idle
    or busy somewhere uncounted more often. Suspects: the new `WaitSubmitted` waits (presenter /
    side readbacks now wait for the worker's `vkQueueSubmit`), batch-to-batch variance (late-night
    runs), or the slice-start BDA sync, whose cost is outside `Pm4ProcessNs`.
  - `sl30-5` lost the device with the long-standing signature (dispatch `0x0ab3a7c64ccce901`,
    `streamBuffer.cpp:183` copies, 2348×1320 re-detiles in flight): the known ~1-in-5 class,
    not new.
- **Next steps, in order:**
  1. Find the regression: add timers around `PrepareBdaForSubmission` (e.g. `BdaSliceSyncNs`) and
     `WaitSubmitted` (count + ns, per caller), then run the current build and an `asst`-equivalent
     build (git stash of today's later changes) back to back, 3 runs each. If `WaitSubmitted`
     costs, wait only when the driver would really block (the hang needs the presenter or side
     queue to submit a wait on an unsubmitted tick), e.g. have the worker do the side submit.
  2. Keep slice sync if it is neutral or better once (1) is resolved (its A/B was a clear
     per-draw win).
  3. Then the steady-30 items: early copy-back of re-read GPU ranges (about half the readbacks
     have a finished producer; needs the in-flight-copy coherence rules worked out in this
     session: CPU reads/writes during the copy must wait for its publish, GC must skip pending
     ranges), image reuse (~2%), and hot write-fault pages (careful: per-slice re-dirtying would
     re-upload ring buffers bound in many slices; 64 KiB fault coalescing was reverted before for
     texture re-uploads).
  4. The draw-heavy whistle/faceoff bursts (3,500–5,000 draws per frame) stay below 30 until
     per-draw cost roughly halves (part 9: pipeline/GPU-side descriptors).
- New this session, all opt-in or diagnostic: `KYTY_SKIP_DRAWS`, `KYTY_SAMPLE_GUEST`,
  `KYTY_IMAGE_SUBALLOC`, `tools/perf/slowfast.py`, thread descriptions for guest threads and the
  GPU thread (the `threads.ps1` name column still comes back empty). Defaults changed: async
  recording on (part 8), BDA slice sync on, submit-ordering fix (always on).

## Session 2026-11-07, part 9: is 60 FPS reachable? (feasibility measurement)

- **GPU-thread budget per frame (async default, `asst-3`, 28 flips/s, ~2,680 draws and ~1,030
  dispatches per frame, ~32 ms of GPU-thread time per frame):** shader-resource materialization
  (walk + decode, reads only guest memory) 264 ms/s = 9.4 ms/frame; buffer lookup/sync/bind
  (`FindBuffers`) 202 = 7.2; dispatch processing 158 = 5.6; other PM4 (decode, registers,
  readbacks, waits) ~148 = 5.3; render targets, images, commit, record ~120 = 4.3. A two-thread
  pipeline that moves only the pure part (materialization) caps at ~1.4× (~39 FPS).
- **New diagnostic `KYTY_SKIP_DRAWS=<percent>[,<delay s>]`** (`renderDraw.cpp`
  `SkipDrawForDiagnostics`): drops that share of graphics draws (every Nth of 100) before any host
  work, starting `<delay>` s after the first draw so the menus still render for the autopilot.
  Counter `SkippedDraws`; the autopilot counts it toward its draws-per-flip gameplay test.
  Rendering is garbage by design (the user saw it: noise, then nothing at 100%).
- **Result (`gskip-0/50/100`, delay 85 s, averages over gameplay after the skip starts):**

  | Draws skipped | Flips/s | GPU thread (`Pm4ProcessNs`) | Dispatch | Game-thread faults |
  |---|---|---|---|---|
  | 0% | 25.2 | 914 ms/s | 150 ms/s | 216 ms/s |
  | 50% | 36.1 | 884 ms/s | 204 ms/s | 333 ms/s |
  | 100% | **52.0** | 858 ms/s | 270 ms/s | 404 ms/s |

  - FPS scales with draw cost, and the **game's own threads run at ≥52 FPS**: per-thread CPU
    (`threads.ps1`) shows no saturated game thread (busiest non-GPU thread 31% of a core at
    25 FPS, 42% at 52 FPS; process ~3.7 of 16 hardware threads).
  - Even with zero draws the GPU thread is still saturated at 52 FPS: dispatches 270 ms/s,
    GPU-thread readbacks (`SideReadbackNs` 108 + `GpuThreadReadbackNs` 62), GPU waits 186 ms/s,
    materialization for dispatches 114 ms/s. Game threads stall 283 ms/s in readbacks
    (`BufferReadSyncNs`) of GPU-written data.
- **Verdict:** 60 is not ruled out by the game itself, but it needs more than the draw path. At 60
  the GPU thread has 16.7 ms per frame; today draws cost ~22 ms/frame and the non-draw work
  (dispatches, readbacks, PM4) ~10 ms/frame, which scales to ~16 ms/frame at 52 FPS by itself.
  The GPU is also at its limit (16.5 ms/frame). So 60 needs draws to drop to a few ms per frame
  (GPU-side descriptors and/or parallel draw preparation), plus dispatch and readback costs roughly
  halved, plus some GPU trimming. Each is a multi-week item; together a multi-month effort with
  real risk. A steady 30 is near (25–28 today), and ~40 looks reachable with a draw-preparation
  pipeline.
- `threads.ps1` now asks for thread descriptions (guest threads are named `guest:<name>` and the
  GPU thread `emu:Thread_Gpu` via `SetThreadDescription`), but the name column came back empty in
  these runs. Not yet debugged.

## Session 2026-11-06/07, part 8: async recording is the default, blocked-time map

The user chose this route over more host-mirror debugging (part 7).

- **Async Vulkan recording is now on by default** (`KYTY_ASYNC_RECORD=0` restores immediate
  recording). Same build, 150 s of on-ice play per run, `hang-repro.ps1`:

  | Mode | Runs | Device losses / hangs | Flips/s per run | Mean |
  |---|---|---|---|---|
  | async (`asst-1..5`) | 5 | 0 / 0 | 27.6, 27.0, 28.0, 25.9, 28.0 | **27.3** |
  | immediate (`imst-1..5`) | 5 | 0 / 0 | 24.5, 25.7, 26.0, 25.2, 24.8 | **25.2** |

  Async also does ~8% more draws/s (72.9k vs 67.4k). Default re-checked with no `KYTY_*`
  variables (`adef-1`): recorder active, 25.9 flips/s with the sampler on, gameplay screenshots
  correct, no loss. (Flips/s here is `FlipsRequested` over gameplay minus the first 15 s; it
  reads higher than the ~21 FPS of earlier sections, so compare within this table only.)
- **Where the GPU thread waits now (async, `asyb-1`, `KYTY_WAIT_STACKS=90`):** ~21% of samples
  blocked:
  - ~8.8% idle in `GuestGpu::ThreadRun` (no work from the game) and 3.7% in `WaitFlipDone`, so
    with async recording the game's own threads and flip pacing start to show.
  - ~8% side-queue readbacks (`DownloadOnSideQueue`), mostly walker loads faulting on GPU-written
    pages (`HandleFault` → `ReadMemory`), plus some from `DrawIndex`. Per second: 36 read
    readbacks (19 with the producer already **complete**, 9 in flight, 8 unknown), 28 write-fault
    downloads (all complete), 100 side readbacks costing 76 ms (~0.76 ms each, ~230 KB each from
    the widened coalescing window; 23 MB/s read + 44 MB/s write-fault downloads). Most cost is
    the fixed submit + fence round trip, not waiting for unfinished GPU work.
  - Image write-back drains are small now (7/s `TextureWriteFaultDownloads`).
- **Other GPU-thread costs seen:** re-protection after uploads (`NtProtectVirtualMemory` 3.7%,
  68k protect calls/s) as the other half of **58k guest write faults/s (233 ms/s, on the game
  threads)**: game writes a tracked buffer page, the GPU thread uploads it and re-protects it, and
  the game faults again next frame. Image create/destroy churn: ~110–120 images/s,
  create + delete ~18 ms/s.
- **Tried: image suballocation (`KYTY_IMAGE_SUBALLOC=1`, opt-in, follows `KYTY_AB_PERIOD`).**
  NVIDIA reports `prefersDedicatedAllocation` for render targets, so VMA gave each its own
  `VkDeviceMemory`. With the hint ignored (requirements queried without the dedicated chain,
  `vmaAllocateMemory` + `vmaBindImageMemory`): `isub-1` in-run A/B, create+delete 18.8 → 16.8 ms/s,
  rendering correct. The `vkCreateImage`/`vkDestroyImage` calls dominate, not the memory. Gain
  within noise, so it stays opt-in. New counters `ImageCreates/ImageCreateNs/ImageDeletes/
  ImageDeleteNs`.
- **Next candidates, by expected gain:**
  1. Readbacks whose producer is already complete (~half of them): publish GPU-written ranges
     eagerly for ranges the CPU has read back before. At the end of the producing command buffer,
     copy them into the download ring, then `WriteBacking` plus clear GPU-modified in a priority
     op once the tick completes, so the later CPU read finds clean memory and no fault. Up to
     ~7% of the GPU thread.
  2. Hot write-fault pages: pages the game rewrites every frame could stay unprotected and be
     uploaded on every use (treated as always CPU-dirty) after N consecutive-frame faults, and
     demoted again when the GPU writes them. Removes fault cost on the game threads and
     re-protection on the GPU thread (~4%).
  3. Image churn (~2%): recycle host images by create info instead of destroying them.

## Session 2026-11-06, part 7: host-mirror crash hunt

- **Bug found and fixed: `ApplyMirror` released imports mid-draw.** With only
  `KYTY_GPU_HOST_MIRROR=1` (no `KYTY_GPU_MIRROR`), `ApplyMirror` runs from `PrepareBda()` inside
  draw/dispatch preparation (after `FindBuffers`, inside an open upload batch). The
  release-before-reimport added for hmir-2 called `ReleaseHostImports` → `m_scheduler.Finish()`
  there: the known "submit mid-command, rest of the command never runs" failure, which fits the
  garbage guest reads of hmir-2..4. Every Windows remap path already calls `UnmapGpuRange` first
  (`ReplaceFixedRangeWithReserved`, the reserved-target paths), which releases imports between
  commands (`UnmapMemory` → `UnmapMirror`). `ApplyMirror` now imports only the gaps between
  existing imports and never releases.
- `hmir-5` (fix, promotion on, cold cache): menus fine (past the old menu crash), then a guest
  crash during the load into the arena (`Job6` walking a list into a null node: guest data
  corruption again). Shaders only *read* through BDA (`LoadBdaDword`, BVH), so GPU stores into
  imports are ruled out.
- New diagnostic `KYTY_GPU_HOST_MIRROR_PROMOTE=0`: tagged (mirror-only) fault pages are not turned
  into device buffers (`BufferCache::SkipHostMirrorPromotion`, `FaultManager`).
- `hmir-6` (promotion off): no guest crash, but a device loss during the load with the
  long-standing default-path signature (re-detiles `image.cpp:310` / `tiler.cpp:294/326` in
  flight; the game's own breadcrumbs stop in `forwardSimpleOpaque`).
- **Repeats: the host mirror is unstable regardless of promotion.** `hmnp-1..3` (promotion off):
  guest crash in `Job6` at the main menu (`hmnp-1`), device losses after reaching gameplay
  (`hmnp-2`, t≈97) and in the load (`hmnp-3`). `hmpr-1..3` (promotion on): device losses in
  all three (two after reaching gameplay, one in the load). 6/6 failed, vs ~1/5 losses by default.
  So promotion is not the corruption source.
- **Aliasing ruled out by a standalone test** (scratch `importtest.cpp`: pagefile section
  `PAGE_EXECUTE_READWRITE | SEC_COMMIT`, whole-range alias view plus a second view, 8 MiB of the
  view imported on the 5090): alias↔view coherence holds after import, under
  `VirtualProtect` read-only/read-write, for a GPU `vkCmdFillBuffer` into the import, after
  `vkFreeMemory`, and when the view is unmapped and remapped at the same address while an import is
  alive. 0 mismatches.
- **Remaining suspect (not proven):** with the mirror, DMA reads return real host bytes instead of
  zero, and those bytes are stale wherever the newest data is GPU-only (host images of the texture
  cache: render targets / storage images in guest memory, never written back). The default path
  reads zero for one frame, then `FindBuffer` syncs the page. Shaders computing from stale inputs
  write garbage into bound storage buffers, which the normal download path hands to the CPU (fits
  `Job6` crashes and garbage-count GPU hangs). Test: never tag (write no fallback entry for)
  pages that overlap a registered texture-cache image, and see if the losses go away.
- All of this is opt-in; the default path has no codegen or behavior change from part 7.

## Session 2026-11-06, part 6: host-memory mirror (opt-in `KYTY_GPU_HOST_MIRROR=1`, unstable)

- Direct-memory mappings are imported zero-copy (`VK_EXT_external_memory_host`, enabled when
  offered, alignment 0x1000 on the 5090; `BufferCache::ImportHostRange`). BDA page-table entries
  no device buffer owns point into the import with bit 0 set (`WriteFallbackEntries`,
  `FallbackAddress`; restored on `Unregister`). `get_bda_pointer` now reads through tagged
  entries and records the page in the fault buffer, so the CPU promotes it to a device buffer
  (working set). This codegen change applies to all DMA shaders; default path re-checked
  (`dsan-4`, cold cache, reached gameplay).
- `hmir-1` (before tags/promotion): 7.45 GiB imported, some imports fail on remapped ranges,
  10.8 FPS (pages never promoted, so DMA reads stayed on PCIe), device loss at t≈109.
- `hmir-2..4` (tags + release-before-reimport): the game itself crashed in the menus (guest reads
  at `0x136e851ffe`, `0xff`). Suspect: freeing imported host memory disturbs the emulator's guest
  memory views. Next: drop release-on-remap again, find which mapping paths remap without
  `UnmapGpuRange`, and test import/free of a guest view in isolation.
- `dsan-7` (default, warm): autopilot stopped at an unrecognized menu screen (menus 30 FPS);
  re-run to confirm gameplay FPS.

## Session 2026-11-06, part 5: full guest-memory mirror (built, opt-in, not viable as is)

- **What it does (`KYTY_GPU_MIRROR=1`, implied by `KYTY_GPU_BUFFER_DESCRIPTORS=1`):** every
  *direct-memory* mapping (`MapDirectMemory` ×2, `KernelMemoryPoolCommit`: `MapGpuRange(...,
  true)` → `RenderContext::MapMemory(..., mirror)`) is queued (`BufferCache::QueueMirror`) and
  mirrored on the GPU thread (`ApplyMirror`: `FindBuffer` over the mapping, buffer `pinned`,
  so the GC skips it and merged buffers inherit the pin; `UnmapMirror` unpins on unmap). The
  BDA page table then covers all of it.
  - Flexible memory is excluded: mirroring it write-protected guest thread stacks (flexible
    `0x7ebad8000…`), and the process died silently (fault inside fault handling). PRT apertures
    are excluded too (`LibKernel::Memory::OverlapsPrtAperture`).
  - With the mirror, CPU writes reach it once per guest submission slice
    (`RenderContext::PrepareBdaForSubmission`, from `GuestGpu::Process`) instead of before
    every DMA draw (per-draw sync re-armed write protection ~2,800×/frame: 1–2 FPS, `mir-9`).
- **Result: correct coverage, unplayable cost** (`mirs-1`): ~2 FPS. The GPU thread idles
  (220 ms/s); game threads drown in write faults (1.15 s/s of fault handling, 584 ms/s of
  protection changes), 146k uploads/s = **1.3 GB/s** of CPU-written direct memory, plus
  ~37 MB/s of new mappings. NHL 27 streams far more data through GPU-visible memory than it ever
  reads as buffers (textures/assets go through it), and a fully write-tracked mirror turns all of
  it into fault + upload work. ~12.75 GiB gets mapped GPU-visible during a boot into gameplay.
- **Diagnostics added:** `KYTY_GPU_MIRROR_TRACE=1` (mapped-size steps, mirror ranges, big BDA
  syncs and upload reads); a last-chance unhandled-exception filter that prints code, address
  and an unwound stack (`Common::ThreadSampler::PrintContextStack`; it did not fire for the
  stack-protection crash, which never reaches it).
- **Design the data points to instead (not built):** keep page-table coverage, drop eager
  content sync. Sync only a *learned working set*: shaders reading through the page table
  record the pages they touch (like the fault buffer), and at each submission the CPU uploads
  the CPU-dirty pages of that set (plus first-touch faults). Write tracking stays limited to
  memory the GPU actually reads, as today. Risk: first touches of fresh pages read zero for one
  frame, which with garbage-bounded loops can hang the GPU, so it needs the loop watchdog (and
  that needs compile-friendly codegen first).
- Default path re-checked: `dsan-2` 21.6 FPS.

## Session 2026-11-06, part 4: GPU-side descriptors, milestone 1 prototype

The route chosen for 60 FPS: the GPU resolves guest descriptors itself, so the host stops
walking, decoding, specializing and binding them per draw.

- **Existing foundation (shadPS4 design, already in the tree):** a GPU page table maps guest
  pages to host buffer device addresses (`get_bda_pointer` in `spirvEmitterMemory.cpp`,
  `BufferCache::CACHING_PAGEBITS` pages); pages without an entry read zero and are recorded in a
  fault buffer that the CPU processes later (`ProcessFaultBuffer`). `ResourceKind::IndirectBuffer`
  loads decode the V# in the shader (`PrepareIndirectBuffer`, `LoadIndirectScalarBuffer`). Today
  it is used only where the walker cannot resolve a descriptor.
- **Prototype (opt-in `KYTY_GPU_BUFFER_DESCRIPTORS=1`, `ResourceTracking.cpp` `Collect`):**
  every read-only buffer load the indirect path supports (scalar constant loads, raw 8/16/32-bit,
  formatted X) is forced to `IndirectBuffer` even when the walker could resolve it. Stores and
  atomics keep descriptors (write tracking unchanged). The shader store header now records the
  codegen switches (`KYTY_GPU_BUFFER_DESCRIPTORS`, `KYTY_LOOP_WATCHDOG`), so a store written
  under other settings is never reused.
- **Result: not usable yet, two blockers** (`gbd-1..18`):
  1. **Compile time.** Each scalar load becomes two nested conditional branches plus a
     page-table call; big shaders get hundreds of them and single compute pipelines took up to
     9.3 s to compile, past the game's ~5 s GPU watchdog. After ~15 warm-up runs the store
     covered boot and the stalls stopped (0 slow pipelines).
  2. **Coverage.** Then every run lost the device ~15 s into boot (or aborted on a zeroed
     render-target format): the page table only covers memory the buffer cache already turned
     into host buffers (at bind time). With all constant reads going through it, shaders read
     zeros for a frame and garbage-bounded loops hang the GPU.
- **Milestone plan from here:**
  1. **Full guest-memory mirror:** back every GPU-visible guest mapping with host buffers and
     page-table entries when it is mapped (`RenderContext::MapMemory`), with CPU-write sync and
     GPU-write tracking at page level for the whole mirror instead of per bound buffer. Then GPU
     reads never see unmapped pages.
  2. **Branch-light in-shader decode:** one selection per load at most (bounds and page presence
     folded into one predicate, fault recording kept in `get_bda_pointer`), V# fields decoded
     once per handle, so NVIDIA compile times stay in the millisecond range. Measure GPU cost
     (the GPU has ~0.2 ms of headroom per frame at 60).
  3. Re-enable milestone 1 on that base; measure walker/binding time saved per draw.
  4. Images: a bindless table of host image views plus a GPU-visible guest-address → view lookup;
     the CPU keeps creating/uploading images from a per-frame "referenced images" scan, which is
     the hard part (the texture cache must still learn which images a draw samples).
- Default path re-checked after the store reset: `dsan-1` 21.1 FPS, no issues.

## Session 2026-11-06, part 3: multi-threaded draw preparation, step 1

- **Asynchronous pixel-shader materialization (built, exact, opt-in `KYTY_ASYNC_MATERIALIZE=1`).**
  - Per draw, the pixel stage's materialization (SRT walk, descriptor decode, specialization)
    is posted to a spinning helper thread, the vertex stage is materialized on the GPU thread
    meanwhile (`MaterializeAhead`), and both lookups then only match modules
    (`Program::materialized_ahead`; `Program::Materialize` split out of `FindModule`).
  - Only plans whose materialization reads nothing but walked guest memory qualify
    (`HelperEligible`: no indirect descriptors, captured/source reads or selector keys). A
    walker load that faults on the helper never touches the buffer cache: the exception handler
    skips it and marks the walk aborted (`SrtWalkerHelperAbort`), and the GPU thread redoes it.
  - `KYTY_ASYNC_MATERIALIZE_VERIFY=1` recomputes every helper result on the GPU thread:
    `amv1`, **3.1 M checks, 0 mismatches**. ~60% of draws qualify (34k posts/s), ~6/s abort.
  - **No FPS gain** (`amab1`, in-run A/B, 105 s): 20.4 vs 20.5 FPS. The GPU thread saved
    ~31 ms/s of materialization but waited 15 ms/s for the helper, and its draw execution and
    dispatches got 7–10 ms/s slower while the helper spun (8 cores / 16 threads, 9800X3D, the
    game's own threads also busy). Total walk work did not shrink (173 vs 181 ms/s).
  - Lesson: overlapping stages *within* a draw saves well under 1 µs per draw and costs a core.
    Multi-threading only pays with larger independent chunks (whole draws or command buffers),
    which the shared caches (buffer/texture cache, page protection) do not allow today.
- The render-target description memo (`KYTY_RT_MEMO`, on) showed in the same A/B: render-target
  resolve 63 → 47 ms/s (~0.3 µs per draw).
- **Where this leaves 60 FPS:** the GPU now fits (16.5 ms/frame), the GPU thread needs ~45 ms of
  CPU per frame. Per-draw cost is spread over walks (~3.4 µs), bindings (~3 µs), state prep,
  commit and record. Realistic routes, each multi-week: (1) GPU-side descriptor resolution
  (bindless host images + a GPU-visible guest-address → descriptor table, so the host stops
  walking and materializing per draw); (2) making the buffer/texture caches and page tracking
  thread-safe so whole command buffers can be prepared on several threads. Incremental
  single-thread work is unlikely to go much past ~25–30 FPS.

## Session 2026-11-06, part 2: GPU budget, relaxed barriers on, device-loss class

- **GPU side measured (`gpu1`, `KYTY_GPU_TIMING=1`): 26.3 ms of GPU work per frame** (500 ms/s
  at 19 FPS). With infinite CPU that alone capped the game at ~38 FPS.
  - The largest block: a chain of ~11 small compute shaders, each ~36×/frame with 1–18
    workgroups (`0xae45…`, `0x067f…`, `0xcbc6…`, `0x4b38…`, `0x84af…`, `0x9608…`, …, ~130 ms/s
    in the serialized profile `gprof4`), latency-bound behind the full barrier after every
    dispatch.
- **Relaxed compute barriers are now on by default** (`KYTY_RELAXED_CS_BARRIERS=0` restores the
  barrier after every dispatch). GPU work per frame **26.3 → 16.5 ms** (`gpurel1`, 12–14k
  barriers/s elided), i.e. the GPU now fits a 60 FPS frame.
  - `rel-long1`: 320 s of play, 10 screenshots through the 1st period (clock 17:52 → 1:40), no
    ice corruption (the old blotches predate later fixes). 23.3 FPS average.
  - FPS barely moves (`dfr-1..5`: 21.3 / 21.7 / 21.6 / lost / 20.6), because the GPU thread's
    CPU work (~45 ms per frame) is the bottleneck. The GPU headroom pays off once that shrinks.
  - Stability `dfr-1..5`: 1/5 device losses (`dfr-4`, the old `0x0ab3a7c64ccce901` /
    `streamBuffer.cpp:183` signature), 0 hangs — same rate as before (`dfv-1..5`).
- **Device-loss class identified (not yet removed).** A full-marker capture (`dl-5`,
  `KYTY_GPU_CHECKPOINTS=1`, mask 15, top-only, bottom every 8; 1 loss in 5 runs) stopped inside
  dispatch `0x005236bd38db99ab`, whose inner loop walks an adjacency list
  `for (i = 0; i < count; ++i)` with `count` loaded from a buffer filled by the previous dispatch
  `0xa0b8d0d17425cdbd` (atomic counters, capped at 1024). Same class as every earlier analysis:
  a shader looping on a garbage count (light-tile count, indirect instance count) until the TDR.
  How the garbage arises is still open (timing-sensitive: markers mostly hide it).
- **Loop watchdog built, opt-in (`KYTY_LOOP_WATCHDOG=1`, needs a cleared `_PipelineCache`).**
  Every invocation gets a 2^22-iteration budget, ticked in each loop header; any branch with an
  edge to a loop's merge block takes it once the budget is spent (in structured SPIR-V those are
  exactly the loop's exits, so the merge phis stay valid; `LoopWatchdogsEmitted/Loops` count
  coverage). Valid SPIR-V, but **cold NVIDIA compiles of guarded compute pipelines took 1–8+ s
  each** (`Slow pipeline:` console lines, `wdog*`/`warm-*`), and several in a row stall the GPU
  thread past the game's own ~5 s GPU watchdog, which core-dumps (`Coredump` stub flood after
  "GPU Stopped Working" with `GfxEopQueue` stalls). Not shippable until it compiles fast (try a
  per-loop SSA counter instead of a Function variable, or guarding only loops whose bound is
  loaded from memory).
- **Cold-cache stalls kill the game.** Today's earlier "menu hangs" (`walk1`, `winl1`, every
  `wdog*`/`warm-*` run) were all the GPU thread inside `nvgpucomp64` compiling one pipeline for
  more than 8 s. The shader store is keyed by the exe's write time (`BuildStamp`), so every
  rebuild recompiles; the driver cache was saved only every 5 min, so crashing runs kept
  nothing. It now saves every 30 s when dirty. After a rebuild expect 1–2 slow first runs.
- **New:** `Slow pipeline:` console lines (>500 ms creates), render-target description memo
  (`KYTY_RT_MEMO=0` disables; A/B not yet conclusive), lldb dumps 64 frames deep.
- **What 60 FPS still needs:** the GPU now fits (16.5 ms). The GPU thread's CPU work must go from
  ~45 to under 16 ms per frame (~2,800 draws × ~10.5 µs + ~1,000 dispatches): per-draw cost to
  ~3–4 µs, which single-threaded micro-optimization will not reach. Needed: splitting draw
  preparation over threads (e.g. SRT walks/materialization on helpers; they are ~3.5 µs/draw and
  read only guest memory) or GPU-side descriptor resolution; plus removing the device-loss class
  so async recording (driver off the thread, +8%) can become the default.

## Session 2026-11-06 (part 1): SRT walker, async-recording stability, present queue

Where it stands: **gameplay ~20 FPS (immediate recording) / ~22 FPS (`KYTY_ASYNC_RECORD=1`)
on ice, menus a steady 30** (dips only at load screens). 60 FPS is far off; see "What 30/60
needs" at the end of this section. Runs now use `measure.ps1 -SkipIntro 8`, which skips the
pre-game presentation (the crowd intro ran 9–12 FPS and polluted earlier gameplay averages).

- **The SRT walker was ~25% of the GPU thread, invisible to the sampler.** New counter
  `WalkSrtNs` (timer around the JIT walk in `WalkSrt`): 233 ms/s, ~1.7 µs per program lookup
  (`asy4-1`). JIT code has no unwind data, so sampled stacks stopped there and the sampler
  attributed only ~5% to materialization.
  - `KYTY_WALK_STATS=<s>` prints, once, per-plan calls, ns/call, an immediate warm repeat
    (ns with caches hot), repeat rates and code size. `KYTY_WALK_DUMP=<bytes>` /
    `KYTY_WALK_DUMP_HASH=<hex,…>` print a walker's node composition (and write its code to
    `walker-<hash>-<stage>.bin` next to the emulator; disassemble with `llvm-objcopy -I binary
    -O elf64-x86-64` + `llvm-objdump -D`).
  - Findings: 1,065 plans, top plan 3%. Walker code is 5–270 KB per shader. Repeats of the
    same user data: ~0% (per-draw constants), so memoization cannot help. The dominant node is
    `LoadAddressU32` with only user-data/constant arguments (a "leaf read"), emitted as a
    19-instruction thunk plus a call/state check per use. About half the time is cache misses
    on guest descriptor tables just written by game threads on other cores (warm repeat:
    5.3 of 10.0 s).
  - **Fix 1 (on, `KYTY_WALKER_INLINE_READS=0` disables): leaf reads used by exactly one root
    fetch (flat read, descriptor word, output; never a block condition, which the
    reachability pass can re-evaluate) are emitted inline at that fetch**, with the base kept in
    r12 across consecutive reads (callee-saved; pushed/popped at the root exits) and constant
    offsets folded. Root uses are counted per emitted fetch site (a flat slot listed by several
    blocks is fetched once per block).
  - **Fix 2 (on, `KYTY_WALKER_PREFETCH=0` disables): a prologue prefetches the lines of leaf
    reads, only for walkers needing ≤32 prefetches** (offsets ≥32 B apart plus each run's last,
    so every touched line holds one). Uncapped (up to 256) it made the big pixel-shader walkers
    slower (`wpf2`).
  - `KYTY_WALKER_VERIFY=1` compiles a reference walker without inlining for every plan and
    compares flat reads, known descriptor words, activity, outputs and captured addresses on
    every walk: `wver2`, 12.6 M walks, **0 mismatches** (`wver1` found the per-site counting
    bug: extra captures only).
  - Result: walker 1.59 → **1.30 µs per call (−19%)**, e.g. VS `0x8e46…` 703 → 353 ns, PS
    `0xa9f1…` 11.1 → 6.8 µs (`wrep1` → `wpf3`). FPS effect is within run-to-run noise
    (~4–5% of the GPU thread).
  - Still slow: walkers with block control flow (VS `0xe352…` 2.5 µs, PS `0x860c…` 18 µs,
    CS `0xe9d3…` 10 µs warm): the reachability pass and walker-load exceptions, not reads.
- **Uncommitted write-back change from 2026-11-05 night caused device losses; reverted to
  opt-in.** `PreserveGpuImagesForCpuWrite` letting the faulting thread wait
  (`KYTY_IMAGE_WRITEBACK_CALLER_WAIT=1` now) lost the device in 3 of 5 async runs (`hrep-1..5`,
  re-detiles in flight): while the caller waits, the GPU thread can render into the image again
  before the caller marks it CPU-dirty, and the next use re-uploads stale memory over it.
  Default (drain on the GPU thread) restored: `wbfix-1..5` (async) 0/5 losses.
- **Hang with async recording found and fixed: a blocking present held the shared queue
  mutex.** `hang-threads-wpf1.txt` (lldb, all threads): the presenter sat in
  `vkQueuePresentKHR` → `NtDxgkSubmitPresentToHwQueue` holding `GraphicContext::queue_mutex`;
  the recorder worker waited for that mutex to submit; the GPU thread waited on a side-queue
  readback whose producer tick was in the worker's queue; the priority thread waited on the
  same tick. Immediate mode has the same exposure (the GPU thread's own submits take that
  mutex).
  - **Fix (on, `KYTY_PRESENT_QUEUE=0` disables): the presenter has its own `VkQueue`** (third
    queue of the graphics family, `GraphicContext::PresentQueue()` / `PresentQueueMutex()`):
    its scheduler (`CommandScheduler(..., present=true)`) submits there and presents there. Its
    blit waits on the rendering timeline at the frames' `render_tick` (already complete when a
    flip is ready, since flips complete in a priority operation after the tick; the wait only
    orders memory across the queues). Swapchain teardown and splash retirement wait on both
    queues.
  - Async recording itself: ~+8% (`cmp-imm` 20.2 vs `cmp-asy` 21.8 FPS, same build), but
    `cmp-asy` lost the device (re-detiles of 2348×1320 images in flight, the known
    stale-upload signature). **Async stays opt-in.**
- **Diagnostics added:**
  - `GpuThreadWaits/GpuThreadWaitNs` (semaphore waits made by the GPU thread; ~6–15 ms/s, so
    the GPU thread rarely waits on the timeline itself), `GpuThreadSyncWaitNs` (priority-op
    waits), `GpuThreadReadbackNs`, `MaterializeFault*`/`MaterializeCleanNs` (materializations
    with/without walker exceptions: exceptions cost only ~22 ms/s).
  - Sampler: samples blocked in a system call now scan the stack for return addresses into the
    emulator (unwinding through ntdll crashed again, `asy3-1`), reported as `== wait sites ==`.
  - `measure.ps1` always runs `hangwatch.ps1` (lldb stacks to `hang-threads-<Name>.txt`,
    then closes the game) and accepts `-SkipIntro N`; `hang-repro.ps1` loops runs and counts
    device losses and hangs.
- **Where the GPU thread goes now (immediate, `prof-imm`, on ice ~20 FPS, ~2,800 draws and
  ~1,000 dispatches per frame, ~10 µs per draw):** driver ~11%; blocked ~19% (side-queue
  readbacks ~9%, idle waiting for the game ~6%, image write-back drain ~2.4%, `Finish` on guest
  unmaps ~1%); SRT walker ~19%; the rest is spread over many functions under 3% each
  (`FindImagesInRegion`, `NtProtectVirtualMemory`, buffer sync, bindings).
- **Default config verified (`dfv-1..5`, `hang-repro.ps1`, 150 s of on-ice play each):**
  gameplay 21.2 / 21.4 / 17.3 / 21.4 FPS, menus 30.0 in every run, 0 hangs.
  - `dfv-3` lost the device with the long-standing signature (re-detiles of 2348×1320 images,
    `image.cpp:310` / `tiler.cpp:294/326`, in flight; also in `fix3-6`, `grid2`, `chain3`,
    `ret-8`, `wsk0`, `cmp-asy`, …). The system overlay's `CheckVulkanResult` is only its first
    observer (ImGui's texture upload waits idle on the main queue).
  - `dfv-4` aborted at boot (t≈11 s): `unsupported render-target format combination: layout=0
    type=0 order=0` (`textureCommon.cpp:84`), seen once before (`cog1`). Not yet investigated.
- **What 30/60 needs:** 30 FPS needs ~1.5× on the GPU thread, 60 FPS ~3× plus a GPU-side
  cut. No single item left is large; the structural options remain GPU-side descriptor
  resolution (removes walker, readbacks for descriptors, materialization) and splitting draw
  processing over threads. Next cheap items: walker block-control-flow cost and walker-load
  exceptions (an always-readable alias view for walker loads with a software dirty check),
  the write-back drain, render-target re-resolve.

## FPS work, 2026-11-05 late (the crash hunt below is parked at 1 loss in 22 runs)

- **Jump-table shaders no longer use the dispatcher fallback (fix, in the tree).** Any shader
  with a resolved `S_SETPC_B64` jump table (a guest `switch`) was forced to the dispatcher
  translation (loop + `OpSwitch` over blocks, every cross-block value spilled to a variable),
  which runs ~100–400× slower on the RTX 5090. `CFG::LowerIndirectBranches` (`ShaderCFG.cpp`)
  now rewrites each table into a chain of uniform `selector == case` branches (new
  `BranchCondition::IndirectCase`, evaluated in `Translator::AddBranchCondition`; last case
  unconditional since the guest clamps the index), and the goto structurizer handles the rest.
  `KYTY_CFG_DISPATCH_JUMP_TABLES=1` restores the old path (needs a cleared `_PipelineCache`,
  since the cache is keyed by build time, not env).
  - Affected in gameplay: PS `0xde18abd5aa890c0a` **816–1,450 → 3.5 µs per draw** (~45–57
    draws/s, was 7–15% of GPU time), PS `0xb29bea487e845ce7` **10–12.6 ms → 28 µs**, plus CS
    `0x9800a9930b27858b`, `0xf710b45277b2441c`. After the fix no shader falls back (new console
    line `shader dispatcher fallback: …` lists any that still do).
  - Runs `jtA` (fix) vs `jtB` (old path), same autopilot, 105 s of gameplay each: 12.0 vs 11.0
    FPS, command-buffer span 506 vs 535 ms/s. Screenshots render correctly (`autopilot-shots\jt*`).
- **Dispatch chaining re-measured (`chain3`):** in-run A/B 9.7 → 12.5 FPS (+29%), but the
  device was lost again ~51 s into gameplay: 3 of 3 chaining runs ever made lost the device at
  43–51 s, against 1 in 22 default runs. Chaining itself is unsafe; keep it off. Not yet found
  what it breaks (copies in `CommitBindings`/`FindBuffers` have their own full barriers).
- **Where the GPU time goes now (`jtA`, 100 s of gameplay):**
  - Solver `0x0813aca5017577ac` is **36% (183 ms/s, 250 single-workgroup dispatches/s,
    0.73 ms each)**, plus `0x2edf88de3cdb2ee3` ~6%. They are latency-bound. Each one is
    1,024 guest threads compiled as `LocalSize 512` with two guest lanes per host invocation,
    because wave64 runs on a 32-wide host subgroup.
    - Options: make chaining safe (the PS5 overlaps these), or emit wave64 as 1,024 host threads.
  - CPU→GPU buffer uploads (`bufferCache.cpp:569`, `SynchronizeBuffer`): ~3,200/s (~250 per
    frame). Each one has `eAllCommands` → transfer → `eAllCommands` barriers, i.e. a full queue
    drain. (Now batched; see below.)
  - GPU-buffer → image re-detiles: ~106/s, **1.8 GB/s** (`image.cpp:280`, `tiler.cpp:326`).
- **Batched upload barriers (in the tree, on by default).** New counters (`up1`) showed that
  89% of the ~5,400 `SynchronizeBuffer` uploads/s are back-to-back, with nothing recorded
  between them. 95% are ≤64 KiB and only ~1% are into written buffers.
  - How it works: inside `BufferCache::UploadBatchScope` (around draw binding preparation in
    `renderDraw.cpp` and `FindBuffers`…`RebindBuffers` in `DispatchDirect`), uploads are
    queued. They are recorded under one global barrier pair when the scope ends, or before
    anything else is recorded: `CommandBuffer::SetRecordHook` runs on the next `Handle()` and in
    `CommandScheduler::Submit`.
  - Copies into overlapping bytes of the same buffer flush the queue first. Batching follows
    `KYTY_AB_PERIOD`; `KYTY_BATCH_UPLOADS=0` turns it off.
  - `batch1`: in-run A/B, 150 s of gameplay, no device loss, correct screenshots.
    - Barrier pairs: 4,280 uploads/s now need 384 pairs.
    - FPS 12.6 ± 0.4 → 13.3 ± 0.5, draws/s +8%; GPU busy unchanged (~591 ms/s).
- **What limits FPS now (`batch1`):** the PM4/GPU thread is ~95% busy (`Pm4ProcessNs`
  ~955 ms/s).
  - **~455 ms/s of that is blocked in `GpuWaitNs`.** It is mostly synchronous readbacks:
    read-side ~63–78/s, `BufferReadSyncNs` ~260 ms/s; write-fault downloads ~26/s,
    `WriteFaultSyncNs` ~210–230 ms/s.
  - Each readback drains the GPU and stalls the CPU, so CPU and GPU take turns. GPU busy is
    ~45 ms per ~75 ms frame.
  - Next step: make readbacks wait only for the producing work, or avoid them. Readback-reason
    attribution (`rbattr.py`) is the starting point. (Done for submitted producers; see below.)
- **Readback producers measured (`rb1`).** `BufferCache::m_write_ticks` (`TickRangeMap` in
  `rangeSet.h`) records the scheduler tick of the latest GPU write per guest range. Sources:
  `ObtainBuffer` writes, `JoinOverlap` copies, image→buffer syncs, CPU-over-GPU uploads.
  Entries of completed ticks are pruned. `Rb*Producer*` counters classify each download:
  - Write-fault downloads (26/s): **all producers already complete** (10/s tracked + 16/s
    pruned).
  - Reads (58/s): 29% complete, 7% in flight, 6% pruned, and **58% recorded in the current
    command buffer** (descriptor/SRT data the next draw needs; only GPU-side resolution helps).
- **Side-queue readbacks (in the tree, on by default).**
  - The device now gets a second queue of the graphics family (`GraphicContext::readback_queue`).
  - If every GPU write to the bytes is already submitted (and no async-download publish is
    pending), `ReadMemory` calls `BufferCache::DownloadOnSideQueue`. It copies on that queue
    after waiting on the master timeline semaphore for the producer's tick, instead of
    submitting the main command buffer and waiting for everything queued.
  - `KYTY_SIDE_READBACKS=0` disables it; it follows `KYTY_AB_PERIOD` (upload batching no
    longer does; it is always on).
  - `side1`, in-run A/B over 150 s of gameplay: no device loss, correct screenshots.

    | | Off | On |
    |---|---|---|
    | FPS | 12.1 ± 0.4 | 13.0 ± 0.5 |
    | Side readbacks | 0 | 48.5/s, 0.54 ms each |
    | GPU waits | 120/s, 435 ms/s | 75/s, 382 ms/s |
    | Write-fault waits | 223 ms/s | 67 ms/s |
    | Main-queue read waits | 220 ms/s | 340 ms/s |
    | GPU busy | 580 ms/s | 609 ms/s |

    The remaining main-queue reads now find a longer GPU backlog, which is why their wait grew.
- **Where it stands:** per frame (~13 FPS) the PM4 thread does ~44 ms of CPU work and the GPU
  ~47 ms. Even perfect overlap would give only ~21 FPS, so both must shrink about 3× for 60.
  Largest items:
  - GPU: solver `0x0813` (~36%), latency-bound single-workgroup dispatches; overlapping them
    (chaining) still loses the device.
  - CPU: draw processing ~380 ms/s (~10 µs per draw, ~38k draws/s); dispatch ~220 ms/s.
  - Waits: the remaining 34/s "producer just recorded" reads.

### Toward a stable 30 FPS (2026-11-05, night; newest)

- **Baseline (`base30`, with the sampler and GPU timing on):** 17.5 FPS.
  - PM4 thread ~934 ms/s, saturated. Draws ~10 µs each (44k/s); direct dispatches 263 ms/s,
    121 ms/s of it readbacks; GPU waits 269 ms/s.
  - GPU busy 546 ms/s (~31 ms per frame).
- **Readback trace (`KYTY_RB_TRACE=<s>`, prints each readback's reader/writer code address).**
  - 99% of the readbacks whose producer was still being recorded were **4-byte reads** by four
    compute shaders (`0x3f9a9a1ddb4d76ab`, `0xa2932b1ddf6e6544`, `0x9ad619ecb118aaaf`,
    `0x784af6a8839215ae`). Each read a counter that the previous dispatch had just written on
    the GPU.
  - The read happens in the SRT walker (JIT x64 run on the GPU thread for descriptor
    materialization). Its load faults on the GPU-written page, and the handler reads the value
    back (`runtimeLinker.cpp` → `TryReadBufferBacking`).
  - E.g. `0x9ad6`: `count = s_buffer_load(...)`, `if (tid < count) {...}`. The value only
    decides block reachability (resource activity); the descriptors come from a CPU-known table.
- **Fix (in the tree, on by default): condition-only walker loads.**
  - `WalkerCompiler::Compile` marks nodes that no descriptor, flat read or output depends on
    (`Node::condition_only`). Their loads are emitted as
    `mov eax,[r10]; jmp short +5; jmp <unknown>`.
  - When such a load faults on GPU-dirty bytes, the handler skips into the unknown path
    (`SrtWalkerUnknownSkip`) instead of reading back. An unknown condition makes both
    successors reachable, so the resources stay active (conservative).
  - Counter `WalkerLoadsUnknown`; `KYTY_WALKER_SKIP_GPU_CONDITIONS=0` disables it.
  - `wsk1` (same settings as `base30`): 17.5 → 20.5 FPS.
    - Readbacks with the producer still recording: 54 → 0.3/s; GPU waits 109 → 58/s
      (269 → 138 ms/s); dispatch processing 359 → 187 ms/s.
    - Draws/s 44k → 59k; screenshots correct.
- **Cheaper stage timers.** `steady_clock` (`QueryPerformanceCounter`) in the debug-counter
  timers cost ~6.5% of the GPU thread. They now use a calibrated invariant TSC
  (`DebugCounters::FastClock`): QPC share 6.5% → 2.5%, ~9.8 → 9.3 µs per draw. This only
  matters when the perf log/overlay is on.
- **Sampler crash fixed.**
  - Two runs (`mip1`, `tsc1`) died in `ntdll+0x20191`: ntdll's function-table lookup, with
    `rbp`/`r13` = the emulator image base, during sampler windows. The sampler unwound the
    suspended GPU thread while that thread was inside ntdll; it raises thousands of
    exceptions per second for walker loads.
  - The sampler now records only RIP for samples taken inside ntdll.
- **Rare device loss still exists.** `wsk0` (walker skip off) lost the device ~44 s into
  gameplay. Signature as in `ret-8`/`chain3`: last completed work was `streamBuffer.cpp:183`
  copies after indirect dispatch `0x0ab3a7c64ccce901`; the next marked command (a tiler fill)
  never started.
- **Clean measurements of the current defaults:**
  - `wskv-1..4` (verify loop, 150 s each): 0/4 device losses, **19.7–22.2 FPS** (mean ~21).
  - `memo2` (perf log on, sampler off): **~22 FPS** at ~65k draws/s, 9.2 µs per draw;
    PM4 thread ~930 ms/s.
- **Per-thread CPU (`thr1`, `tools/perf/threads.ps1`):** the GPU thread uses 84% of a core
  (the rest is GPU waits); no game thread is above 26%. The GPU thread is the bottleneck.
- **Tried, no gain, reverted:** a per-thread memo of `TextureCache::FindImagesInRegion`
  (generation-invalidated). `memo2` in-run A/B: 22.0 vs 21.9 FPS; render-target resolve
  66.5 → 62.2 ms/s only.
  - Note: `memo1` showed a fake +29% because chaining and side readbacks still followed
    `KYTY_AB_PERIOD`. Both are now always on (only their env switches disable them), so the
    A/B toggle is free for new experiments.
- **What 30 FPS needs:** per frame ~2,950 draws × 9.2 µs ≈ 27 ms plus ~1,000 dispatches ×
  ~8 µs ≈ 8 ms of GPU-thread CPU work, before waits; the budget is 33 ms.
  - Per-draw cost must drop ~40%, or work must leave the GPU thread.
  - Remaining exclusive costs: driver ~11%, exceptions (walker loads ~7.7k/s + GPU-thread write
    faults) ~4–5%, page re-protection (`NtProtectVirtualMemory`) ~3%, Vulkan memory
    allocate/free ~2%, `memmove` ~2%, materialization ~2%.
  - The structural options: an async Vulkan recording thread (driver and emulator record code,
    ~20% of the thread), or GPU-side descriptor resolution.
- **Async Vulkan command recording (built, opt-in `KYTY_ASYNC_RECORD=1`, off by default).**
  - `CommandBuffer::Handle()` now returns a `CmdRef` (`commandRecorder.h`) instead of a
    `vk::CommandBuffer`. Without a recorder it calls Vulkan directly (immediate mode; the
    presenter's scheduler always is).
  - With a recorder, each command is a closure in a 16 KiB chunk arena; pointer arguments
    (barriers, regions, descriptor writes, rendering info, push constants) are deep-copied.
  - A worker thread replays the chunks into the pooled `VkCommandBuffer`, then ends and submits
    it with the tick the GPU thread assigned. Chunks are recycled per submitted buffer.
    `CmdRef::Raw(fn)` is the escape hatch for raw calls; the profiler keys on `CmdRef::Id()`.
  - It works: gameplay renders correctly and runs stably. On the GPU thread the driver share
    (`vkGetInstanceProcAddr`) fell 10.8% → 0.9%, and per-draw record+commit 1.06 → 0.5 µs.
  - **But no FPS gain** (`async2..5` vs `memo2`/`imm-*`: 18–21 vs 20–22 FPS). GPU-thread waits
    rose from ~4% to ~11%.
    - Every synchronous `Wait(CurrentTick)` now also covers the worker's replay and submit
      latency.
    - Texture write-back downloads before CPU writes (`PreserveGpuImagesForCpuWrite`, a full
      drain each) jumped 1.4 → 17–48/s: images otherwise downloaded in the background once
      their tick completes are still GPU-owned when the CPU writes, since ticks now complete
      later.
  - Wait attribution (`KYTY_WAIT_STACKS=<s>`, GPU-thread stacks of sampled blocking waits):
    38/60 are such write-back waits run via `GuestGpu::ProcessCommands`; 18/60 are buffer
    readbacks from faults.
  - Conclusion: async recording only pays once the synchronous drains are gone. Next: make the
    image write-back wait only for the image's producer (side queue, like buffer readbacks), or
    avoid it. Then re-measure with `KYTY_ASYNC_RECORD=1`.
  - Default path verified after the `CmdRef` change: `imm-1/2`, 21.6 / 20.6 FPS, 0/2 device
    losses.
- **Where it stood earlier (`tsc2`, ~19 FPS, a heavier scene):** GPU busy ~610 ms/s (~32 ms per
  frame); PM4 thread ~914 ms/s, draws ~9.3 µs each.
  - Both CPU and GPU must shrink ~35% for a stable 30.
  - CPU candidates per draw: program lookup/materialization (~3.7 µs), render-target re-resolve
    (`ResolveRenderColorTarget` rebuilds the full description + `UpdateSize` every draw, ~4%
    of the thread), driver calls (~10%), buffer uploads (~6%), side readbacks (~6%).

### Chaining "crash" re-tested (2026-11-05, latest)

- **Chaining no longer loses the device.** Solver allowlist, 150 s of gameplay per run unless
  noted:

  | Runs | Configuration | Device losses |
  |---|---|---|
  | `chk1` | with the overlap detector, 90 s | 0/1 |
  | `chv-1..4` | always on | 0/4 |
  | `chold-1..2` | old dispatcher jump tables | 0/2 |
  | `chrep-1..2` | upload batching and side readbacks off, A/B + GPU timing | 0/2 |
  | `chorig-1..2` | this morning's exact `chain3` configuration | 0/2 |
  | `ch1q-1..2` | as `chorig`, but with a single Vulkan queue | 1/2 |

  This morning's 3/3 losses at 43–51 s are not reproducible, and no single change from today
  explains them. The one `ch1q` loss came **during the arena load** (texture upload in flight;
  see the next item), not in a chain.
- **Overlap detector (`KYTY_CHAIN_CHECK=1`).** It prints descriptor ranges of a chained
  dispatch that overlap a range an earlier chain member wrote.
  - Chains are runs of up to 6 `0x2edf` dispatches.
  - The only overlap: every member "writes" the same 4-byte buffer `0x116000c100`. Neither
    solver has atomics (only `BUFFER_STORE_DWORDX2/X3`), so it is a binding marked written whose
    stores fall out of bounds, not a real race.
- **Chaining A/B (`chrep-1/2`, same run):** 13.5 → 16.9 and 13.9 → 18.3 FPS (+25–32%).
  - GPU busy 620 → 500 ms/s; GPU wait 465 → 297 ms/s.
  - `chv` runs (chaining always on, no sampler or GPU timing): ~19 FPS.
- **Now on by default**:
  - `KYTY_CHAIN_SINGLE_GROUP` defaults to on (`=0` disables).
  - `KYTY_CHAIN_SHADERS` defaults to `0813aca5017577ac,2edf88de3cdb2ee3` (`all` = every
    single-workgroup dispatch, untested).
- **New fix: guest images with more mip levels than the full chain.**
  - NHL 27 creates e.g. 128×128 textures with 10 levels (AMD clamps the extras to 1×1). These
    were passed to `vkCreateImage` unchanged, which is invalid
    (VUID-VkImageCreateInfo-mipLevels-00958), and uploads copied into levels 8–9.
  - The `ch1q-2` device loss had exactly such an upload in flight (`image.cpp:280`, 128×128,
    10 levels, `R16G16B16A16_UINT`, right after its detile dispatch). This matches the old
    "texture upload in flight" signature (`tex1`, `hunt9-5`, `fix2-2`).
  - The host image now gets at most the full chain (`Image::Image`). `Upload`/`Download`
    drop regions of the extra levels, `FindView` maps views of them to the last host level,
    per-level barriers and `ClearImage` skip them.
  - Counters: `ImagesMipClamped`, `ImageCopiesMipSkipped`. A console line prints the first 10
    clamped images.
  - `mip2` renders correctly. Not yet proven to remove the load-time loss.
- **New defaults verified (`defv-1..6`, verify loop, 150 s of gameplay each):** 0/6 device
  losses.
  - **18.3–19.2 FPS in gameplay** (mean ~18.8; was ~10 at the start of the day). The sampler
    and GPU timing were off in these runs, as in `chv`.
  - ~877 images per run had their mip levels clamped.
  - `chshot`: on-ice screenshots with chaining render correctly.
- One unexplained host crash (`mip1`): a null-pointer read at `0x3d8` in a non-emulator module
  on a host thread, while shaders compiled during the first load after a rebuild. It did not
  repeat in `mip2`.

## Resume here first (2026-11-05): GPU device-loss fix in progress

The device loss blocks every overlap/FPS optimization (dispatch chaining, relaxed barriers), so
finish it before FPS work. Everything is uncommitted on branch `nhl27pc`; the current build
compiles and is staged. Details and evidence for each step are in "GPU hang investigation"
bullets further down ("Device-loss root cause found", "Root cause (2026-11-05, late)",
"Correction").

### What is established

- Device losses (~1 in 3 default autopilot runs before the fixes; Windows logs `nvlddmkm`
  event 153, no Display 4101) are the host GPU stuck in a **consumer of stale GPU-produced data**:
  - **A:** indirect draw `0x3614b5e9e443323e` read an instance count of ~1.35e8 / ~1.04e9; the
    emulator's CPU backing held the same garbage while the range was marked GPU-written.
  - **B/C:** light-tile pass, indirect dispatch `0x0f837cbe319b9a4c`, loops
    `for i < count` with `count` from `IMAGE_LOAD` of a per-tile `R32G32_UINT` {count, offset}
    image (1174×660 or 1024×512). Seen in 4 captures in a row (`hunt11-1/2`, `hunt12-1/2`,
    `hunt13-1/2`). That image is re-detiled right before the readers (`image.cpp:279` upload,
    reasons: GPU buffer writes + CPU writes); no dispatch writes it as an image.
  - At upload the grid image is `gpu_modified=1`, `buffer_modified=1` (a dispatch wrote a
    buffer starting exactly at the grid address: the game writes the grid as a buffer with
    shader-side tiling) **and** `cpu_dirty=1`; a CPU-written 2048×2048 BC texture (format 141)
    lies inside the grid's memory range (streaming memory reuse).
- `KYTY_CHECK_GRID_UPLOADS` reads the CPU backing, which is stale for GPU-written memory: its
  "SUSPICIOUS" lines do **not** prove the uploaded data is garbage.

### Fixes in the tree (keep; both are correct regardless)

1. Unaligned 32-bit buffer access (`spirvEmitterMemory.cpp`: `LoadWordPrepared`,
   `StoreWordPrepared`, `LoadIndirectBuffer`). Did not change the crash rate by itself.
2. `TextureCache::PreserveGpuImagesForCpuWrite` (called in `RenderContext::HandleFault` write
   path and `RenderContext::InvalidateMemory`): writes GPU-modified images back before a CPU
   write. `fix3-*`: 5/6 runs clean (1 loss during the arena load); fires ~700–1,000×/run.
   Limitation: it only works while the image is `SafeToDownload()` (not yet CPU-dirty or
   buffer-modified), which the grid image is not at upload time.

### 2026-11-05 evening session (newest)

- **"Writer 07af never executes" was a diagnostic artifact.** The indirect-args log in
  `RenderExecutor::DispatchIndirect` called `TryReadBufferBacking` on GPU-modified args. That
  downloads (`BufferCache::ReadMemory` → `DownloadBufferMemory` → `CommandScheduler::Wait` →
  `Submit`) **after** `vk_buffer = buffer.Handle()` was captured. The dispatch was then recorded
  into an already submitted Vulkan buffer and silently never ran, so every diagnostic run
  (`fill1/2/4/5`, `stack1`) crashed on the first gameplay frame. Proof: a fill marker after the
  copy also never landed; the handle at the end of the dispatch differed from the captured one
  while the scheduler tick advanced by exactly one; the submit stack (probe `g_submit_probe`)
  pointed at `TryReadBufferBacking`. Fixed: the log skips the backing read when the range is
  GPU-dirty.
- **General hazard this exposed:** `DispatchDirect` (`renderCompute.cpp`, handle captured
  *before* `CommitBindings`) and the draw path (`renderDraw.cpp`, captured before
  `CommitVertexBuffers`/`CommitBindings`/`CommitIndexBuffer`) record into a raw handle. Anything
  in between that submits (stream-buffer wrap → `CommandScheduler::Wait`, readbacks) drops the
  rest of that command. New always-on detector: `CommandBuffer::RawHandle()` compared at the end
  of every draw/dispatch → `RETIRED RECORDING: <what> <hash>` (`ReportRetiredRecording`, first
  50). A clean 150 s run (`retired1`) printed **0**.
- Verification `ret-*` (10 default runs, current tree): **1/10 device losses** (`ret-8`, 127 s
  perf time, ~60 s into gameplay), **0 retired recordings in all 10** — dropped command buffers
  are not the cause of the remaining hang. Default markers put the stall in unmarked work after
  dispatch `0x0ab3a7c64ccce901` / stream-buffer uploads.
- Diagnostic capture `hunt18-*` (`hunt-loop.ps1 -Attempts 12 -Captures 1`, full markers +
  indirect-args log): **0/12 device losses**, all ~150 s of gameplay. Before today the same
  loop captured a loss within 2–5 runs. Since `KYTY_GPU_LOG_INDIRECT=1` (used by every
  `hunt*` capture) carried the args-readback artifact above, **earlier hunt reports
  (light-pass grid signatures B/C, `hunt11`–`hunt17`) may be partly self-inflicted**; don't
  build on them without a fresh capture. Current tree overall: 1 loss in 22 runs.
- The present thread's submits are on the presenter's own `present_scheduler` (not a race).
- Tools added: `Common::ThreadSampler::PrintCurrentStack(label)` (symbolized stack on Windows);
  args-ring entries now print `fill=` (feed… = slot executed), `src_offset/size`, `cb`, `tick`,
  and the record that follows the log.

- CPU-over-GPU upload logging (`CpuOverGpuUploads`): **0** events in a 120 s run — the
  `SynchronizeBuffer` hypothesis is not supported.
- GPU-side samples of the grid upload source (`KYTY_CHECK_GRID_UPLOADS` + indirect-args ring):
  high-entropy, BC-texture-like bytes at all sampled offsets — the grid image was re-detiled from
  memory that never held the grid.
- `KYTY_TRACE_IMAGE_READS=<hash>`: normally the light pass reads a stable grid (`gpu_modified=1`,
  never re-uploaded). Grid writes are storage-image binds (2 per frame, `grid write storage`). On a
  resolution change the grid moves to fresh memory (e.g. `0x12e9000000`, 1024×512 ↔ 1174×660);
  the first storage bind re-detiles garbage, and the crash follows in that frame (`hunt16-3`).
- No alias images overlap the grid; no dispatches are dropped for lack of a program.
- Tried and **disabled by default**: publishing GPU-written images before an overlapping new image
  (`KYTY_OVERLAP_PUBLISH=1`): NHL 27's transient 4K targets overlap constantly, 362 publishes during
  boot stalled the GPU thread (boot hang, guest coredump).
- **Current suspect (being verified):** `RenderExecutor::TryConsumeComputeMetaClear` skips *any*
  dispatch (without bitwise XOR) that writes a buffer at a registered metadata base
  (`m_surface_metas`). In a reused transient heap, stale metadata entries make it drop unrelated
  GPU work (e.g. a grid clear or an args producer) — consistent with every signature reading
  "never written this frame" data. Shader `0x1bc3f95f9a62d03b` was consumed this way ≥200 times
  per minute. Experiment `KYTY_META_CLEAR_EXECUTE=1` keeps the bookkeeping but runs the dispatch.
  - **It corrupts gameplay rendering** (the user saw it: the rink becomes spiky red/black
    triangles; menus and the pre-game tunnel look fine). Keep it **off** (it is opt-in).
  - With it on, 0/10 device losses (`meta-*`), but only 4 runs reached full length (the others
    were cut short by overlapping test runs, see below) — weak evidence that the skipped dispatches
    matter for the hang. Next: find out which consumed dispatches are real metadata clears and
    which are unrelated work, e.g. require the written descriptor's range to equal the registered
    metadata range before consuming, instead of base-address equality only.
- **Impostor metadata clears (likely root cause):** with the registry now recording each
  entry's owning depth image and range size (`MetaDataInfo::owner/size`,
  `TextureCache::DescribeMeta`), a 60 s run showed 106 of 112 distinct consumed
  (shader, address) pairs are genuine (`0x1bc3f95f9a62d03b`, single buffer, written size ==
  registered HTILE size) and 6 are impostors: multi-buffer passes (`0x7fd524a7ac97dd14` 4
  buffers / 0x355000 bytes, `0x1285995486ad7bb5`, `0xc5c0b2f4e61dd6ba`, `0x097db40f858eebf4`,
  `0x32d7a685f90e9ee7`) whose output buffer starts at a live 0x8000-byte HTILE base
  (`0x11c0280000`) — they were dropped entirely. `0x7fd5…` was in flight in `hunt10-2`.
  **Fix:** `TryConsumeComputeMetaClear` consumes only a dispatch whose single written buffer is
  exactly the registered range of a live owner; others run natively. Gameplay renders correctly
  (`exact1`). `KYTY_META_CLEAR_EXECUTE` removed.
  - Verification `exact-*` (10 default runs): **2/10 device losses**, both ~30–45 s into
    gameplay; **all 10 reached gameplay** (load-time losses were ~1 in 3 before). Default markers
    do not localize the remaining two; diagnostic capture `hunt17-*` next.
- **Process hygiene:** stopping a background verification task by killing a matching
  PowerShell process did not stop it; it kept launching emulators (with the experiment's env) in
  parallel with foreground tests, producing screenshots from the wrong instance and a stalled
  perf log. Stop background loops through the task tool, and check
  `Get-Process kyty_emulator` before starting a run.
- `measure.ps1 -ShotsAfter 25,45` now saves emulator-side screenshots N s after gameplay starts
  (`autopilot-shots\<Name>\gameplay+N.png`) for rendering checks.

### Working hypothesis to test next (2026-11-05 morning; not supported, see above)

The detile source for the grid comes from `BufferCache::ObtainBufferForImage` →
`SynchronizeBuffer` → `ForEachUploadRange`: CPU-modified pages (from the overlapping streamed
texture) are uploaded from the CPU backing **over GPU-written bytes of the grid buffer**, so the
detiled grid is partly garbage → huge `count` → the light pass loops for seconds → TDR. The same
"CPU upload over GPU-written bytes" would explain signature A.

### Next steps (in order)

1. **Test the hypothesis directly** (diagnostic, opt-in):
   - In `BufferCache::SynchronizeBuffer` (`bufferCache.cpp`, the `ForEachUploadRange` lambda),
     log uploads whose range intersects `m_gpu_modified_ranges` / `IsRegionGpuModified` (address,
     size, buffer), first N per second. If the grid range shows up right before the hang, the
     hypothesis holds. Also check `MemoryTracker::ForEachUploadRange` semantics: does it upload
     CPU-dirty pages that also hold GPU-dirty bytes? (Buffer write faults call
     `ReadMemory(is_write)` to download GPU bytes first — find out why that did not cover these
     pages: e.g. the GPU-modified state lives only on the image, or the download window misses.)
   - GPU-side truth for the grid: in `Image::Upload` for `R32G32_UINT`, copy a few words of the
     detile *output* (linear buffer) into the device-loss-safe ring (reuse
     `GpuLogIndirectArgs`), so the report shows the counts the light pass will read.
2. **Fix** according to 1: never let a CPU upload overwrite GPU-written bytes that were not
   downloaded first (download-before-upload for mixed pages, or byte-precise upload ranges that
   skip GPU-dirty bytes). Keep `PreserveGpuImagesForCpuWrite`.
3. **Verify:** `tools\perf\verify-loop.ps1 -Prefix fixN -Runs 10` (default config). Goal 0/10
   device losses (baseline ~3/10). Check a gameplay frame for corruption
   (`autopilot-shots\<run>\gameplay.png`).
4. **Then** retry the FPS overlap work that the hang blocked: `KYTY_CHAIN_SINGLE_GROUP=1
   KYTY_CHAIN_SHADERS=0813aca5017577ac,2edf88de3cdb2ee3 KYTY_AB_PERIOD=5`, long A/B (it showed
   7.9 → 11.6 FPS in 29 s before crashing).
5. **Before committing:** gate or remove diagnostic noise that is always on
   (`LogUnalignedBufferBases` prints 200 lines per run; the grid check is env-gated), keep the
   tools, split into commits (unaligned access fix; image write-back fix; diagnostics/tools;
   autopilot/input pipe/profiler).

### How to reproduce and capture

- Capture loop (diagnostic markers, stops after 2 device losses, usually within 2–5 runs):
  `powershell -File tools\perf\hunt-loop.ps1 -Prefix huntN`. Reports: `..\console-huntN-<n>.log`,
  section after "GPU buffer markers after device loss" (in-flight records, indirect args with
  host state, dispatches writing below the bad record, records touching the stalled command's
  image).
- Shader dumps: `$env:KYTY_DUMP_SHADERS='hash,…'` then a short `measure.ps1 -Autopilot` run;
  files `shader-<hash>-*.txt/.spvasm` next to the emulator (cached shaders are recompiled).
- PM4 around a dispatch: `$env:KYTY_TRACE_AFTER='hash'` (console lines `trace after dispatch`).
- Environment variables set in the calling PowerShell are inherited by `measure.ps1`; they do
  **not** persist between separate tool calls/shells.

## Resume here (2026-11-04 evening)

- **State:** uncommitted on branch `nhl27pc`; everything below builds and is staged (the
  sampled-marker work from the previous continuation now compiles and runs).
- **Getting to gameplay:** the game no longer reaches gameplay unattended (sign-in notice, then
  Play Now → Select Team). Use `tools\perf\measure.ps1 -Name X -Seconds N -Autopilot`: it drives
  sign-in → Play Now → **Triangle (CPU vs CPU)** → confirmations → Start Game through the new
  emulator input pipe and emulator-side screenshots (no focus needed, works minimized), ~60 s from
  launch to the arena. `-Seconds` then counts gameplay time. Details in `tools/perf/README.md`.
- **Gameplay baseline (this build, autopilot runs):** ~10 FPS, ~31k draws/s, ~12–15k dispatches/s.
  - GPU thread (sampler, 60 s): 38% blocked in `vkWaitSemaphores`, almost all inside
    `BufferCache::DownloadBufferMemory` (synchronous readbacks).
  - New counters: read-side readbacks ~51–59/s, 240–300 ms/s (~4.5 ms each, ~1 MB each).
    **Write-fault readbacks** (CPU writes to pages holding GPU-written bytes; previously not
    counted at all): ~22/s, ~1.1 MB each, 50–150 ms/s on the GPU thread and ~220 ms/s of
    blocked game threads (`WriteFault*`).
  - `KYTY_GPU_TIMING=1`: **GPU busy ~570 ms/s ≈ 59 ms per frame**. Even perfect CPU/GPU overlap
    would give only ~17 FPS, so GPU-side cost must come down as well as the readback waits.
  - Texture re-uploads caused by GPU buffer writes to image memory (`TextureUploadsGpu`): ~146/s,
    ~1.65 GB/s detiled (~170 MB per frame); CPU-written uploads ~118/s, ~93 MB/s.
- **Per-shader GPU profile (new, `KYTY_GPU_PROFILE`, 3 gameplay runs; reports and shader dumps
  in `gpu-profiles\` in the workspace root):** command-buffer span ~410–460 ms/s; 88% of marked
  time is in the top 80 of ~650 entries.
  - **Single-workgroup compute dispatches** `0x0813aca5017577ac` (78–163 calls/s, ~710–790 µs
    each, 14–28%) and `0x2edf88de3cdb2ee3` (~690–740 µs each, up to 7%). Both on the
    **graphics** queue. `0x0813…` is a ~1,660-instruction wave64 shader, 1,024 threads
    (`V_LSHL_ADD_U32 v30, s2, 10, v0`), LDS-heavy, 11 `s_barrier`s, an outer loop with three
    inner loops, `V_RSQ/V_SQRT/V_MAC` math (looks like an iterative cloth/hair-style solver).
    One workgroup occupies one SM of 170 for ~0.75 ms while nothing else runs; latency-bound.
    IR shows per-lane exec predication (~1,000 `SelectU32`, ~900 `Phi`), no subgroup emulation.
  - Pixel shaders `0xde18abd5aa890c0a` (~1.3–1.5 ms per draw, ~4–5 per frame, 12–15%) and
    `0xb29bea487e845ce7` (~12–13 ms, once per ~8 frames): ~1,100-instruction lighting-style
    shaders with one loop each.
  - Emulator work: texture uploads `image.cpp:277` + tiler ~20–25 ms/s; `bufferCache.cpp:554`
    ~10–14 ms/s.
  - Async-compute (ACB) work is serialized on the single host queue; its largest entry is
    `0xbb744ce23ff1e901` (~15 ms/s).
- **Why the solver dispatches cost so much (2026-11-04, late):**
  - SPIR-V (`KYTY_DUMP_SHADERS` now also writes `.spvasm`): buffers are descriptor-bound SSBO
    arrays, LDS is a `Workgroup` array — **no BDA/page-table indirection**. The translation is
    bulky (`0x2edf…`: 512 threads, ~2,600 `OpSelect`, ~2,300 `OpPhi`, ~1,000 conditional
    branches; several times the guest instruction count) but not pathological.
  - `KYTY_TRACE_AFTER=hash` (prints the next PM4 packets after a dispatch of that shader) shows
    the real cause: the guest issues **chains of up to 5+ back-to-back 1-group dispatches**
    (`0x0813…` ×5, mixed with `2edf…`, each `SET_SH_REG 0x240` → its own user-data table,
    i.e. one per object) with **no sync packet in between**; it syncs only after the chain
    (`EVENT_WRITE 0x410/0x407` + `ACQUIRE_MEM`). The PS5 overlaps those dispatches across CUs;
    the emulator puts a full compute barrier after each, so ~13–17 one-SM solves run back to back.
  - Experiment `KYTY_CHAIN_SINGLE_GROUP=1` (+ optional allowlist `KYTY_CHAIN_SHADERS=hash,…`;
    reuses the relaxed-barrier chaining for consecutive 1-group dispatches only; A/B-able):
    - All 1-group dispatches (`chain1`): ~3,000 qualify per second (far more than the solvers);
      29 s A/B, FPS 7.7 vs 7.6 — no visible gain, noisy.
    - Solver allowlist only (`chain2`): ~180 barriers elided/s; 29 s A/B, FPS 7.9 → 11.6 and GPU
      busy per frame 61 → 45 ms with similar draws per frame — promising but short and noisy.
    - **Both runs lost the device ~43 s into gameplay** in the known SSS pass
      (`SubSurfaceScatteringGenerateTilesCS`, fault IPs `0x20021a980…0x20021d9a0`). None of the
      six non-chaining gameplay runs that day (65–150 s each) did. Together with "markers on
      every dispatch hide the hang", this points to the hang being a synchronization race that
      more GPU overlap makes likelier. Keep chaining off by default.
- **Device-loss root cause found (2026-11-05):** a corrupted indirect draw argument record.
  - New marker modes: `KYTY_GPU_CHECKPOINTS_TOP_ONLY=1` (draw/dispatch markers only at top of
    pipe, which does not serialize and so does not hide the hang) with
    `KYTY_GPU_CHECKPOINTS_BOTTOM_EVERY=8` (completion marker on every 8th) pinned the stall to
    an indirect draw (shader `0x3614b5e9e443323e`, args in the `0x11c0…` transient heap) right
    after the game's 32-thread copy dispatches `0x87ed4ea1bccbd79a`.
  - `KYTY_GPU_LOG_INDIRECT=1` (GPU copies each indirect record into device-loss-safe memory just
    before the draw) showed the record the GPU read: `vertexCount=4`,
    **`instanceCount=0x080c0c83` (~135M)**. Its high half `0x080c` (2060) is the instance count the
    previous such draw read: a dword read/written **2 bytes off**.
  - A bind-time check found 200 buffers per minute bound with guest bases ≡ 2 (mod 4) (stride-2
    / 16-bit buffers in the same `0x11c0…` heap, mostly written), e.g. shaders `0x741d…`,
    `0x040e…`, `0x81d3…`. The SPIR-V emitter's descriptor path computed dword indices as
    `byte_address >> 2`, so unaligned dword loads returned the aligned dword and unaligned
    dword stores wrote 2 bytes early, clobbering neighbors. The guest GPU allows unaligned
    access. Allocation addresses move per frame, hence the randomness.
  - **Fix:** `LoadWordPrepared`/`StoreWordPrepared` (`spirvEmitterMemory.cpp`) now handle
    `address & 3 != 0` for 32-bit buffer views: loads merge two dwords (as `LoadBda` already
    did), stores do two masked atomic merges. Multi-dword raw accesses go through the same
    functions. `LoadIndirectBuffer` (descriptors read from memory) now uses the unaligned
    `LoadBda` for 32-bit components too.
  - **Verification: not fixed yet.** Default config, 150 s gameplay per run: after the first fix
    4/5 clean (`fix1-*`), after the second 3/5 clean (`fix2-*`); 3/10 lost the device (load and
    gameplay), about the pre-fix rate. Either the corrupted record has another writer or the
    unaligned case was not the trigger.
  - Diagnostic config on the fixed build (`hunt8-2`): same indirect draw `0x3614…` (args at
    `0x11c0556800`) read `instanceCount=0x3e211dd4` (~1e9, looks like a leftover float). The
    record-time host view: `gpu_dirty=1` and the emulator's CPU backing held **the same garbage**
    — the GPU read the memory's old contents; this frame's producer never reached the buffer the
    draw reads. The 2-byte-shifted value seen earlier was probably the same staleness.
    Candidate producer: the `0x87ed…` 128-byte copies right before. Dispatch marker records now
    carry the first written / first read-only buffer base (`address=` / `read=`) to check
    whether those copies target the args address and where their source comes from.
  - Second signature (`hunt9-5`, also likely `tex1`, `hunt2`, `fix2-2`): no indirect draw in
    flight; the only started-not-completed command is the texture re-upload copy
    (`image.cpp:277` `copyBufferToImage`) right after its detile dispatch (`tiler.cpp:325`)
    completed. Device-loss addresses again only `InstructionPointerUnknown`.
  - `hunt11-1`/`-2` (same signature twice): in flight the indirect dispatch
    `0x0f837cbe319b9a4c` (sane args {112,1,1} / {5920,1,1}), right after the 147×83-group
    `0x4f0398f4d7966add`. `0x0f83` is a light-tile shading pass whose inner loop runs
    `for (i = 0; i < count; i++)` with `count` from an `IMAGE_LOAD` of a per-tile {count, offset}
    image; garbage in that image makes it loop for seconds (TDR). Same class as signature A:
    the consumer reads stale/garbage GPU-produced data. Many hangs follow texture re-uploads
    (`image.cpp:277`) of screen-sized images (1174×660 `B10G11R11`/`R16`), so the suspect is the
    texture cache re-uploading an image from stale guest memory over GPU-written image data.
    Records now carry image addresses; the report lists uploads/writers/readers of the image the
    stalled command reads.
  - **Root cause (2026-11-05, late):** image re-upload over GPU-written data. `hunt12/13`: the
    image the light pass reads (`R32G32_UINT` 1174×660 / 1024×512, per-tile {count, offset}) had
    no dispatch writer in 60,000 records; its only producer in the window was an emulator re-upload
    whose reason was **CPU writes**. `KYTY_CHECK_GRID_UPLOADS=1` showed those uploads (2 per run)
    carry garbage (~99% of elements > 65535, max ≈ 0xFFFFFFFF), while the pass normally runs fine
    every frame — i.e. the grid is normally GPU-rendered, and a CPU write to its pages (false
    sharing or heap reuse) made `InvalidateCpuAliases` mark the whole image CPU-dirty; the next
    bind re-detiled stale guest memory (never written back) over the GPU data.
  - **Fix:** `TextureCache::PreserveGpuImagesForCpuWrite` (called from the write-fault path and
    `RenderContext::InvalidateMemory` before images are invalidated) synchronously downloads any
    overlapping GPU-modified, downloadable image to guest memory on the GPU thread, so the CPU
    write lands on the GPU's data (as on the PS5). Counter `TextureWriteFaultDownloads`.
    Verification (`fix3-*`, 6 default runs): 5/6 clean through 150 s of gameplay, 1 device loss
    during the arena load (before: 7/10 clean). The write-back fires ~700–1,000 times per run.
  - **Correction:** the grid check reads the CPU *backing*, which is stale whenever the GPU wrote
    the memory, so "garbage at upload" is not proven. `grid2` shows the grid image at upload is
    `gpu_modified=1`, `buffer_modified=1` (a dispatch wrote a buffer starting exactly at the
    grid address — the game likely writes the grid as a buffer with shader-side tiling, and the
    emulator re-detiles it from that GPU buffer, the normal `TextureUploadsGpu` path) and
    `cpu_dirty=1`; a CPU-written 2048×2048 BC texture (format 141) overlaps the grid's range
    (streaming memory reuse). Open hypothesis: the CPU-dirty pages from that overlap make
    `ObtainBufferForImage` upload stale CPU bytes over parts of the GPU-written buffer before
    the detile. Next: compare the detile source on the GPU (copy a few grid words from the
    source buffer into the device-loss-safe ring, like the indirect-args log) with the backing,
    and log `ObtainBufferForImage` CPU uploads that overlap GPU-dirty ranges.
  - **Next (hang):** (A) catch signature A with the new `address=/read=` fields and follow the
    args' producer (is it on the guest async-compute queue, a CP write, or a buffer the copies
    write through a different host object?); (B) log the upload's image size/format/regions at
    record time for signature B. Default runs: ~1 in 3 loses the device.
- **Next for FPS:** (1) fix the device-loss race first — it blocks every overlap optimization
  (chaining, relaxed barriers); then re-measure `KYTY_CHAIN_SHADERS` with a long A/B; (2) cut
  write-fault readbacks (wait only for the producer tick, or byte-precise handling
  of CPU writes to mixed pages); (3) GPU-side descriptor resolution for the read-side readbacks.
- **Hang lead:** run `tex1` lost the device while loading the arena with the default markers.
  `VK_EXT_device_fault` again reported only an instruction pointer, `0x200434ad0`, 0x200 from
  the previous capture (`0x2004348d0`) — likely the same code. As in `mark8a`, the last completed
  emulator work was a texture upload (tiler fill + detile + `image.cpp:277` copy), followed by
  unmarked game work and then a readback copy (`bufferCache.cpp:217`, started, never completed).
  Top-of-pipe "started" markers do not prove the copy itself hung.

## Previous continuation (2026-11-03/04)

- **State then:** the last successful Release build matched SHA-256
  `DA612B0661ECBD8E1B5B0BFE8C1F0B804BAB0C74283C4B3EF7B5B7E86636BE34`; the sampled-marker changes
  below were unbuilt. (Since built: see above.)
- **Build / run / measure:** helpers in `tools/perf/` (see its README). Build with
  `cmd /c tools\perf\build.cmd`; measure with `tools\perf\measure.ps1 -Name X -Seconds N`.
  Always run one emulator instance at a time; `measure.ps1` stops old ones and closes the game.
- **Frame rate now:** gameplay ~12–20 FPS depending on scene (no 60 FPS yet); intro/menus already
  reach 30/60 flips. Kept optimizations: fault-free register-table/label reads and SRT walker load
  emulation (−60% readbacks, FPS unchanged). Experiments left opt-in: relaxed compute barriers
  (causes black ice blotches; keep off), materialization memo.
- **Main finding on FPS:** the GPU is idle ~57% of the time; the CPU and GPU take turns because
  the emulator must read GPU-written descriptor/constant data on the CPU (synchronous readbacks).
  Early flush / async readback cannot help (reverted). Real lever: GPU-side descriptor resolution.
- **Open blocker:** the draw-only marker run reproduced the TDR and captured the first
  `VK_EXT_device_fault` address, but it stopped between marked draws rather than naming the
  offending command. See the incomplete-continuation section immediately below.
- **Resume next (then):** finish and stage the sampled-marker build, smoke-test mask 1 with direct-dispatch
  period 16 / phase 0, then run long enough to capture a device loss. 2026-11-04: built; a 240 s
  smoke test printed the sampling line and ran without device loss, but the 900 s capture
  (`sample16b`) sat in the menus — redo it with `-Autopilot`. Inspect the sampled bracket,
  reported device-fault address, and shader hash before starting the BDA/use-after-free audit.

## Incomplete continuation — draw-only capture and sampled markers (2026-11-03/04)

This work was interrupted after source edits and before a successful rebuild. Preserve the edits,
but do not treat them as validated.

### Draw-only marker result (old `DA612...` executable)

- The first two `measure.ps1` attempts (`drawmask4a` / `drawmask4b`) exited before the emulator
  started because the nested PowerShell environment string did not survive quoting; they produced
  no console/performance logs. The direct launch used for `drawmask4c` worked.
- `console-drawmask4c.log` and `perf-drawmask4c.log` are in the workspace root. With
  `KYTY_GPU_CHECKPOINTS=1`, `KYTY_GPU_CHECKPOINTS_MASK=4`, and relaxed barriers left off, the GPU
  lost the device around performance-log `t=43`.
- `VK_EXT_device_fault` returned `Incomplete` and one `InstructionPointerUnknown` address:
  `0x00000002004348d0`, precision `0x10`.
- Buffer markers reported `last started #327032`, `last completed #327032`, and
  `last recorded #327554`. The final four completed draw records were:
  `0xcebb4cc6d7b3c603` (51189 and 38526 indices), `0xf5eba2e9e45b53d4` (4), and
  `0x29513703a84c1b89` (6). Since started equals completed, no marked draw was in flight; the GPU
  stopped after draw 327032 and before the next observed draw marker.
- The fatal game breadcrumb was `worldMainTransparent`, with the last opcode a 2,904-index
  `DrawIndexedInstanced`. Earlier `sssConvHorizontal` watchdog reports in the same run recovered;
  this fatal occurrence was not localized to the prior SSS breadcrumb.

### Source edits made but not built

- `gpuCheckpoints.cpp`:
  - Added direct-dispatch marker sampling with
    `KYTY_GPU_CHECKPOINTS_DISPATCH_EVERY=N` and optional
    `KYTY_GPU_CHECKPOINTS_DISPATCH_PHASE=P` (`ordinal % N == P`). Defaults preserve the existing
    mark-every-dispatch behavior when direct-dispatch mask bit 1 is selected.
  - Added strict positive-period / in-range-phase parsing, startup diagnostics, compile-time
    predicate checks, and device-loss reporting of direct dispatches seen versus selected.
  - Extracted the checkpoint mask lookup and added up to 24
    `RECORDED, NOT OBSERVED STARTED` records after the last observed marker so sampled runs show
    the first commands on the far side of the stall.
- `gpuCheckpoints.h`: documented the two sampling environment variables.
- `renderCompute.cpp`: moved the direct-dispatch checkpoint around both the normal `dispatch` and
  the ordered-append `dispatchBase` loop. The latter previously bypassed direct checkpoints.
- `tools/perf/README.md`: documented marker mask bits and the new sampling controls.

### Incomplete validation / exact next commands

- A Release build was started with `tools\perf\build.cmd`, but no successful completion was
  observed and `_Build\windows-local\kyty_emulator.exe` was not updated. No new executable or PDB
  was staged. Treat compile status as unknown.
- The tracked-file `git diff --check` completed with only the existing LF-to-CRLF warnings; the
  checkpoint source/header and performance README are untracked, so the build remains the required
  validation.
- Next: rerun `cmd /c tools\perf\build.cmd`, fix any compiler error, confirm build/staged hashes,
  then launch with mask 1 plus
  `KYTY_GPU_CHECKPOINTS_DISPATCH_EVERY=16` and
  `KYTY_GPU_CHECKPOINTS_DISPATCH_PHASE=0`. Start with a short boot test, then a 600–900 second run.

## Session 2026-11-02 summary

State at the time: uncommitted on `nhl27pc`.

### Build environment note

The build directory had to be reconfigured. Build from a shell with
`vcvars64.bat`, `C:\Program Files\LLVM\bin` and `nhl27pc\.tools\glslang-16.6.0\bin` on `PATH`,
and pass `-DKYTY_BUILD_LAUNCHER=OFF` (Qt is not installed; only `kyty_emulator` is needed).
Running `cmake --build` from a plain shell triggers a failed regeneration that deletes
`CMakeFiles/rules.ninja`.

### New telemetry

- `KYTY_PERF_CSV=<file>` (or `run_nhl27.ps1 -PerfLog <file>`) appends one line per second with
  every nonzero counter rate, so runs can be analyzed without reading the overlay. It enables
  collection by itself. Counter names come from the `KYTY_DEBUG_COUNTERS` X-macro.
- `opXX=calls/ns` per PM4 type-3 opcode (inclusive), `rXX` per custom NOP sub-op
  (`r06` WaitFlipDone, `r17` Flip, `r1A` ContextState, ...), `rbopXX` synchronous readback
  time charged to the packet that caused it.
- Stage timers: `Cs*Ns` (dispatch: program, fast path, pipeline, buffers, images, commit, record),
  `Draw*Ns` (bindings, buffers, pipeline, targets, commit, record, programs),
  `Program{Key,Materialize,Match}Ns` and `ProgramLookups`.

~~The game boots straight through the intro into live CPU-vs-CPU gameplay with no input.~~
Superseded 2026-11-04: the game now stops at a "Sign in required" notice and later at Play Now →
Select Team. Use `measure.ps1 -Autopilot` (Play Now, Triangle = CPU vs CPU).

### Findings

1. Intro/menu phases already reach 30 or 60 flips/s; there the PM4 thread spends most of its time
   in `r06` WaitFlipDone (present back-pressure), i.e. it is idle, not overloaded.
2. In gameplay the PM4 thread is saturated (~950 ms/s). Representative per-second costs
   (≈46–53k draws/s, 18–23k dispatches/s):
   - Draw processing ≈ 480–515 ms/s (~10 µs/draw): state prep ~5 µs (program lookup ~4 µs,
     render-target resolve ~1 µs), binding preparation ~3.3 µs, commit/record ~1.3 µs.
   - Descriptor materialization (`MaterializeResources` on every program lookup) ≈ 237 ms/s for
     ~102k lookups/s (~2.3 µs each). **Largest single identified CPU item; next target.**
   - GPU waits ≈ 320–360 ms/s, mostly from synchronous readbacks (each `Wait(CurrentTick())`
     drains all recorded work).
   - Dispatch ≈ 210–230 ms/s.
   - Guest write faults ≈ 45k/s at ~10–25 µs on the game threads.
3. GPU SM activity was ~60–65% with ~3% memory activity at ~15 FPS: the GPU is busy but nearly
   idle in throughput, consistent with thousands of small dispatches serialized by barriers.
   This is why each readback wait costs ~1–2 ms.
4. Texture "uploads" of 1–8 GB/s are GPU-side buffer→image detile copies, not PCIe traffic; not
   a priority.

### Experiment: relaxed compute barriers (opt-in, kept)

`KYTY_RELAXED_CS_BARRIERS=1` (or `run_nhl27.ps1 -RelaxedComputeBarriers`). Previously every
dispatch recorded a full `compute → ALL_COMMANDS` barrier afterwards (and a hazard barrier before
writers), while guest sync packets were no-ops. Now, for graphics-queue dispatches that use
neither LDS storage nor GDS:

- the post-dispatch barrier becomes pending on the `CommandBuffer`;
- it is dropped only if the next recording is another such dispatch with no guest sync packet in
  between (then the hazard barrier is also skipped if only dispatches ran since the last one);
- any guest sync packet (`EVENT_WRITE`, `ACQUIRE_MEM`, `RELEASE_MEM`, `WAIT_REG_MEM[_64]`,
  `MEM_SEMAPHORE`, `WRITE_DATA`, `COPY_DATA`, `DMA_DATA`, `COND_EXEC`, `SET_PREDICATION`) or any
  other command recording (`CommandBuffer::Handle()`, including `End()`) records it first.

Result: ~40–75% of dispatch→dispatch barriers elided; two runs (~5 and ~4.5 min, intro and
gameplay) without visible corruption or crash. Per-operation, dispatch cost fell from ~11.9 to
~9.1 µs and readback time from ~258 to ~173 ms/s in gameplay. The headline FPS difference
between runs (11.5 vs 20.8) is **not** a fair comparison: the baseline segment was a heavier
faceoff scene (~4,650 vs ~2,200 draws/frame). Keep it opt-in until an A/B in the same scene
and more stability time.

### Hang reproduced (baseline configuration)

Run 4 (relaxed **off**) froze after ~50 s of gameplay at a faceoff. PM4 activity went to zero and
the game's own watchdog printed an "RC2 BreadCrumbs report — GPU Stopped Working": frame 2 on the
Main queue completed `DeferredGpuProbes` and stopped before `sssConvHorizontal`; Async queue
completed. Most likely the graphics PM4 stream is parked on a `WAIT_REG_MEM` (or memory
semaphore) whose label never gets written. Full log: `run4-gpu-hang-console.log` in the workspace
root. Next step for triage: log the address/reference of the suspended wait when PM4 makes no
progress for >1 s.

### Experiment: materialization memo (opt-in, not a win)

`KYTY_MATERIALIZE_CACHE=1`. For plans with only direct descriptors (no indirect descriptors,
captured reads, selector keys or indirect args), the walker runs and, if its scratch, flat reads
and shader base equal the previous successful refresh, the post-walk refresh is skipped and only
`snapshot.user_data` is updated. Counters `MaterializeHits/Misses` and `Miss*` classify misses.

Gameplay result (run 6): 66k lookups/s, 45k eligible, **only ~16% hits**. Of the misses, the
flat read values differ in ~89% and the descriptor words differ in ~72% (~32% only in DWORDs 0–1,
but for buffers DWORD 1 also holds the stride, so "base only" isn't proven). Walker-internal-only
differences account for ~8%. Per-draw constant buffers move every draw, so exact-input
memoization can't pay. Misses cost an extra walk, so it stays off by default.

A faster design would patch only the changed address fields into the cached snapshot and reuse
the specialization when every specialization-relevant field (buffer stride/format/swizzle/OOB,
image format/type/dims, sampler words) is unchanged. That needs exact per-type field masks.

### Hang reproduced twice at the same pass

Run 6 (relaxed barriers **on**) hung at ~150 s with the identical report (`sssConvHorizontal`,
Main queue, after `DeferredGpuProbes`), right after 25–27 FPS gameplay. So it's independent of
the relaxed-barrier experiment and fairly reproducible (2 of the last 3 gameplay runs).
Log: `run6-gpu-hang-console.log`.

### Hang root cause: host GPU TDR (runs 7–12)

- During a fatal hang every emulator thread is idle and the game stops submitting. The new
  event-queue stall reporter shows a guest thread blocked >5 s on `GfxEopQueue` (graphics events
  ident 0x0 and 0x20, never triggered): it is waiting for an end-of-pipe interrupt.
- That interrupt is a deferred priority operation that waits for the GPU tick, and the tick never
  completes: `waitSemaphores` returns device-lost (`masterSemaphore.cpp`), and Windows logs
  `nvlddmkm` event 153 (GPU timeout/reset) at the same time.
- Every fatal hang (runs 4, 6, 7, 9, 11, 12) has the same game report: last pipeline
  `ShaderProgram_SubSurfaceScatteringGenerateTilesCS`, then a `CopySubresource`, entering
  `sssConvHorizontal`. It reproduces **with relaxed barriers on and off**, and can happen from
  ~20 s to ~10 min in.
- Watchdog reports alone are not fatal: run 8 printed one during a shader-compile burst
  (`hairResolve`) and recovered.
- `VK_NV_device_diagnostic_checkpoints` (`KYTY_GPU_CHECKPOINTS=1`, `gpuCheckpoints.cpp`) did not
  help: after this TDR the driver returns markers that were never recorded.

Open question: is the subsurface-scattering work an infinite loop, or just a submission that
exceeds Windows' 2 s TDR limit? Raising `TdrDelay` (a system registry setting, user's call) or
splitting long submissions would tell them apart. Next: find that shader's host hash (log
dispatches between the `DeferredGpuProbes` and `sssConvHorizontal` markers via the guest's
`R_PUSH_MARKER` packets) and inspect its loops, atomics and indirect arguments.

### Ice corruption with relaxed barriers

With `-RelaxedComputeBarriers`, gameplay later in the 1st period showed black speckles and large
black blotches on the ice that change from frame to frame, a typical synchronization race. With
relaxed barriers off, the early-period frames I sampled were clean, but run 12 hung before the
same game time, so the A/B is not conclusive. **Keep relaxed barriers off** (the default) until
this is resolved.

### Frame-rate work, late session (relaxed barriers off)

New tools (all opt-in):

- `KYTY_SAMPLE_GPU=delay,duration`: built-in stack sampler for the GPU/PM4 thread (no admin),
  writes `thread-samples.txt` next to the executable with exclusive/inclusive functions and
  "fault sites" (the function that touched protected memory and its callers).
- `KYTY_AB_PERIOD=N`: flips the optimizations under test every N seconds within one run and tags
  each logged second `ab=0/1`; compare windows with an A/B script. Separate runs drift too much
  (24k vs 48k draws/s in the "same" window) for FPS comparisons.
- `KYTY_GPU_TIMING=1`: timestamps around each submitted command buffer → `GpuBusyNs`, `GpuGapNs`.
- `ProtectCalls/ProtectNs`, `Bind*Ns` binding-stage timers, `WalkerLoadsEmulated`.
- `nsys-capture.ps1` (workspace root, run elevated): Nsight CPU sampling + Vulkan trace.

Findings on the GPU thread (sampler):

- ~37% of samples were synchronous readbacks triggered by page faults. The biggest source,
  `SET_SH_REG_INDIRECT` register tables (15.7%), and `WAIT_REG_MEM` labels (2.3%) were false
  sharing: CPU-written data on pages that also hold GPU-written bytes.
- Fixes (kept, default on): register tables (SH/CX/UC) and wait labels read through
  `TryReadBufferBacking` (byte-precise dirty check). SRT walker loads (`mov eax,[r10]`) are
  completed from the backing store in the exception handler (`CompleteLoad`) instead of
  unprotecting the page. Fault handling fell from 34% to 6% of the GPU thread; readback calls
  166 → 66/s (in-run A/B, 72 s per variant).
- **FPS did not change measurably** (12.5 ± 1.0 vs 11.7 ± 1.0). The remaining readbacks are
  reads of genuinely GPU-written SRT/descriptor data, and each still drains the GPU.
- GPU timestamps in gameplay: GPU busy ~432 ms/s, idle between submissions ~567 ms/s, CPU
  waiting on GPU ~334 ms/s. **Not GPU-bound**; CPU and GPU take turns (record → submit → wait).
- Per draw (gameplay): BDA sync 88 ms/s, almost entirely `VirtualProtect` (65k calls/s), caused
  by 48k guest write faults/s (~560 ms/s across game threads). Telemetry timers themselves cost
  ~5% of the GPU thread (`QueryPerformanceCounter`).
- Nsight (elevated): `vkWaitSemaphores` 339 ms/s (median 0.26 ms, max 55 ms);
  `vkAllocateMemory`/`vkFreeMemory` ~100/s each at 0.1–0.2 ms. Guest threads are unnamed and
  unsymbolized, so their samples were not actionable.

### Experiment: predictive early flush (reverted)

Learned 64 KiB granules that produced synchronous readbacks; when a draw/dispatch bound a GPU
write over a hot granule, the PM4 loop submitted right after that packet. In-run A/B (5 s
toggle, ~24 s per variant before the GPU hang): submissions 1,139 → 5,611/s, FPS 19.7 ± 2.7 →
20.4 ± 2.5, CPU GPU-wait 295 → 301 ms/s, GPU idle gaps 532 → 454 ms/s. No measurable gain.
The readback waits are not spent on unrelated queued work: the game's GPU produces
descriptor/constant data that the next draw needs decoded on the CPU, so the wait is for the
producer itself. Async copy-out cannot shorten that, so stage 2 was not built and stage 1 was
reverted.

The lever left on this path is architectural: resolve those GPU-produced descriptors on the GPU
(bindless/indirect descriptor access in the recompiled shaders) instead of materializing them
on the CPU.

### GPU hang investigation (device loss after SSS tile generation)

Tools added:

- GPU buffer markers (`gpuCheckpoints.cpp`, `VK_AMD_buffer_marker`, host-coherent system
  memory, survives the TDR). On by default for indirect dispatches and the emulator's own copies,
  clears and internal dispatches (`GpuCheckpointScope` at each call site). After device loss the
  console prints "last started / last completed" and the records in flight.
  `KYTY_GPU_CHECKPOINTS=1` marks all dispatches and draws (`KYTY_GPU_CHECKPOINTS_MASK`: 1 direct
  dispatch, 2 indirect, 4 draw, 8 emulator work). `KYTY_INDIRECT_MARKERS=0` turns markers off.
- `VK_EXT_device_fault`: after device loss, prints the driver's fault description/addresses
  (enabled with the markers; not yet captured on a hang).
- `KYTY_DIAG_INDIRECT=1`: logs indirect-dispatch group counts (implausible or over limit).

Findings:

- Indirect dispatch arguments were sane up to the hang (no over-limit or huge counts).
  `DS_ORDERED_COUNT` is a plain per-wave atomic, so no ordered-append spinlock.
- Guest PM4 markers do not carry pass names (only `flip`, `Signal`, …).
- Copy markers localized one hang (run mark8a): the last started and completed command was a
  texture upload (`image.cpp` `copyBufferToImage` after the tiler's fill + detile dispatch), and 35
  later copy markers were never started. So the GPU stopped on an unmarked game draw/dispatch
  recorded right after a texture upload.
- Markers on every direct compute dispatch (masks 1/3/5/9, or all) kept the game running 7–9 min
  in each of 6 runs; without them most runs lose the device within 40–250 s (control: 61 s).
  Markers on indirect dispatches only, or on copies only, still hung (243 s, 142 s).
- Full barriers (all commands → all commands) around indirect dispatches did **not** prevent it
  (one clean run, then a hang at 96 s). So the extra marker serialization changes timing rather
  than fixing a missing memory dependency, or the hazard involves the GPU front end.
- The run-to-run hang time is very random (one mask-8 run went 9 min clean), so single clean runs
  prove little.

Next: catch one hang with markers on draws only (mask 4) and on direct dispatches with a
reduced rate, to name the stalled command; read the `VK_EXT_device_fault` report; and check
whether the stalled shader uses buffer-device-address (raw pointer) access, which can fault when
the buffer cache frees memory (~100 `vkFreeMemory`/s) still referenced by in-flight work.

### Recommended next steps

1. GPU-side descriptor resolution for GPU-produced SRT data (see above); large change.
2. Keep pages the guest rewrites every frame unprotected and sync them per submission, to cut
   the write-fault/`VirtualProtect` ping-pong (needs a correctness argument about writes during
   PM4 processing).
3. The reproducible GPU hang (below) still blocks long runs.
4. Debug the reproducible hang first: it blocks long A/B runs. Log the suspended PM4 packet
   (opcode, label address, reference, current value, queue) when no progress is made for >1 s.
2. Incremental descriptor patching (see above) if materialization is still the top CPU item.
3. Same-scene A/B of relaxed compute barriers, then consider making it default.
4. Reduce remaining readback stalls (`WAIT_REG_MEM` labels `rbop93`, dispatch `rbop15`,
   opcode 0x63) — e.g. wait only for the tick that produced the dirty range.

## Objective

Run NHL 27 gameplay reliably at 60 FPS through the NHL27PC/Kyty fork on this machine, while
preserving correct rendering and game behavior.

This is still an active bring-up and performance effort. The game reaches live gameplay, but the
60 FPS target has not yet been reached and the intermittent pre-match crash is not considered
fixed.

## End-of-day state

- Branch: `nhl27pc`
- Base commit: `bf57344`
- Working tree: modified and intentionally uncommitted
- Configuration: Release, clang-cl, Ninja
- Build directory: `nhl27pc/_Build/windows-local`
- Staged runtime: `KytyPS5-2026-11-01-telemetry-Windows-x64`
- Game data: `PPSA34063-app0`
- Emulator was closed cleanly after the final test.
- The build and staged executables match.
- SHA-256: `3440DCA90BC6A4593A5D5270DB71A03C044E24EAE2CB111AE4929CDB0DF96920`

## Test system

- CPU: AMD Ryzen 7 9800X3D, 8 cores / 16 threads
- GPU: NVIDIA GeForce RTX 5090, 32 GB
- System RAM: 64 GB
- Test render size: 1280x720
- Present mode: Mailbox at 60 Hz
- Shader cache: enabled and warm
- Shader optimization type: `None`
- Implicit Vulkan layers: disabled with `VK_LOADER_LAYERS_DISABLE=~implicit~`
- Guest printf: `Silent` in the optimized preset

The low total CPU percentage is misleading: the graphics/PM4 path is substantially serialized and
one critical thread remains saturated.

## Retained improvements

### 1. Silent guest logging

Guest file logging was writing an approximately 118 MB log at up to roughly 43 MB/s during the
test. Switching the optimized launcher to silent guest printf raised the heavy intro from about
5–7 FPS to roughly 13–14 FPS.

`tools/run_nhl27.ps1 -Optimized` now selects `--printf-direction Silent` and also applies the warm
shader cache, 720p Mailbox presentation, and implicit-layer filter.

### 2. Safe image and sampler descriptor reuse

Image and sampler bindings are reused when the cached image is still registered and does not need
rebinding. Buffer descriptor reuse is deliberately excluded because the attempted shortcut was
not safe.

Observed reuse varies by scene:

- Images: approximately 27–58%
- Samplers: approximately 47–70%

This reduced draw/bind setup cost. A representative earlier gameplay sample reached 19.4 FPS with
draw processing around 12.6 microseconds per call, binding around 5.4 microseconds per call, and
dispatch processing around 16.4 microseconds per call.

### 3. Detailed performance overlay

The overlay now reports:

- PM4 processing and decode/execute cost
- Draw, state preparation, binding, and dispatch cost
- Vulkan queue submissions and command-buffer flushes
- AGC helper/submission activity
- Page-fault access type and handler stages
- Readback call count, queueing, dirty/download counts, and timing
- Descriptor reuse rates
- Shader/pipeline compile activity
- Buffer/texture transfer rates

Overlay controls:

- `F2`: hide/show
- `Shift+F2`: switch compact/detailed mode

The detailed timers collect only while the overlay is visible. When hidden, the retained
instrumentation has only the inexpensive enabled check on these paths.

### 4. Wider buffer-readback coalescing window

`DownloadWindowSize` is now 2 MiB instead of 512 KiB. The downloader still copies only dirty byte
ranges inside the window; the wider range primarily coalesces nearby GPU-written islands so they
share fewer synchronous Vulkan completions.

The final test showed a useful reduction:

| Metric | 512 KiB gameplay sample | 2 MiB final gameplay sample |
| --- | ---: | ---: |
| Observed FPS | 11.3 | 18.4 |
| Readback calls | 209/s | 104/s |
| Dirty/download calls | 209/s | 104/s |
| Readback body time | 495 ms/s | 404 ms/s |
| Download time | 494 ms/s | 403 ms/s |
| Average readback | 2.37 ms | 3.90 ms |
| Buffer downloads | 19.9 MB/s | 97.7 MB/s |
| PM4 processing | 952 ms/s | 950 ms/s |

The FPS values are not a strict apples-to-apples comparison because the camera and draw count per
frame differed. The internal readback result is still meaningful: call frequency was approximately
halved and total synchronous readback blocking fell by about 90 ms per second. The cost per call and
transfer bandwidth increased as expected. Keep 2 MiB as the current baseline, but confirm it in a
repeatable scene before considering 4 MiB or a dynamic policy.

## Current bottleneck analysis

### Serial PM4/graphics execution is the primary limit

Across gameplay samples, PM4 processing remains near 950 ms per wall-clock second. That is the
dominant barrier after the logging, descriptor, and readback improvements.

A representative 512 KiB gameplay sample before the final readback change showed:

- 12.4 FPS / 80.8 ms per frame
- 2,748 draws per frame, approximately 34,028 draws/s
- 741 dispatches per frame, approximately 9,170 dispatches/s
- PM4: approximately 975 ms/s
- Draw processing: 424 ms/s, 12.5 microseconds/call
- State preparation: 213 ms/s, 6.3 microseconds/call
- Binding: 197 ms/s, 5.8 microseconds/call
- Dispatch processing: 479 ms/s, 49 microseconds/call
- Faults: 22,422/s, 518 ms/s, 23.1 microseconds average

AGC native helpers already generate roughly 90–100% of command DWORDs. Replacing only the command
builder/parser is therefore unlikely to recover enough time; the next work should target native
hot paths or batching inside PM4 execution, draw/dispatch state handling, and synchronization.

### Fault/readback result

The detailed breakdown established that Windows exception delivery itself is not the main cost.
The expensive part is the buffer readback path:

- Most readbacks in the profiled scenes execute inline on the graphics thread.
- Almost every read fault represented real GPU-owned data and caused a real download.
- Queue wait was negligible in the clean samples.
- The synchronous Vulkan completion/download dominated the readback body.

The 2 MiB window is a safe incremental improvement. Longer term, predictive/asynchronous
readback at an ordering boundary may be more valuable than widening the window indefinitely, but
it must preserve CPU visibility and ownership semantics.

### Shader compilation is not the steady-state bottleneck

With the warm cache, menu/arena runs commonly showed zero or near-zero shader and pipeline
compiles. Compilation spikes can still affect first-run transitions, but they do not explain the
steady gameplay frame rate.

### GPU hardware is not saturated

The RTX 5090 is not the limiting resource in these samples. Host CPU serialization, command
translation/state work, page protection, and synchronization prevent the GPU from being fed fast
enough.

## Experiments that were reverted

Do not reintroduce these without a redesigned correctness argument and focused A/B test.

### Unsafe buffer descriptor reuse

A buffer `BufferId` reuse shortcut reached about 33% buffer reuse and improved some intro costs,
but it crashed in the arena. Buffer alias/owner selection is not stable enough for this shortcut.
Only image and sampler reuse remains.

### 64 KiB pre-invalidation around AGC submissions

This did not improve performance and caused device-loss behavior. It was fully reverted.

### 64 KiB page-fault coalescing

This reduced the fault count but triggered roughly 1.4–2.5 GB/s of texture uploads and made total
time/FPS worse. It was fully reverted.

## Intermittent crash status

Some safe builds reach gameplay; other launches can still exit during the intro or before the
match. This is intermittent and was also seen after the unsafe buffer-reuse experiment was removed,
so it should not be attributed solely to that experiment.

Windows Application Event Log did not provide a useful hard-error record in the inspected run.
Silent optimized launches intentionally do not generate the very large guest printf log. A future
crash-focused run may need a selectively filtered log or explicit device-loss/crash markers rather
than restoring all guest printf output.

## Files currently modified

- `src/common/debugCounters.h`
  - Added performance counters and scoped timing support.
- `src/graphics/guest_gpu/graphicsRun.cpp`
  - PM4/guest-GPU telemetry.
- `src/graphics/host_gpu/renderer/cache/bufferCache.cpp`
  - 2 MiB readback window and detailed readback/fault timing.
- `src/graphics/host_gpu/renderer/commandScheduler.cpp`
  - Queue/flush/wait telemetry.
- `src/graphics/host_gpu/renderer/pipeline/descriptors.cpp`
- `src/graphics/host_gpu/renderer/pipeline/descriptors.h`
  - Safe image/sampler descriptor reuse and counters.
- `src/graphics/host_gpu/renderer/renderCompute.cpp`
- `src/graphics/host_gpu/renderer/renderDraw.cpp`
  - Dispatch/draw-stage timing.
- `src/graphics/host_gpu/renderer/renderContext.cpp`
  - Fault access and handler-stage timing.
- `src/graphics/presentation/performanceOverlay.cpp`
  - Detailed diagnostic rows.
- `src/libs/agc.cpp`
  - AGC helper/submission counters.
- `src/loader/runtimeLinker.cpp`
  - Guest I/O/import telemetry.
- `tools/run_nhl27.ps1`
  - Optimized NHL 27 launch preset.

## Build and launch

From `nhl27pc`:

```powershell
cmake --build _Build\windows-local --config Release --target kyty_emulator -j 16
```

Stage the rebuilt executable from the workspace root:

```powershell
Copy-Item -LiteralPath 'nhl27pc\_Build\windows-local\kyty_emulator.exe' `
    -Destination 'KytyPS5-2026-11-01-telemetry-Windows-x64\kyty_emulator.exe' -Force
```

Launch through the helper from `nhl27pc`:

```powershell
.\tools\run_nhl27.ps1 `
    -GameDirectory '..\PPSA34063-app0' `
    -KytyDirectory '..\KytyPS5-2026-11-01-telemetry-Windows-x64' `
    -Optimized `
    -EmulatorArguments '--perf-overlay'
```

Equivalent effective emulator options:

```text
--game PPSA34063-app0
--no-splash
--printf-direction Silent
--shader-cache
--shader-optimization-type None
--screen-width 1280
--screen-height 720
--present-mode Mailbox
--perf-overlay
```

## Validation completed

- Release build succeeds.
- `git diff --check` passes; PowerShell reports only the existing LF-to-CRLF warning for the launch
  script.
- The staged executable hash matches the build output.
- Safe image/sampler-only descriptor reuse reaches live gameplay.
- The 2 MiB readback window reaches live gameplay without a fault in the final test.
- No emulator process was left running.

## Recommended next session

1. Re-run the 2 MiB build in the same matchup and capture two or three steady gameplay samples at
   comparable camera/draw counts. Treat the readback counters as the main A/B evidence, not one FPS
   snapshot.
2. Add a deeper PM4 breakdown by opcode/category and identify the largest repeated interpreter or
   state-validation blocks inside the approximately 950 ms/s serial path.
3. Target a native/batched hot path with a narrow correctness boundary. Good candidates are repeated
   draw-state packets, redundant state emission, and dispatch setup; a wholesale PS5-library swap is
   too broad for the next step.
4. Keep the readback window at 2 MiB initially. Test 4 MiB or an adaptive policy only after measuring
   call reduction against download bandwidth and total readback time in the same scene.
5. Continue crash triage separately from performance work. Add concise crash/device-loss markers so
   a failing optimized run remains diagnosable without restoring high-volume guest logging.

The immediate 60 FPS strategy is therefore: reduce serial PM4 work first, then revisit predictive
readback and remaining draw/dispatch state overhead. The current data does not support spending time
on lower resolution or a faster GPU.
