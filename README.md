# Until Dawn (2015) — Native PC Recompilation

**The original PlayStation 4 version of Until Dawn is now playable from start to finish on Windows — natively, without console emulation.**

This project aims to bring the original **Until Dawn (2015)** to PC, running natively on Windows with its original x86-64 code and our own custom Vulkan renderer.

Development is ongoing. The current build has completed a full play-through on the development PC, but it is **not yet a polished or consistently smooth experience**.

> [!WARNING]
> **Early work-in-progress.** Expect graphical glitches, crashes, shader-compilation pauses and significant frame-rate drops. Some sections of the game currently run **well below 30 FPS**, even on the development PC. Performance is **not addressed in this test release**; it will be improved in future releases, along with stability and hardware support, but there is no guaranteed timetable.

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
- [Future development](#future-development)
- [License](#license)
- [Disclaimer](#disclaimer)

---

## Download

**[Download the latest release from the Releases page](https://github.com/SuperCatElvis/UntilDawn_PC/releases)**

**Current version:** [WIP 0.1.2](https://github.com/SuperCatElvis/UntilDawn_PC/releases/tag/v0.1.2-wip) (`v0.1.2-wip`), the first public test release. Download **`UntilDawn_WIP_0.1.2.zip`** from the release's Assets list (the "Source code" archives only contain this repository's documentation).

The download contains the program, a hardware compatibility tester and documentation, but **no game files**.

---

## Features

What is implemented in WIP 0.1.2:

### Game
- A complete play-through from the prologue to the end credits has been achieved on the development PC.
- Saves work and are kept next to the program in `savedata\`.
- The pre-order bonus chapter **"The Road Not Taken"** (`UDAWNDLC00000001`) works if you have it: an extra
  section in Chapter 2 and two extra Mystery Man clues.
- **Movie skipping:** the intro logos and the "Previously on Until Dawn" recaps can be skipped with an on-screen
  Skip prompt (press any button, then Cross / Enter).

### Graphics
- Custom Vulkan renderer; the game renders at the PS4's 1920×1080 and is shown in a resizable window.
- Windowed or borderless display modes, configurable window size.
- Configurable frame-rate cap (60 FPS by default). **A 60 FPS cap does not mean the game currently runs at 60 FPS consistently.**
- On-disk shader cache (`Cache\`), with shaders compiled in parallel in the background.

### Controllers and input
- **DualSense** (PS5), **DualSense Edge** and **DualShock 4** (PS4) over USB or Bluetooth, with no extra
  software: buttons, touch pad (swipes and click) and motion sensors. **DualSense and DualShock 4 have both been gameplay-tested** (DualShock 4 confirmed by Mocca). Motion-control drift during some “Don't Move” sequences remains a known issue.
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
| **Memory** | 16 GB recommended. The program warns below 12 GB. |
| **GPU** | A Vulkan 1.3 driver supporting `VK_EXT_external_memory_host` and `VK_KHR_push_descriptor`, plus the features and limits checked at startup. **Gameplay has been verified on the development NVIDIA GPU; compatibility tests have also been run on additional NVIDIA and AMD GPUs.** Gameplay and rendering compatibility remain under investigation across hardware. |
| **Disk** | About 50 GB for the game files, plus up to about 1 GB for the shader cache. |
| **Game** | A decrypted dump of Until Dawn for PS4, **CUSA02636**, with the **01.03** update applied, made from your own **legally obtained** copy. Other regions and versions are refused. |

### Tested hardware

The table distinguishes **full gameplay verification** from a **hardware compatibility test**. A compatibility test checks CPU features, Vulkan support and shader behaviour; passing it does not mean that the entire game has been played or that all scenes render correctly.

| CPU | GPU used for compatibility test | Driver | OS | Memory | Result |
| --- | --- | --- | --- | --- | --- |
| Intel Core i9-12900KF | NVIDIA GeForce RTX 3090 Ti | 617.42, 621.01 | Windows 11 | 64 GB | Full play-through; 66/66 compatibility checks passed |
| Intel Core Ultra 9 275HX | NVIDIA GeForce RTX 5070 Laptop GPU | 617.42 | Windows 11 | 32 GB | 66/66 compatibility checks passed |
| AMD Ryzen 7 5700G | NVIDIA GeForce RTX 3060 Ti | 610.88 | Windows 11 | 24 GB | 67/67 compatibility checks passed |
| AMD Ryzen 5 7500F | NVIDIA GeForce RTX 3080 | 617.42 | Windows 11 | 32 GB | 67/67 compatibility checks passed |
| AMD Ryzen 7 5700X | NVIDIA GeForce RTX 5070 | 596.49 | Windows 11 | 16 GB | 63/63 compatibility checks passed |
| AMD Ryzen 5 9600X | AMD Radeon RX 9070 XT | 26.8.1 | Windows 11 | 32 GB | All compatibility checks passed with WIP 0.1.3; graphical corruption in WIP 0.1.2 fixed in WIP 0.1.3 (confirmed) |
| AMD Ryzen 7 5800X | NVIDIA GeForce RTX 3090 | 617.14 | Windows 11 | 32 GB | 67/67 compatibility checks passed |

The Core Ultra 9 system also detected **Intel integrated graphics** as usable, but the compatibility test ran on the **RTX 5070 Laptop GPU**, so this does not verify gameplay on Intel graphics. AMD Radeon testing has identified issues under investigation; see [Known issues](#known-issues). Reports from additional AMD and Intel systems are especially welcome — see [Compatibility testing](#compatibility-testing).

---

## Installation

### 1. Download and extract

1. Download **`UntilDawn_WIP_0.1.2.zip`** from the
   [Releases page](https://github.com/SuperCatElvis/UntilDawn_PC/releases).
2. Extract it to a folder you can write to, for example `D:\Games\UntilDawn_WIP`. Don't use `Program Files`:
   the program keeps its settings, logs, shader cache and saves in its own folder.

```
UntilDawn_WIP_0.1.2\
  UntilDawn.exe            <- start the game
  Config\settings.ini      <- settings
  Game\                    <- put your game files here (or set game_dir)
  compat\                  <- hardware compatibility tester
  README.txt  Controls.txt  KNOWN_ISSUES.txt  THIRD_PARTY_NOTICES.txt
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

Settings (frame-rate limit, windowed / borderless, window size) and troubleshooting:
**[docs/INSTALLATION.md](docs/INSTALLATION.md)**.

> [!NOTE]
> The first visit to a new area may compile shaders for your graphics card. This can cause **pauses lasting 10–20 seconds** during the first play-through. Later runs can benefit from the shader cache, though other freezes and crashes are still possible.

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

- **Occasional crashes.** In a full play-through on the development PC the game crashed twice; saves were kept.
  The cause (some game memory is occasionally overwritten) is being investigated.
- **Characters occasionally deform ("explode") for a frame or two**, and a collectible can vanish or reappear.
- **Graphical bugs:** black mouths in some close-ups, a red flash in the Episodes menu, smeared text at the start
  of the end credits.
- **Long freezes while shaders compile** on the first play-through (see the note under Installation).
- **Frame-rate drops:** frame rate varies widely by scene. Many sections reach 30–60 FPS on the development PC, but **some sections run well below 30 FPS** (10–20 FPS measured in places, and lower during shader-compilation pauses). Performance is not addressed in this test release; it will be improved in future releases. The default 60 FPS cap is a limit, not a guaranteed frame rate.
- **No vibration or light bar** on DualSense / DualShock 4 yet (Xbox controllers vibrate).
- **False "missing files" message when the folder path is long** (cause confirmed): move the release to a shorter
  folder, so the path to its `Game` folder is 62 characters or fewer, e.g. `C:\Games\UntilDawn_WIP_0.1.2`. A fix is
  verified in a test build and planned for the next release.
- **Corrupted textures and black blocks on some AMD GPUs** (under investigation): a potential fix is being tested on
  an affected PC; not confirmed yet. Intel graphics are untested.
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

## Future development

Our immediate priorities are:

1. **Stability** — identify and fix the underlying memory corruption behind remaining crashes and occasional character distortions.
2. **Graphical accuracy** — address known rendering bugs and bring the picture closer to the original PS4 version.
3. **Hardware compatibility** — verify and improve support for AMD and Intel GPUs and AMD CPUs, using reports from other PCs.
4. **Performance** — reduce shader-compilation stalls and severe FPS drops, then work toward consistently smooth gameplay.

**Long-term performance goal:** render the original game at **4K resolution with a smooth, consistent 60 FPS**, and ideally support higher frame rates where hardware allows. This is an ambition for future development, **not a capability of WIP 0.1.2**. The current renderer still renders internally at 1920×1080; achieving native 4K output and stable performance will require further work.

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
