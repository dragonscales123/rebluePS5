<h1 align="center">
  <img width="1480" height="662" alt="re:Blue" src="https://github.com/user-attachments/assets/1779fdfd-bc3a-416d-8b6c-38874d8eae93" />
</h1>

> [!IMPORTANT]
> re:Blue PS5 is an unofficial project and contains NO ASSETS from the original game.
> It is useless unless you have a legal backup of the three Blue Dragon discs (Global release, US/EU).
> It runs on a jailbroken PS5 only, as homebrew.

# re:Blue PS5

A PS5 homebrew port of [re:Blue](https://github.com/zolaware/reblue), which rebuilds Blue Dragon as a native program through static recompilation: the original Xbox 360 code is translated ahead of time into code the console runs directly, instead of emulating an Xbox 360 around it. On the PS5 it renders through Vulkan (Mesa's RADV driver, built into the title) and runs on the [ReXGlue SDK's PS5 port](https://github.com/dragonscales123/rexglue-sdk-PS5/tree/ps5-port).

This is a port of zolaware's work, not a replacement for it. Please don't take PS5 issues to the re:Blue team.

## Table of Contents

- [Status](#status)
- [Requirements](#requirements)
- [How to Install](#how-to-install)
- [Features](#features)
- [FAQ](#faq)
- [What the PS5 Port Changes](#what-the-ps5-port-changes)
- [Building](#building)
- [Credits](#credits)
- [License](#license)

## Status

Playable. Tested on Disc 1 over several play sessions: field exploration, dungeons, battles with a full party, cutscenes, voiced scenes, achievements, saving and loading, and Exit back to the home screen, with a stable frame rate at up to 3840x2160.

Not tested yet: Discs 2 and 3, DLC, and mods.

Download the latest build from [Releases](https://github.com/dragonscales123/rebluePS5/releases).

## Requirements

- A jailbroken PS5 that can run homebrew titles from `/data/homebrew/`, with FTP access to the console.
- All three retail Blue Dragon discs or their disc images (US/EU release).
- About 15 GB free on the console.

## How to Install

1. **Get the title folder.** Download the latest zip from [Releases](https://github.com/dragonscales123/rebluePS5/releases), or build it yourself (see [Building](#building)). Either way you get a folder named `PPSA99213` with `eboot.bin`, `sce_sys/`, `sce_module/` and `.reblue_install`.
2. **Copy it to the console** as `/data/homebrew/PPSA99213/`.
3. **Add the game files.** Extract the files from all three disc images (for example with `extract-xiso`) and merge them into `/data/homebrew/PPSA99213/game/`, so that `game/default.xex` and `game/pack/` exist.
4. **Launch** Blue Dragon from the home screen once your launcher has registered it (ShadowMount+ does this on its own a few seconds after the folder stops changing).

There is no setup wizard on the PS5. `.reblue_install` tells the title that its game files are in `game/` next to it.

## Features

### On the PS5

- **Native, not emulated:** the recompiled game runs directly on the console's CPU and renders through Vulkan on the PS5 GPU, at up to 3840x2160.
- **DualSense ready:** the controller is picked up on its own, with PlayStation button prompts throughout. On the PS5 the Create button belongs to the system, so the **touch pad** is the game's Back/quit button, and the prompts show the touch pad to match.
- **Settings made for the console:** options that only make sense on a PC (window modes, monitor, mouse and keyboard, the updater) are hidden. Resolution, aspect ratio, FOV, frame rate, VSync and every graphics option stay.
- **Clean exit:** choosing Exit returns you to the home screen.
- **Self-contained install:** saves, settings, achievements and logs all live in the title folder, so backing up or moving the game is copying one folder.
- **Crash reports you can act on:** if the game ever stops, `logs/` has a report with every game thread and the faulting state, ready to attach to an issue.

### From re:Blue

Everything below comes from re:Blue and works the same way on the PS5. All of it is configurable in game, from the title screen or the camp menu.

#### Graphics

- Render resolution presets from 1280x720 up to 3840x2160
- Aspect ratios 16:9, 4:3, 16:10, 21:9, 32:9, plus auto and stretch
- Four quality presets, Low through Ultra
- MSAA up to 8x or SSAA up to 4x
- Anisotropic filtering
- Shadow quality and draw distance
- Depth of field adjustment
- Unlocked frame rate, with optional caps at 30, 60, 90, or 120, and VSync

#### Quality of Life

- Save from the camp menu anywhere instead of only at save points
- Field of view adjustment, 45 through 120 degrees
- Skip the in-game tutorial pages
- Full area map on the world map screen, with zoom, floor switching, and a legend
- Optional map markers for the hidden items, chests, and barriers a floor still has, plus per-floor counts, carried onto the field compass
- The field HUD can fade out once you stop pressing anything, or stay off entirely
- Achievement list viewable in game, with eight new re:Blue achievements alongside the original ones
- Master volume control
- UI language and voice language chosen separately
- Custom menus in English, French, German, Italian, and Spanish

#### Mods and DLC

- Built-in mod manager
- Official DLC is supported

Neither has been tested on the PS5 yet.

## FAQ

### Where is my save data and configuration stored?

Everything lives in the title folder, `/data/homebrew/PPSA99213/`:

- Saves and settings: `profiles/default/`
- Your configuration file: `profiles/default/reblue.toml`
- Game files from your discs: `game/`
- Logs: `logs/`, plus `boot.txt` for the first steps of startup
- Mods: `mods/`

Copy `profiles/` off the console before you delete or reinstall the title folder.

### I want to update the game. Will I lose my save data?

No. Replace `eboot.bin` (and `sce_sys/`, `sce_module/` if they changed) over FTP. Your saves, settings and game files are left alone.

### The home-screen icon didn't change after an update

The home screen keeps its own copy of the icon from when the title was registered. Delete the Blue Dragon tile (not the folder in `/data/homebrew/`), and ShadowMount+ registers it again with the new icon.

### Which button quits or goes back?

The touch pad. The PS5 keeps the Create button for screenshots and recording, so games never see it.

### The game closed with "Something went wrong". What do I send?

The newest file in `logs/` and `boot.txt`. On the PS5 a crash report also lists every guest thread and the faulting guest state.

## What the PS5 Port Changes

Most of the work is in the [ReXGlue SDK's PS5 port](https://github.com/dragonscales123/rexglue-sdk-PS5/tree/ps5-port); the rest is the PS5 build in this branch.

### ReXGlue SDK

- **PS5 platform support:** guest memory in one direct-memory arena with fixed views (64 KiB granularity), FreeBSD register layouts for the exception handler, PS5 threads, cpusets and signals, and a static runtime build.
- **Heap corruption at boot:** the console's `getcontext()` writes a larger context than the SDK's `ucontext_t` declares, which overran every thread's fiber object. Fibers on PS5 now switch with their own small routine (callee-saved registers, MXCSR, x87 control word) and don't use ucontext at all.
- **Audio crash at boot:** guest addresses at `0xE0000000` and up sit 4 KiB higher in host memory on the PS5. The runtime applied that offset but the code generator didn't, so the game read the kernel's writes 4 KiB away (first seen as XMA audio contexts reading back as null). The codegen template now matches the runtime.
- **Library collision:** the SDK's VulkanMemoryAllocator is renamed in the static library so it doesn't clash with the renderer's.
- **PS5 patches for SDL3 and FFmpeg.**

### re:Blue

- **PS5 build and packaging:** Vulkan-only build with a prebuilt SPIR-V shader cache, a link step that produces a signed `eboot.bin`, `sce_sys`, and an auto-generated install record.
- **Console startup:** paths under `/app0`, RADV entry points for volk, boot breadcrumbs in `boot.txt`, and exiting back to the shell.
- **Libc gaps:** functions the payload SDK lacks (`timegm`, `pathconf`, `tempnam`, ...), and CP932 text conversion without iconv.
- **Exit crash fixed:** choosing Exit ran the desktop teardown, which crashed on the console; the PS5 build now flushes its log and returns straight to the system.
- **Crash diagnostics:** on PS5 a crash report also lists every guest thread and its processor, plus the faulting guest state.
- **Touch pad prompts:** the PlayStation prompt set's Back glyph is replaced with a touch pad pictogram on PS5, since the touch pad click is what reaches the game as Back.
- **Console settings:** rows with no meaning on the PS5 (display mode, monitor, window size, cursor auto-hide, updater, mouse options, keyboard prompt switch) are hidden there.
- **Quieter logs:** no pointer-lock requests (there is no mouse), and a missing controller database is no longer a warning.
- **Debug allocator:** an optional checking allocator for chasing heap bugs (`REBLUE_PS5_DEBUG_HEAP=ON`).
- **Code regeneration:** the build regenerates the game code whenever the code generator changes, so generator fixes actually take effect.

## Building

The PS5 build cross-compiles on Linux (Ubuntu 24.04; WSL works) with clang/lld 20, mihawk-99's PS5 payload SDK and PS5_Vulkan (RADV), and the ReXGlue SDK's `ps5-port` branch:

```sh
ps5/build.sh
```

The full guide, with the toolchain commits, the game files to put in `assets/`, the folder layout and the debug options, is in [docs/PS5.md](docs/PS5.md). Building needs files from your own copy of Blue Dragon.

## Credits

re:Blue is the work of its development team, and this port would not exist without it.

### re:Blue Development Team

- **[crack](https://github.com/tomcl7)** project lead and developer

- **[rcold](https://github.com/RC0ld)** developer and has done an absurd amount for this project. A lot of re:Blue looks the way it does because of him.

### Playtesting and Support

- **[infernozotza](https://github.com/Zotza)** - Playtester 
- **baus.98** - Playtester
- **[wolfaeterni](https://github.com/Zolawolf)** - Playtester and French Translations 
- **[griever666.](https://github.com/grv666)** - Playtester
- **[fungus](https://github.com/fungoid-creature)** - Playtester
- **[graine25](https://github.com/Graine25)** - macOS and Linux Development Support
- **[zhyxeryz](https://github.com/Zhyxeryz)** - Playtester and German Translations
- **[Azar42](https://github.com/Azar42)** - Playtesting
- **[ZolaKluke](https://github.com/ZolaKluke)** - Playtester
- **[emersed](https://github.com/RaphyEmersed)** - Playtester
- **[mrcmunir](https://github.com/mrcmunir)** - Spanish Translations
- **[mystixor](https://github.com/mystixor)** - German Translations
- **[toby](https://github.com/TbyDtch)** - Graphic Design

### PS5 Port

- **[dragonscales123](https://github.com/dragonscales123)**, with Claude
- **BlackBearReloaded**, for the clean-room `libc.prx`
- **[mihawk-99](https://github.com/mihawk-99)**, for the PS5 payload SDK, PS5_Vulkan and the RADV port this build renders through
- Touch pad prompt glyph drawn for re:Blue PS5
- Dashboard icon: [Dragon emblem](https://pixabay.com/illustrations/dragon-mythology-fantasy-monster-1721875/) by TheDigitalArtist on Pixabay, under the Pixabay Content License

### Special Thanks

- The **[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)** team, for the toolchain this project is built on.

- The **[hedge-dev](https://github.com/hedge-dev)** team, for [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) and for blazing the trail for Xbox 360 recompilations with [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp).

- The wider **Xbox 360 emulation scene**, and the [Xenia](https://github.com/xenia-project/xenia) project in particular. A lot of the hardest problems were solved long before this project started.

## License

re:Blue is under the BSD 3-Clause License; see [LICENSE](LICENSE).

The PS5 title combines re:Blue and the ReXGlue SDK (BSD-3-Clause, with Xenia-derived parts), Mesa/RADV (MIT), SDL (zlib), FFmpeg (LGPL-2.1), and the PS5 payload SDK platform layer and `libc.prx` (GPL-3.0-or-later), so a built title as a whole is GPL-3.0-or-later. The release zips carry every license text in `LICENSES/`.
