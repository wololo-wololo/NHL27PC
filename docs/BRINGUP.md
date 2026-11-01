# NHL 27 bring-up log

This document records reproducible compatibility observations without including game files,
decrypted code, shader caches, or user-specific paths.

## Public work to build on

- [KytyPS5 issue #1892](https://github.com/KytyPS5/KytyPS5/issues/1892) reports the main menu and
  quick-play loading. Its first-time setup notes say to select Headphones for audio.
- [KytyPS5 PR #1903](https://github.com/KytyPS5/KytyPS5/pull/1903) recognizes the legacy LS shader
  resource registers used before gameplay. Its author reports controllable gameplay at about
  13 FPS on an RTX 5070 Ti. It does not implement hardware tessellation.
- [AnyPS5 discussion #4275](https://github.com/boykopovar/AnyPS5/discussions/4275) reaches team
  selection and match loading at roughly 1.3–2 FPS and documents compute-shader timeouts,
  framebuffer problems, and missing stencil-plane sampling.
- [nhl27-ps5-extractor](https://github.com/lerrel129/nhl27-ps5-extractor) contains read-only
  Frostbite TOC/CAS, roster, and texture analysis tools plus prior AnyPS5 relinker research.

## 2026-11-01 local baseline

- Title: NHL 27
- Title ID: `PPSA34063`
- Tested content version: `01.003.001`
- KytyPS5 revision: `893b03552699dc873474023bc1f4c69c57fb36b2`
- Host: Windows x64, Vulkan
- Result: boots into Frostbite rendering, then the GPU stops; not playable on this host

Launch shape:

```text
kyty_emulator --game <PPSA34063-app0> --no-splash \
  --printf-direction File --printf-output-file nhl27-guest.log
```

Observed progression:

1. Core services, controller, audio, Vulkan, and the title metadata initialize.
2. The runtime enters Frostbite and begins compiling vertex/pixel/compute shaders.
3. AMD RC2 breadcrumbs report a stopped GPU in the `mainOpaque` render pass.
4. The last reported pipeline is
   `ShaderProgram_DeferredLightTile_LightingCS_ClearTile`.
5. The last reported opcode is an indirect dispatch at byte offset 16 in its argument resource.

Other observations:

- One unresolved `libkernel_module_info_v1` import reaches a stub but is not the immediate stop.
- Frostbite asks `/download0` and `/temp0` to resolve asset-like keys containing characters that
  Windows cannot represent in filenames. Kyty logs and rejects these names. A virtual filename
  encoding layer may be needed, but this did not cause the observed GPU stop.
- The first captured run reused an existing shader cache. A clean-cache capture is required before
  treating a shader hash or pipeline object as stable evidence.

## 2026-11-01 patched fork validation

- Fork revision: `723fb51`
- Compatibility change: recognize the legacy LS shader resource descriptors from KytyPS5 PR #1903
- CI: [Windows, Linux, and macOS builds and tests passed](https://github.com/wololo-wololo/NHL27PC/actions/runs/41332687902)
- Host: NVIDIA GeForce RTX 5090, AMD Ryzen 7 9800X3D, Windows x64
- Cache state: clean artifact directory; `--shader-cache` was not enabled
- Result: a quick-play match reached live first-period gameplay and rendered the full rink, players,
  crowd, and HUD. The clock reached 18:34 and the observed frame rate briefly reached about 14 FPS.
  Controller input was not exercised during this automated capture.

This is a substantial improvement over the baseline stop, but it is not yet a stable or polished
playable release. First-run shader compilation repeatedly stalled presentation, sometimes at 0 FPS,
and the renderer emitted recoverable `GPU Stopped Working` breadcrumb reports while continuing to
advance. Observed locations included:

- `ShaderProgram_SubSurfaceScatteringGenerateTilesCS` in `sssConvHorizontal`
- `ShaderProgram_DeferredLightTile_LightingCS_ClearTile` in `mainOpaque`, using an indirect dispatch
- `worldMainTransparent` draw calls
- `ShaderProgram_StrandHairRasterizerFineCs` in `hairResolve`
- `ShaderProgram_MeshWind` in `FieldInteraction/InteractionUpdate`

No game data, shader cache, save data, logs, or captured frames are committed to this repository.

## Next diagnostic milestones

1. Repeat with a persistent shader cache and measure a warm second boot separately from compilation.
2. Capture the `ClearTile` indirect-dispatch arguments and the recoverable compute timeouts using
   Kyty's shader/debug infrastructure.
3. Determine whether each timeout is an infinite guest shader loop, invalid dispatch/draw data, or
   a resource-coherency problem. Do not treat every breadcrumb report as the same defect.
4. Verify controller input, pause/resume, audio, saves, and a complete period on the warm run.
5. Add a virtual filename encoding layer for Frostbite keys that are invalid Windows filenames.
6. Implement the smallest generic correctness fixes possible. If a temporary guard is necessary,
   gate it on `PPSA34063` and keep it disabled for all other titles.
7. Add focused regression tests before enabling fixes by default.
