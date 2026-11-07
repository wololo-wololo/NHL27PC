# Performance and hang-debugging helpers

Local helpers from the 2026-11-02 session. Paths are hard-coded for this machine
(`C:\Users\Administrator\Downloads\NHL.27.PS5-DUPLEX`); adjust them if the workspace moves.

| File | Use |
| --- | --- |
| `build.cmd` | Builds `kyty_emulator` with VS 2022 Build Tools, LLVM clang-cl and the bundled glslang on `PATH`, launcher disabled (no Qt). Run from `cmd` or `cmd /c tools\perf\build.cmd`. |
| `measure.ps1 -Name X [-Seconds N] [-Sample 'delay,dur'] [-Env '$env:VAR=''1'';']` | One measured run: stops any running emulator, stages the build (exe + pdb), runs the optimized preset with `perf-X.log`, closes the game after N seconds. Writes `console-X.log` and, with `-Sample`, `thread-samples-X.txt` in the workspace root. |
| `measure.ps1 ... -Autopilot` | Same, but drives the menus to Play Now CPU-vs-CPU gameplay first (see below); `-Seconds` then counts gameplay time. Screens seen are saved under `autopilot-shots\<Name>\` in the workspace root. |
| `measure.ps1 ... -Autopilot -SkipIntro 8` | Taps Cross 8× (1.5 s apart) once the arena renders, skipping the pre-game presentation to the opening faceoff. Every run is also watched by `hangwatch.ps1`; the final line reports `hangwatch: …`. |
| `hangwatch.ps1 -Name X [-Kill]` | Waits for a PM4 hang in `perf-X.log` (8 idle seconds), then writes all thread stacks with lldb to `hang-threads-X.txt` (UTF-16; read with `iconv -f UTF-16LE`) and, with `-Kill`, closes the emulator. |
| `hang-repro.ps1 [-Prefix p] [-Runs n] [-Seconds s] [-StopOnHang]` | Repeats autopilot runs with the caller's environment; prints device losses and hangs per run and in total (stability checks). |
| `autopilot.ps1 [-PerfLog perf.log]` | Boot → sign-in notice (Cross) → main menu, Play Now (Cross) → Select Team (**Triangle** = CPU vs CPU) → team/jersey confirmations (Cross ×4) → Start Game (Cross) → waits for in-arena rendering (10 s at ≥1500 draws/flip). Reacts as soon as each screen appears (~60 s launch → arena with a warm shader cache). `-Identify` names the current screen and saves `peek.png`; `-Record NAME` stores the current prompt bar as reference `NAME` in `nav-refs.txt`. |
| `hunt-loop.ps1 [-Prefix huntN]` | Device-loss capture loop with the localizing diagnostics (start-only markers, completion every 8th, indirect-args log); stops after 2 captures. |
| `verify-loop.ps1 [-Prefix fixN] [-Runs 10]` | Default-config runs; prints device losses per run and the total (fix verification). |
| `pad.ps1 CMD...` | Sends commands to the emulator input pipe, e.g. `pad.ps1 'tap triangle' 'wait 1500' 'tap cross' 'shot C:\tmp\a.bmp'`. |
| `capture.ps1 [-Out file.png]` | Host-side window capture with `PrintWindow` (unfocused or covered, **not** minimized; includes the overlay). Prefer `pad.ps1 'shot …'`. |
| `screen.ps1` | Shared capture/prompt-bar signature helpers (dot-sourced). |
| `waithang.ps1 -Log perf.log` | Waits until PM4 has been idle for 8 s after gameplay (fatal hang) or the emulator exits. |
| `perfsum.py LOG [keys]` | One compact row per logged second. |
| `avg.py LOG START END KEYS…` | Averages counters over a time window (`*Ns` printed as ms/s). |
| `keys.py LOG START END KEYS…` | Raw per-second values. |
| `abstat.py LOG START END [KEYS…]` | Compares `ab=0` vs `ab=1` seconds of an in-run A/B (`KYTY_AB_PERIOD`). |
| `slowfast.py START END SLOW_MAX FAST_MIN LOG…` | Compares counters between slow and fast seconds (by `FlipsRequested`) across logs. |
| `cmpgroups.py START END A_LOG[,…] B_LOG[,…] [TOP]` | Mean per-second counters of two groups of runs over a window, sorted by difference. |
| `phasecost.py LOG NAME:START-END…` | Per-frame and per-draw cost of each Ns counter for named time windows of one run. |
| `timeline.py LOG START END` | Second-by-second transition timeline (flips, draws/frame, PM4, compiles, waits, uploads). |
| `intro.py LOG…` | Per run: FPS, worst second, draws per frame and µs per draw for the pre-game intro (t 70–106), its >4,500-draw seconds and early play (t 115–128). |
| `rbattr.py LOG START END` | Readback time by PM4 opcode (`rbopXX`). |
| `nsys-capture.ps1` | Nsight Systems CPU sampling + Vulkan trace. Needs an elevated PowerShell (the user approved this). |
| `gpu_workload.py DB`, `cpu_threads.py DB [N]` | Summaries of an exported Nsight `.sqlite`. |

Scripted input and screenshots (no window focus needed, works minimized):

- `KYTY_INPUT_PIPE=1` (or a pipe name) makes the emulator listen on `\\.\pipe\kyty-input`; one
  command per line, one reply line each (`ok`, `pong`, `err …`). Commands: `ping`,
  `tap <button> [ms]`, `down <button>`, `up <button>`, `axis <LeftX|LeftY|RightX|RightY|L2|R2> <0-255>`,
  `release`, `shot <path>` (next guest flip as 24-bit BMP at game resolution, without host
  overlays). Buttons: cross circle square triangle up down left right l1 r1 l2 r2 l3 r3 options
  touchpad. Input goes to the keyboard player. Source: `src/graphics/presentation/window/inputPipe.cpp`,
  screenshot readback in `swapchain.cpp` (`CaptureScreenshot`).
- Screens are recognized by the near-white button-prompt text at the bottom of NHL 27's menus,
  which is static while backgrounds animate. If a game update changes a menu, re-record its
  reference with `autopilot.ps1 -Record <name>` while that screen is shown.

SRT walker (descriptor materialization JIT, `SrtWalker.cpp`):

- `KYTY_WALK_STATS=<s>[,<start s>]`: that long after the first walk, print once the plans with
  the most walker time (calls, ns/call, warm repeat, repeat rates, flat reads, code bytes); with
  `<start s>`, walks before that point are not counted (e.g. `95,70` covers the pre-game intro).
- `KYTY_WALK_DUMP=<bytes>` / `KYTY_WALK_DUMP_HASH=<hex>,…`: node composition of big walkers /
  of those hashes (the latter also writes `walker-<hash>-<stage>.bin`).
- `KYTY_WALKER_VERIFY=1`: run a reference walker (no inlining) beside every walk and print
  mismatches (slow; correctness check after walker changes).
- `KYTY_WALKER_INLINE_READS=0`, `KYTY_WALKER_PREFETCH=0`: disable the 2026-11-06 walker
  optimizations; `KYTY_WALKER_FAST_MARKS=0` the 2026-11-07 single-store reachability marks,
  `KYTY_WALKER_NEST=0` nested single-use thunks, `KYTY_WALKER_PREFETCH_PARTIAL=0` the first-32
  prefetches of walkers that need more (then none, as before).
- `KYTY_PRESENT_QUEUE=0`: present on the main queue (default: a separate queue).


Texture-cache lookup memos (2026-11-07 part 12, on by default, follow `KYTY_AB_PERIOD`):

- `KYTY_TEXTURE_MEMO=0` disables the `ResolveTexture` memo and the render-target/depth image
  reuse. `KYTY_TEXTURE_MEMO_VERIFY=1` runs the full lookup beside every memo hit and counts
  differences (`TextureMemoMismatches`, `Texture memo MISMATCH` / `Target memo MISMATCH` lines).
  Counters `TextureMemoHits/Misses`, `TargetMemoHits/Misses`.

GPU time per shader:

- `KYTY_GPU_PROFILE=delay,duration` (seconds after start; `1` = 0,3600) writes
  `gpu-profile.txt` next to the emulator every 5 s: GPU time per (kind, shader hash) for draws,
  dispatches (`-acb` = guest async-compute queue) and marked emulator work (by source line).
  Bottom-of-pipe timestamps before/after each command; they serialize the queue, so use the
  shares for attribution, not for FPS. Source: `src/graphics/host_gpu/renderer/gpuProfiler.cpp`.
  With `-Autopilot`, gameplay starts ~60–70 s after launch, so use e.g. `75,45`.
- `KYTY_DUMP_SHADERS=hash[,hash…]` (hex, as in the profile) writes the decoded RDNA2 and the IR
  of those shaders to `shader-<hash>-<stage>.txt`, and each compiled variant's SPIR-V
  disassembly to `shader-<hash>-<stage>-<id>.spvasm`, next to the emulator.
- `KYTY_TRACE_AFTER=hash[,hash…]` prints, for dispatches 201–240 of each listed shader, the next
  24 PM4 packets (opcode + first dwords) and following dispatches to the console log — shows
  whether the guest synchronizes after a dispatch.
- Experiment (off by default, unstable): `KYTY_CHAIN_SINGLE_GROUP=1` drops the emulator's
  barrier between consecutive single-workgroup dispatches without guest sync; optional
  `KYTY_CHAIN_SHADERS=hash,…` allowlist; follows `KYTY_AB_PERIOD`.

Device-loss diagnostics (2026-11-05; see the progress doc):

- `KYTY_GPU_CHECKPOINTS_TOP_ONLY=1`: draws and direct dispatches get only the top-of-pipe
  "started" marker (bottom-of-pipe markers serialize the queue and hide NHL 27's hang).
- `KYTY_GPU_CHECKPOINTS_BOTTOM_EVERY=N`: keep the completion marker on every Nth of those.
- `KYTY_GPU_LOG_INDIRECT=1`: before each indirect draw the GPU copies its first argument record
  into device-loss-safe memory; the report prints the last 16 with the emulator's view at record
  time (`gpu_dirty`, `cpu_dirty`, CPU backing).
- Dispatch records print the first written (`address=`) and read-only (`read=`) buffer base.
- The console always lists the first 200 buffers bound at guest bases not 4-byte aligned.
- Proven repro config: `KYTY_GPU_CHECKPOINTS=1`, `_MASK=15`, `_TOP_ONLY=1`, `_BOTTOM_EVERY=8`,
  `KYTY_GPU_LOG_INDIRECT=1`, `measure.ps1 -Autopilot -Seconds 150`, retried until a device loss
  (usually within 2–5 runs).

GPU-hang marker controls:

- `KYTY_GPU_CHECKPOINTS=1` enables the explicitly selected marker kinds;
  `KYTY_GPU_CHECKPOINTS_MASK` uses bits 1=direct dispatch, 2=indirect dispatch, 4=draw,
  and 8=emulator copy/internal work.
- `KYTY_GPU_CHECKPOINTS_DISPATCH_EVERY=N` with optional
  `KYTY_GPU_CHECKPOINTS_DISPATCH_PHASE=P` marks only direct-dispatch ordinals where
  `ordinal % N == P`. This reduces the timing perturbation from a marker after every dispatch;
  other selected marker kinds are unchanged.
