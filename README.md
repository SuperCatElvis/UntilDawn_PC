# Until Dawn (2015) — Native PC Recompilation

**The original PlayStation 4 version of Until Dawn is now playable from start to finish on Windows — natively, without console emulation.**

This project aims to bring the original **Until Dawn (2015)** to PC, running natively on Windows with its original x86-64 code and our own custom Vulkan renderer.

Development is ongoing. A full play-through was completed on the development PC with WIP 0.1.3 (later versions have so far been tested in shorter sessions), but the port is **not yet a polished or consistently smooth experience**.

> [!WARNING]
> **Still an early work-in-progress.** Expect graphical glitches, crashes, shader-compilation pauses and frame-rate drops. v0.2.1 prepares the game's shaders before it starts, so the scenes it covers no longer pause to compile them during play. WIP 0.2.0 fixed several long-standing problems — the disappearing arm and collectibles during item inspection, black mouths after the prologue, freezes caused by log writes and a random start-up crash — and added fixes for long sessions and laptops with two GPUs that still need testing. The memory corruption behind most crashes is **not fixed yet**, and the heaviest scenes still run **below 30 FPS**, even on the development PC. Performance, stability and hardware support will keep improving in future releases, but there is no guaranteed timetable.

> [!IMPORTANT]
> **You must supply your own legally obtained game files.** This project does not provide the original PS4 game. The loader requires a decrypted dump of **Until Dawn (CUSA02636)** with update **01.03** applied, made from your own legally obtained copy of the game. See [Installation](#installation) and [Third-party notices](THIRD_PARTY_NOTICES.md) for details about the runtime package.

This is an unofficial fan project and is not affiliated with Sony Interactive Entertainment or Supermassive Games. See the [Disclaimer](#disclaimer).

---

## Contents

- [Download](#download)
- [Features](#features)
- [System requirements](#system-requirements)
- [Installation](#installation)
- [Controls](#controls)
- [Known issues](#known-issues)
- [Compatibility testing](#compatibility-testing)
- [Reporting bugs](#reporting-bugs)
- [How it works](#how-it-works)
- [Modding & technical documentation](#modding--technical-documentation)
- [Source code](#source-code)
- [Future development](#future-development)
- [License](#license)
- [Disclaimer](#disclaimer)

---

## Download

**[Download the latest release from the Releases page](https://github.com/SuperCatElvis/UntilDawn_PC/releases)**

**Current version:** [v0.2.1](https://github.com/SuperCatElvis/UntilDawn_PC/releases/tag/v0.2.1) (`v0.2.1`, work in progress): shaders prepared before the game starts (a progress screen on the first start, from a shader list that comes with the program and your own game files), a renderer hang while textures load fixed, faster shader handling. Download **`UntilDawn_0.2.1.zip`** from the release's Assets list (the "Source code" archives hold this repository — documentation and source code — not the program). Release notes: [docs/releases/v0.2.1.md](docs/releases/v0.2.1.md) · previous: [0.2.0](docs/releases/v0.2.0-wip.md) (collectibles and the arm during item inspection, black mouths, garbled X prompts, freezes from log writes), [0.1.5](docs/releases/v0.1.5-wip.md), [0.1.4](docs/releases/v0.1.4-wip.md), [0.1.3](docs/releases/v0.1.3-wip.md).

The download contains the program, a hardware compatibility tester and documentation, but **no game files**.

---

## Features

What is implemented in v0.2.1:

### Game
- A complete play-through from the prologue to the end credits has been achieved on the development PC (with WIP 0.1.3).
- Saves work and are kept next to the program in `savedata\`.
- The pre-order bonus chapter **"The Road Not Taken"** (`UDAWNDLC00000001`) works if you have it: an extra
  section in Chapter 2 and two extra Mystery Man clues.
- **Movie skipping:** the intro logos, the "Previously on Until Dawn" recaps, the butterfly-effect tutorial, the
  movie that opens the prologue and the Chapter 1 bus ride can be skipped with the game's own Skip prompt (press
  any button, then Cross / Enter). On the tutorial and the prologue movie the prompt's text still shows as boxes.
  Since 0.2.0 a skip also stops the sounds posted since the movie started (meant for music that kept playing; not
  yet confirmed in play).

### Graphics
- Custom Vulkan renderer; the game renders at the PS4's 1920×1080 and is shown in a resizable window.
- Windowed or borderless display modes, configurable window size.
- Choice of graphics card (`gpu`) and presentation mode (`present_mode`, since 0.2.0); in `auto` mode the program
  switches to a safer mode by itself if frames take very long to reach the screen.
- Configurable frame-rate cap (60 FPS by default). **A 60 FPS cap does not mean the game currently runs at 60 FPS consistently.**
- The renderer runs on its own thread alongside the game (since 0.1.4; `renderer_thread = 0` switches back).
- On-disk shader cache (`Cache\`), with shaders (graphics and, since 0.1.5, compute) compiled in parallel in the background.
- Shader preparation (**new in v0.2.1**): before the brightness screen, the shaders the game will need are compiled behind a
  progress screen (Esc / Circle skips it): on the first start those in `pipelines.udm` (a list of shader locations and
  settings, read together with your own game files; no game code is included), later also everything earlier
  sessions needed.
- Lower memory use (since 0.1.5): the game's many small memory heaps are committed in steps as the game uses
  them, and the renderer's video-memory copies are sized to your graphics card.

### Controllers and input
- **DualSense** (PS5), **DualSense Edge** and **DualShock 4** (PS4) over USB or Bluetooth, with no extra
  software: buttons, touch pad (swipes and click) and motion sensors. **DualSense and DualShock 4 have both been gameplay-tested**. Motion-control drift during some “Don't Move” sequences remains a known issue.
- **Xbox-style controllers** (XInput, including DS4Windows / Steam virtual pads), with vibration.
- **Keyboard and mouse**, including mouse aiming.

### Usability
- Checks the required game files and executable versions at startup and reports missing or incompatible files.
- Checks your CPU and graphics card at start and explains any missing requirement.
- Settings in a simple `Config\settings.ini`, recreated with defaults if deleted.
- Session logs in `Logs\` (the current session plus the five before it).
- Included **hardware compatibility tester** (`compat\run_compat.bat`).

---

## System requirements

These are the requirements the program checks for. Compatibility reports have now been received from several different PCs (see [Tested hardware](#tested-hardware)), but there are still no verified minimum specifications. A passing compatibility test does not guarantee that gameplay is free of bugs.

| | Requirement |
| --- | --- |
| **OS** | Windows 10 or 11, 64-bit |
| **CPU** | x86-64 with AVX, F16C, BMI1, LZCNT, POPCNT, MOVBE and SSE4.2 — in practice Intel Core 4th generation (Haswell, 2013) or newer, or AMD Ryzen (or Excavator) or newer. Older Pentium/Celeron and pre-Ryzen AMD CPUs cannot run the game's code. |
| **Memory** | 16 GB recommended. The program warns below 12 GB. Keep Windows' page file on (the default). |
| **GPU** | A Vulkan 1.3 driver supporting `VK_EXT_external_memory_host` and `VK_KHR_push_descriptor`, plus the features and limits checked at startup. **Gameplay has been verified on the development NVIDIA GPU, and WIP 0.1.3 renders the same on an AMD Radeon RX 9070 XT; compatibility tests have also been run on additional NVIDIA GPUs.** Players have run WIP 0.1.4 on a GTX 1660 Ti and an RTX 5050 Laptop GPU; WIP 0.1.5 and 0.2.0 have so far been played on the development NVIDIA GPU only. Laptops with two GPUs can have problems (see [Known issues](#known-issues)). Intel graphics are untested. |
| **Disk** | About 50 GB for the game files, plus up to about 1 GB for the shader cache. |
| **Game** | A decrypted dump of Until Dawn for PS4, **CUSA02636**, with the **01.03** update applied, made from your own **legally obtained** copy. Other regions and versions are refused. |

### Tested hardware

The results are from WIP 0.1.2 and 0.1.3 unless noted. The table distinguishes **full gameplay verification** from a **hardware compatibility test**. A compatibility test checks CPU features, Vulkan support and shader behaviour; passing it does not mean that the entire game has been played or that all scenes render correctly.

| CPU | GPU used for compatibility test | Driver | OS | Memory | Result |
| --- | --- | --- | --- | --- | --- |
| Intel Core i9-12900KF | NVIDIA GeForce RTX 3090 Ti | 617.42, 621.01 | Windows 11 | 64 GB | Full play-through; 66/66 compatibility checks passed |
| Intel Core Ultra 9 275HX | NVIDIA GeForce RTX 5070 Laptop GPU | 617.42 | Windows 11 | 32 GB | 66/66 compatibility checks passed |
| AMD Ryzen 7 5700G | NVIDIA GeForce RTX 3060 Ti | 610.88 | Windows 11 | 24 GB | 67/67 compatibility checks passed |
| AMD Ryzen 5 7500F | NVIDIA GeForce RTX 3080 | 617.42 | Windows 11 | 32 GB | 67/67 compatibility checks passed |
| AMD Ryzen 7 5700X | NVIDIA GeForce RTX 5070 | 596.49 | Windows 11 | 16 GB | 63/63 compatibility checks passed |
| AMD Ryzen 5 9600X | AMD Radeon RX 9070 XT | 26.8.1 | Windows 11 | 32 GB | All compatibility checks passed with WIP 0.1.3; graphical corruption in WIP 0.1.2 fixed in WIP 0.1.3 (confirmed); WIP 0.1.4, 0.1.5 and 0.2.0 not yet tested |
| AMD Ryzen 7 5800X | NVIDIA GeForce RTX 3090 | 617.14 | Windows 11 | 32 GB | 67/67 compatibility checks passed |
| AMD Ryzen (model not reported) | NVIDIA GeForce RTX 5080 | 617.42 | Windows 11 | 56 GB | 67/67 compatibility checks passed |
| Intel Core i5-9400F | NVIDIA GeForce GTX 1660 Ti | 616.92 | Windows 11 | 16 GB | 66/66 compatibility checks passed (WIP 0.1.4); gameplay confirmed, with shader compilation freezes |

**Gameplay reports with WIP 0.1.4:** Intel Core i5-9400F + GeForce GTX 1660 Ti (playable, with occasional crashes from the memory corruption described under [Known issues](#known-issues)); AMD Ryzen 5 220 + GeForce RTX 5050 Laptop GPU with Radeon 740M integrated graphics (long presents and "device lost" crashes when the integrated GPU drives the screen; WIP 0.2.0 contains a fix for the "device lost" part that has not been tested on that laptop yet — see Known issues).

The Core Ultra 9 system also detected **Intel integrated graphics** as usable, but the compatibility test ran on the **RTX 5070 Laptop GPU**, so this does not verify gameplay on Intel graphics. The AMD Radeon graphical corruption reported in WIP 0.1.2 is fixed in WIP 0.1.3 (confirmed on the RX 9070 XT); see [Known issues](#known-issues). Reports from additional AMD and Intel systems are especially welcome — see [Compatibility testing](#compatibility-testing).

---

## Installation

### 1. Download and extract

1. Download **`UntilDawn_0.2.1.zip`** from the
   [Releases page](https://github.com/SuperCatElvis/UntilDawn_PC/releases).
2. Extract it to a folder you can write to, for example `D:\Games\UntilDawn_WIP`. Don't use `Program Files`:
   the program keeps its settings, logs, shader cache and saves in its own folder.

```
UntilDawn_0.2.1\
  UntilDawn.exe            <- start the game
  pipelines.udm            <- the shader list for the first start (v0.2.1; keep it next to UntilDawn.exe)
  Config\settings.ini      <- settings
  Game\                    <- put your game files here (or set game_dir)
  compat\                  <- hardware compatibility tester
  README.txt  INSTALLATION.txt  CHANGELOG.txt  Controls.txt  KNOWN_ISSUES.txt  THIRD_PARTY_NOTICES.txt
```

### 2. Add your game files

You need a decrypted dump of **CUSA02636 with the 01.03 update applied** on top of the base game, made from your own **legally obtained** copy of the game. The
program uses these parts of it (other files can stay; they are ignored):

```
eboot.bin
all_programs.prx
sce_sys\param.sfo
localcachepink\lumps\
localcachepink\streams\
Movies\
audio\
```

The simplest method is to **copy** your game files into the release's `Game` folder, so that `Game\eboot.bin` exists.

To avoid copying the files, you can also **point the launcher at your existing game folder**: open `Config\settings.ini` and set `game_dir` to the folder containing `eboot.bin`:
  ```ini
  game_dir = D:\PS4\CUSA02636
  ```
Replace `D:\PS4\CUSA02636` with your own game folder. An optional directory-junction setup is documented in **[docs/INSTALLATION.md](docs/INSTALLATION.md)**.

**Optional DLC:** if you own "The Road Not Taken", copy its `UDAWNDLC00000001` folder from your own copy into
the game folder, next to `eboot.bin`.

### 3. Start the game

Double-click **`UntilDawn.exe`**. At every start it checks your game files (all 7,634 of them, and the version)
and your CPU and graphics card, and tells you in plain words if something is missing or wrong.

The program creates `Logs\`, `Cache\`, `savedata\` (your saves) and `temp0\` itself when it runs.

Settings (frame-rate limit, windowed / borderless, window size, renderer thread, GPU choice, presentation mode) and troubleshooting:
**[docs/INSTALLATION.md](docs/INSTALLATION.md)**.

**Updating from 0.2.0:** copy the new `UntilDawn.exe`, the new `pipelines.udm`, `compat\ud_compat.exe` and the
text files over your old folder, but **do not overwrite** `Config\settings.ini`, `savedata\` or `Game\`. Saves,
settings and `Cache\` carry over; the new setting `shader_preparation` uses its default (on) when it is not in your
file. The first start shows a "Preparing shaders" screen while the shaders are compiled (Esc or Circle skips it).

**Updating from 0.1.5 (or earlier):** copy the new `UntilDawn.exe`, `compat\ud_compat.exe` and the text files over
your old folder, but **do not overwrite** `Config\settings.ini`, `savedata\` or `Game\`. Saves and settings carry
over; the new settings (`gpu`, `present_mode`, `heap_slot_mb`, `heap_slot_reuse`) use their defaults when they are
not in your file. Every shader is translated anew in 0.2.0, so the old `Cache\` no longer matches (you can delete it)
and the first-play-through shader pauses return once.

> [!NOTE]
> From v0.2.1 the shaders are compiled **before the game starts**, behind a "Preparing shaders" screen: on the first start those in `pipelines.udm` (about 30–40 seconds on the development PC, Core i9-12900KF, 24 threads; longer on CPUs with fewer cores), later also those earlier sessions needed (under a second when nothing changed). On the development PC the first-start sequence (brightness, controls, intro, menu) then compiled no shaders during play. Scenes that are not in the list yet still compile their shaders the first time they appear — with 0.2.0 and an empty cache that meant one pause of about 12 seconds and a few of 3–8 seconds in the first minutes, and 10–20 seconds or more on CPUs with fewer cores — and are prepared from the next start on. Other freezes and crashes are still possible.

---

## Controls

Controllers, keyboard and mouse all work at the same time. Full list: **[docs/CONTROLS.md](docs/CONTROLS.md)**.

| PS4 button | Keyboard / mouse | Xbox controller |
| --- | --- | --- |
| Left stick | W A S D | Left stick |
| Right stick (camera) | I J K L, or the mouse with mouse aiming on (**M**) | Right stick |
| Cross | Enter or Space | A |
| Circle | Esc or Backspace | B |
| Square | Q | X |
| Triangle | E | Y |
| L1 / R1 | 1 / 3 | LB / RB |
| L2 / R2 | Z / C (right / left mouse button with mouse aiming on) | LT / RT |
| D-pad | Arrow keys | D-pad |
| Options | Tab | Menu |
| Touch pad click | T | View |
| Touch pad swipes | F (left), G (right), R (up), V (down) | — (use the keyboard) |

DualSense and DualShock 4 controllers work like on the PS4, including the touch pad and the "Don't move" motion
moments.

---

## Known issues

The most important ones — full list in **[docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md)**:

- **Occasional crashes — not fixed yet.** Now and then the lowest byte of a pointer in the game's memory is set to
  zero, and the game crashes later when it follows that pointer. It happens on the development PC and on players'
  PCs with every version so far, including in testing of 0.2.0; WIP 0.2.0 does not fix it. The writer has now been
  caught: it is the game's own code finishing an object whose memory overlaps another object still in use (two of
  the game's allocations overlap). Saves are kept. Fixing it is the top stability priority.
- **Characters occasionally deform ("explode") for a frame or two** (most likely the same problem).
- **Laptops with two GPUs** (an NVIDIA GPU plus integrated graphics that drive the screen): very slow frame
  presentation, "device lost" crashes and flickering UI in some menus were reported with 0.1.4
  ([issue #2](https://github.com/SuperCatElvis/UntilDawn_PC/issues/2)). WIP 0.2.0 contains a fix for the program's part
  of the "device lost" hang and can switch to a safer presentation mode by itself, but it has **not been tested on such a
  laptop yet**. Also try Windows' Graphics settings → UntilDawn.exe → "High performance", or the laptop's
  discrete-GPU (MUX) mode.
- **Graphical bugs:** flickering spots on the R2 choice cards, flickering subtitles, a see-through strip in the black
  bar of the character introductions for a few frames, a slight lighting effect on Chris's glasses (not yet checked),
  the main-menu character animating too fast, light squares on Emily's eyes and dark squares on a wall in the
  prologue, totem visions that lose their background or UI, a red flash in the Episodes menu, smeared text at the
  start of the end credits, and the Skip prompt's "Skip" text as white boxes on the butterfly tutorial, the
  prologue's opening movie and later recap clips.
- **Shader-compilation pauses** the first time new content appears (see the note under Installation): on a new
  install up to about 12 seconds even on the development PC, longer on CPUs with few cores; once per content.
- **Frame-rate drops:** the frame rate still varies by scene, and is similar to 0.1.5. On the development PC (RTX 3090 Ti) light scenes often reach 60 FPS, busy scenes run at about 30–45 FPS, and the heaviest scenes (the "stranger" shots in the prologue) at about 22–29 FPS, lower during shader-compilation pauses. The default 60 FPS cap is a limit, not a guaranteed frame rate.
- **WIP 0.2.0's changes have only been played on the development PC (NVIDIA).** If 0.2.0 crashes or stutters where 0.1.5 did not, please report it. `renderer_thread = 0` in `Config\settings.ini` (the renderer on the game's thread) is still available for freezes and stutters; it does not prevent the memory-corruption crashes.
- **No vibration or light bar** on DualSense / DualShock 4 yet (Xbox controllers vibrate). **Keys cannot be remapped** yet.
- **Fixed in WIP 0.2.0:** the character's arm and the picked-up clue or totem vanishing during item inspection
  ([issue #3](https://github.com/SuperCatElvis/UntilDawn_PC/issues/3)) and, by the same fix, Mike's lantern arm coming
  apart in Chapter 5; black mouths in Chapter 2 and later after the
  prologue; striped / garbled X button prompts; a white flash on the character introduction cards; freezes of a
  second or more caused by log writes; a random crash at start-up. **Changed in WIP 0.2.0, not yet confirmed in
  play:** game data loaded after about 25–30 minutes invisible to the GPU; the aiming reticle shown as a red square;
  music that kept playing after a skipped movie; the "device lost" hang on laptops with two GPUs.
- **Improved in WIP 0.1.5:** memory use (about 8.2 GB in RAM instead of 12.7 GB on the development PC's test route). **Fixed in WIP 0.1.4:** hair that looked hazy or see-through.
- **Fixed in WIP 0.1.3 (still fixed):** the false "missing files" message when the folder path was long, and the
  graphical corruption on AMD GPUs in WIP 0.1.2 (noisy textures, black blocks, missing foliage, stepped lighting) —
  confirmed on an AMD RX 9070 XT. More AMD reports are welcome. Intel graphics are untested.
- Online / PlayStation Network features are not available. Trophy unlocks are recorded locally (in
  `savedata\` and the log), but there is no on-screen trophy notification.

---

## Compatibility testing

The release includes a hardware compatibility tester. It checks your CPU, memory, Windows and graphics driver
against what the port needs, runs the port's own CPU and GPU code on your hardware, and compiles every one of the
game's GPU programs with your driver. It installs nothing and changes nothing.

1. Run **`compat\run_compat.bat`** from the release folder (takes about 1–5 minutes).
2. It creates **`compat\compat_report.zip`**.
3. Open a [compatibility report](https://github.com/SuperCatElvis/UntilDawn_PC/issues/new/choose) and attach
   the ZIP — even if everything passed. Reports from **AMD and Intel** systems are especially useful.

Details: **[docs/COMPATIBILITY_TESTING.md](docs/COMPATIBILITY_TESTING.md)**.

---

## Reporting bugs

Please use **[GitHub Issues](https://github.com/SuperCatElvis/UntilDawn_PC/issues)** and include:

- What happened, and what you were doing (chapter, scene, menu).
- Whether it happens again, and how to make it happen.
- A screenshot or short video for graphical problems.
- `Logs\UntilDawn.log` from the session with the problem (if you restarted the game since then, it is
  `Logs\UntilDawn.previous.log`).
- `compat\compat_report.zip` (run `compat\run_compat.bat` once).
- Your CPU, graphics card and driver version (the report contains these).

**Privacy:** logs and compatibility reports can contain hardware details, file paths and potentially identifying information (such as a Windows username in a path). Please review and redact anything sensitive before posting them publicly.

**Do not ask for, share or link game files** in issues. Such posts will be removed.

---

## How it works

The PS4's processor uses the same x86-64 instruction set as a Windows PC, so the game's original program code
does not need to be emulated: **it runs directly on your CPU**, natively on Windows. There is no console
emulator and no CPU emulation.

What this project provides is everything around that code:

- **A loader** that reads the game's own executables (`eboot.bin`, `all_programs.prx`) from your game folder,
  maps them into memory and starts them. Nothing is decrypted by this program, and the game files on disk are
  never modified; small compatibility patches are applied in memory only.
- **A replacement for the PS4 system libraries.** The game calls 748 PS4 system functions (threads, files,
  memory, saves, audio, controllers, graphics). This project answers those calls with Windows equivalents.
- **A custom Vulkan renderer.** The game's PS4 GPU command streams are translated to Vulkan, and its AMD GCN
  GPU programs (shaders) are translated to SPIR-V on your PC and compiled by your graphics driver. Compiled
  shaders are cached on disk so later runs stutter less.

All of the game's code, art, audio and video are read at run time from **your own copy** of the game.

For an in-depth look at the internals (for modders and the curious), see
**[docs/TECHNICAL_OVERVIEW.md](docs/TECHNICAL_OVERVIEW.md)**.

---

## Modding & technical documentation

Interested in modding the original Until Dawn on PC?

The **[Technical Overview](docs/TECHNICAL_OVERVIEW.md)** explains how the native recompilation works, including
the game's memory, file formats, rendering, shaders, input systems and in-memory modifications.

The documentation is intended to help anyone interested in understanding the game's internals or exploring the
possibility of creating mods for the native PC version.

Modding support is still in its early stages, and there is currently **no dedicated mod loader**. We hope to
expand the documentation and make the game more accessible to modders as development progresses.

---

## Source code

The complete source of the port — the Windows runtime and PS4 compatibility layer, the Vulkan renderer, the
GCN → SPIR-V shader translator, the hardware compatibility tester, the build scripts, tests and supporting tools —
is in **[source/](source/README.md)**, with instructions for building it on Linux with LLVM. It contains no game
code, data or assets; see [Third-party notices](THIRD_PARTY_NOTICES.md). `data/pipelines.udm` (the shader preparation
manifest, v0.2.1) holds only locations in the game's files, translator inputs and pipeline state, checked to contain no
shader code. It is the source of the releases from
v0.2.1 on; for earlier releases see [Third-party notices](THIRD_PARTY_NOTICES.md#earlier-releases-wip-012--020).

---

## Future development

Our immediate priorities are:

1. **Stability** — identify and fix the underlying memory corruption behind remaining crashes and occasional character distortions, and the problems on laptops with two GPUs.
2. **Graphical accuracy** — address known rendering bugs and bring the picture closer to the original PS4 version.
3. **Hardware compatibility** — verify and improve support for AMD and Intel GPUs and AMD CPUs, using reports from other PCs.
4. **Performance** — WIP 0.1.4 roughly quadrupled the frame rate in the heaviest scenes on the development PC, WIP 0.1.5 cut memory use and shader-compilation freezes, and WIP 0.2.0 removed the freezes caused by log writes; next is a redesign of the renderer so that its work is spread over several CPU cores, plus fewer shader-compilation stalls, working toward consistently smooth gameplay.

**Long-term performance goal:** render the original game at **4K resolution with a smooth, consistent 60 FPS**, and ideally support higher frame rates where hardware allows. This is an ambition for future development, **not a capability of WIP 0.2.0**. The current renderer still renders internally at 1920×1080; achieving native 4K output and stable performance will require further work.

**Planned quality-of-life improvements:** a proper **in-game settings menu** for graphics, frame-rate limits, controls, input bindings and other configurable options, rather than relying mainly on `Config\settings.ini`.

**Localisation:** support for additional languages is planned closer to a more complete release. It is **not a current development priority**, and language availability will depend on the relevant game resources and implementation work.

More documentation for people interested in the internals and in creating mods may follow (see [Modding & technical documentation](#modding--technical-documentation)).

---

## License

See [LICENSE](LICENSE). The licence applies only to this project's own work. It does not apply to, and grants no
rights in, Until Dawn, its program code, assets or trademarks. Third-party notices are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

---

## Disclaimer

This is an unofficial, non-commercial fan project. It is **not affiliated with, endorsed or supported by Sony
Interactive Entertainment or Supermassive Games**. "Until Dawn" is a trademark of Sony Interactive
Entertainment. "PlayStation", "DualSense" and "DualShock" are trademarks of Sony Interactive Entertainment.

The original game installation is **not provided**; users must supply their own legally obtained copy. Any game-derived material identified in the runtime package is documented in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Do not ask for, share or upload game files.

The software is provided as-is, without any warranty.
