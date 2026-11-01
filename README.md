# NHL 27 PC

NHL 27 (PS5, `PPSA34063`) running on Windows through a game-focused fork of
[KytyPS5](https://github.com/KytyPS5/KytyPS5).

The PS5 and a Windows PC are both x86-64 machines. KytyPS5 loads the game's x86-64 code directly
on the host CPU, replaces PS5 system services with native implementations, and translates the
PS5's RDNA 2 graphics work to Vulkan. This project keeps Kyty's generic launcher available while
putting NHL-specific defaults, diagnostics, patches, and compatibility work behind the NHL 27
title ID.

> [!WARNING]
> This is an early development fork, not a playable release. On the current baseline the game
> boots into the Frostbite renderer, then the GPU stops in the `mainOpaque` pass near an indirect
> compute dispatch. See [the bring-up log](docs/BRINGUP.md).

## Project shape

- **Focused distribution:** one-click setup and tested defaults target NHL 27 (`PPSA34063`).
- **Multi-game-capable core:** generic Kyty behavior remains available; title workarounds must be
  gated by title ID and version.
- **Upstream-friendly fixes:** reusable HLE, shader, renderer, filesystem, and performance fixes
  should stay generic and be suitable for submission to KytyPS5.
- **No game data:** the repository contains no games, keys, firmware, saves, shader caches, or
  proprietary system software.

## Current status

| Component | Status |
| --- | --- |
| Game metadata / executable load | Working |
| PS5 HLE initialization | Working far enough to enter Frostbite |
| Audio initialization | Working in the tested path |
| Vulkan initialization | Working |
| Rendering | GPU hang in `mainOpaque`; not playable on the current test host |
| Dedicated NHL 27 launcher | Planned; validated launch script available now |
| Other games | Generic Kyty launcher remains available, but is not this fork's test focus |

Baseline: NHL 27 content version `01.003.001`, KytyPS5 commit `893b035`, Windows x64.

Other testers have reached the main menu and controllable gameplay with open KytyPS5 PR
[#1903](https://github.com/KytyPS5/KytyPS5/pull/1903). Results remain hardware-dependent and do
not yet make the game generally playable.

## Run the current baseline

You need a legally obtained, decrypted NHL 27 dump and an official KytyPS5 Windows build. Keep
both outside this repository.

```powershell
.\tools\run_nhl27.ps1 `
  -GameDirectory "D:\Games\PPSA34063-app0" `
  -KytyDirectory "D:\Emulators\KytyPS5-2026-11-01-893b035-Windows-x64"
```

The script refuses the wrong title ID, verifies the required files, and starts Kyty without
copying game data. Pass `-ShaderCache` after a clean diagnostic run, and use
`-EmulatorArguments` for additional Kyty options.

To launch other games, use the unmodified `kyty_launcher.exe` from a Kyty build. NHL-specific
changes in this fork must not silently apply to another title.

## Building

The source currently follows KytyPS5's build requirements: Visual Studio 2022 Build Tools with
Clang, CMake, Ninja, Qt 6, glslang, Python, and initialized Git submodules.

```powershell
git clone --recursive https://github.com/YOUR_ACCOUNT/nhl27pc.git
cd nhl27pc
cmake -S . -B _Build/windows -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=clang-cl `
  -DCMAKE_CXX_COMPILER=clang-cl `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.x.x/msvc2022_64"
cmake --build _Build/windows
```

Run the repository safety audit before every push:

```powershell
.\tools\check_no_game_files.ps1
```

CI runs the same audit and rejects known game-content paths/extensions and tracked files larger
than 50 MiB.

## Legal

This project is for interoperability, compatibility research, and preservation. It does not
distribute NHL 27, PlayStation firmware, keys, or copyrighted system software. Use only software
you obtained legally.

This fork is derived from KytyPS5 and remains licensed under GPL-2.0; see [LICENSE](LICENSE) and
[LICENSES](LICENSES). “NHL,” “EA SPORTS,” and “PlayStation” are trademarks of their respective
owners. This project is not affiliated with or endorsed by EA, the NHL, or Sony.
