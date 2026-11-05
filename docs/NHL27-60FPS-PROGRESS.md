# NHL 27 PC — 60 FPS progress handoff

Last updated: 2026-11-05 (Europe/Berlin)

## FPS work, 2026-11-05 late (newest; the crash hunt below is parked at 1 loss in 22 runs)

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
