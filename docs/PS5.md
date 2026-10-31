# re:Blue on the PS5 (homebrew)

This branch builds re:Blue as a homebrew title for a jailbroken PS5. It
renders through plume's Vulkan backend on RADV, which is linked into the
title, and runs on the ReXGlue SDK's PS5 port
(github.com/dragonscales123/rexglue-sdk-PS5, branch `ps5-port`).

Status: boots, plays with graphics, music, voices and DualSense input; battles,
cutscenes, saving and loading tested on Disc 1. Discs 2 and 3 are untested.

You need your own copy of Blue Dragon. Nothing from the game is in this
repository, and nothing derived from it should be published.

## What you need

- Ubuntu 24.04 (WSL works; keep the work tree in the Linux filesystem, not
  under `/mnt/c`).
- clang/lld 20. clang 18 cannot build the SDK (`std::expected`).
- mihawk-99's PS5 toolchain at these commits:

  | Repository | Commit |
  | --- | --- |
  | PS5_PayloadSDK | `b5efad5` |
  | PS5_Vulkan | `252bf91` (with its RADV release archive built) |
  | PS5_VulkanTemplate | `91179df7` |
  | PS5_Mesa | `47e9eb61ee7` |

  PS5_VulkanTemplate's `ps5/tools/bootstrap.sh` builds the payload SDK and
  RADV. Building Mesa's host kernel compiler needs LLVM 18's SPIR-V
  translator, so `llvm-config-20` must not be found while `mesa_clc`
  configures.
- The checkouts side by side:

  ```
  work/
    reblue/              this repository, branch ps5-port
    rexglue-sdk/         dragonscales123/rexglue-sdk-PS5, branch ps5-port (cloned into this folder name)
    PS5_VulkanTemplate/
    PS5_Vulkan/
  ```

  Initialise the submodules in both repositories
  (`git submodule update --init --recursive`). `ps5/build.sh` applies the PS5
  patches for SDL and FFmpeg (in the SDK's `tools/patches/`) and plume
  (`ps5/patches/`) the first time it runs.

## Game files

Put these from your own Disc 1 into `assets/` (ignored by git):

- `default.xex` from the disc root.
- The 152 shaders (`*.vso`, `*.pso`) from `pack/!necessity.ipk`
  (`bd_system\fx\`). IPK1 records are zlib streams.

## Build

```
ps5/build.sh
```

Options: `--sdk`, `--vulkan`, `--rexsdk`, `--title-id` (default `PPSA99213`).
The script builds the SDK's code generator and XenosRecomp for the host,
compiles the game shaders to SPIR-V, generates and compiles the recompiled
code, and links the title with `ps5/link-title.sh`. The first build takes a
long time (about 110 MB of generated C++).

`REBLUE_PS5_DEBUG_HEAP=ON ps5/build.sh` builds a checking allocator
(`src/platform/ps5_debug_heap.cpp`: guard bytes, a quarantine and a checker
thread) for chasing heap damage. Switching it rebuilds one file.

The result is `out/build/ps5/dist/PPSA99213/`: `eboot.bin`, `sce_sys/`,
`sce_module/` and `.reblue_install`.

The dashboard icon (`ps5/sce_sys/icon0.png`) is TheDigitalArtist's dragon
emblem from Pixabay (Pixabay Content License). To use your own, put a 512x512 PNG at
`ps5/sce_sys/icon0.local.png`. Git ignores it and the build uses it in
place of the placeholder.

## Install

1. Copy the title folder to `/data/homebrew/PPSA99213/` (FTP works).
2. Extract the files from all three disc images and merge them into
   `/data/homebrew/PPSA99213/game/`, so that `game/default.xex` and
   `game/pack/` exist.
3. Launch it from your homebrew launcher.

The title folder includes `.reblue_install`, a portable install record that
points the title at `game/` next to it; the PS5 build has no setup wizard.

Logs go to `/data/homebrew/PPSA99213/logs/`, and early boot steps to
`boot.txt` in the same folder. Settings and saves live under `profiles/`.
`ps5-env.txt` in the title folder, one `NAME=value` per line, sets environment
variables before startup.

## What the PS5 port changes

In the SDK (`ps5-port` branch):

- Platform: `REX_PLATFORM_PS5` (FreeBSD-derived payload SDK, static title).
- Guest memory: one direct-memory arena with fixed views, 64 KiB allocation
  granularity (`src/core/memory_ps5.cpp`, `MapViewsPs5`).
- Exceptions, threads and signals use FreeBSD's register layouts and calls.
- Fibers: the console's `getcontext()` writes a larger context than the SDK's
  `ucontext_t` declares, so fibers switch with their own routine
  (`rex_ps5_fiber_switch`, `src/core/fiber_posix.cpp`).
- The codegen template applies the 4 KiB host offset for guest addresses at
  `0xE0000000` and up on PS5, matching the runtime. Without it the game reads
  kernel writes into the physical heap 4 KiB away (first seen as XMA contexts
  reading back as null in the audio mixer).
- The SDK's copy of VulkanMemoryAllocator is renamed in the static library so
  it does not collide with plume's.

In reblue:

- `cmake/RebluePs5.cmake`, `ps5/`: PS5 build, link and packaging.
- `src/platform/ps5_title.cpp`: startup on the console (paths under `/app0`,
  RADV entry points for volk, crash breadcrumbs, exit to the launcher).
- `src/platform/ps5_libc.cpp`: libc functions the payload SDK lacks.
- `src/core/encoding.cpp`: CP932 conversion without iconv.
- `src/platform/crash_handler.cpp`: on PS5 a crash report also lists every
  guest thread and its processor, the audio engine's per-processor state and
  the faulting guest context.
- No setup wizard and no updater on PS5; `ps5/link-title.sh` writes the
  install record into the title folder. Shaders are compiled at build time.
