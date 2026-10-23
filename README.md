# Until Dawn — Native PC Recompilation

**PC recompilation of the original Until Dawn (2015), running natively on Windows with a custom Vulkan renderer.**

> [!WARNING]
> **Early work-in-progress.** The game is playable from start to finish, but expect graphical bugs, crashes,
> stutters and hardware compatibility problems. It has only been tested on one PC so far.

> [!IMPORTANT]
> **No game files are included.** You need your own decrypted dump of the original PS4 game
> (**CUSA02636**) with the **01.03** update. Downloading this project does not give you the game.

This is an unofficial fan project for the **original 2015 PlayStation 4 release** of Until Dawn. It is not the
2024 remake and is not affiliated with Sony Interactive Entertainment or Supermassive Games
(see [Disclaimer](#disclaimer)).

---

## Contents

- [How it works](#how-it-works)
- [Download](#download)
- [Features](#features)
- [System requirements](#system-requirements)
- [Installation](#installation)
- [Controls](#controls)
- [Known issues](#known-issues)
- [Compatibility testing](#compatibility-testing)
- [Reporting bugs](#reporting-bugs)
- [Future development](#future-development)
- [License](#license)
- [Disclaimer](#disclaimer)

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

---

## Download

**[Download the latest release from the Releases page](https://github.com/SuperCatElvis/UntilDawn_PC/releases)**

Current version: **WIP 0.1.2** (`v0.1.2-wip`), the first public test release.

The download contains the program, a hardware compatibility tester and documentation, but **no game files**.

---

## Features

What is implemented in WIP 0.1.2:

**Game**
- The whole game is playable from the prologue to the end credits (one full play-through on the development PC).
- Saves work and are kept next to the program in `savedata\`.
- The pre-order bonus chapter **"The Road Not Taken"** (`UDAWNDLC00000001`) works if you have it: an extra
  section in Chapter 2 and two extra Mystery Man clues.
- **Movie skipping:** the intro logos and the "Previously on Until Dawn" recaps can be skipped with an on-screen
  Skip prompt (press any button, then Cross / Enter).

**Graphics**
- Custom Vulkan renderer; the game renders at the PS4's 1920×1080 and is shown in a resizable window.
- Windowed or borderless display modes, configurable window size.
- Frame-rate limit: 60 FPS by default (as on PS4), or 30, 120, 144, 165, any other value, or unlimited.
- On-disk shader cache (`Cache\`), with shaders compiled in parallel in the background.

**Controls**
- **DualSense** (PS5), **DualSense Edge** and **DualShock 4** (PS4) over USB or Bluetooth, with no extra
  software: all buttons, touch pad (swipes and click) and motion sensors. Tested with a DualSense.
- **Xbox-style controllers** (XInput, including DS4Windows / Steam virtual pads), with vibration.
- **Keyboard and mouse**, including mouse aiming.

**Usability**
- Checks your game files at every start (all 7,634 files, plus the version of the two executables) and tells you
  in plain words if something is missing or the wrong version.
- Checks your CPU and graphics card at start and explains any missing requirement.
- Settings in a simple `Config\settings.ini`, recreated with defaults if deleted.
- Session logs in `Logs\` (the current session plus the five before it).
- Included **hardware compatibility tester** (`compat\run_compat.bat`).

---

## System requirements

These are the requirements the program checks for. Only one PC has been tested so far (see
[Tested hardware](#tested-hardware)); there are no verified minimum specs yet.

| | Requirement |
| --- | --- |
| **OS** | Windows 10 or 11, 64-bit |
| **CPU** | x86-64 with AVX, F16C, BMI1, LZCNT, POPCNT, MOVBE and SSE4.2 — in practice Intel Core 4th generation (Haswell, 2013) or newer, or AMD Ryzen (or Excavator) or newer. Older Pentium/Celeron and pre-Ryzen AMD CPUs cannot run the game's code. |
| **Memory** | 16 GB recommended. The program warns below 12 GB. |
| **GPU** | A Vulkan 1.3 driver that supports `VK_EXT_external_memory_host` and `VK_KHR_push_descriptor`. Expected to work: NVIDIA GTX 900 series (Maxwell) or newer; AMD RX 400 (GCN 4) / RX 5000 (RDNA) or newer with a 2022+ driver; Intel Arc, or Gen9+ integrated graphics with a 2022+ driver. **Only NVIDIA has been tested.** |
| **Disk** | About 50 GB for the game files, plus up to about 1 GB for the shader cache. |
| **Game** | Your own decrypted dump of Until Dawn for PS4, **CUSA02636**, with the **01.03** update applied. Other regions and versions are refused. |

### Tested hardware

| CPU | GPU | Driver | OS | Memory | Result |
| --- | --- | --- | --- | --- | --- |
| Intel Core i9-12900KF | NVIDIA GeForce RTX 3090 Ti | 617.42, 621.01 | Windows 11 | 64 GB | Full play-through; compatibility test 66/66 passed |

AMD Radeon, Intel Arc / integrated graphics and AMD Ryzen CPUs are **untested**. Reports from these systems are
especially welcome — see [Compatibility testing](#compatibility-testing).

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

You need your own decrypted dump of **CUSA02636 with the 01.03 update applied** on top of the base game. The
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

Choose **one** of these:

- **Copy** the game into the release's `Game` folder, so that `Game\eboot.bin` exists.
- **Point to it:** open `Config\settings.ini` and set `game_dir` to the folder that contains `eboot.bin`:
  ```ini
  game_dir = D:\PS4\CUSA02636
  ```
- **Link it** (saves disk space): delete the release's `Game` folder (it only holds a note), open a Command
  Prompt in the release folder and run:
  ```bat
  mklink /J Game "D:\PS4\CUSA02636"
  ```

Replace `D:\PS4\CUSA02636` with your own game folder.

**Optional DLC:** if you own "The Road Not Taken", copy its `UDAWNDLC00000001` folder from your own copy into
the game folder, next to `eboot.bin`.

### 3. Start the game

Double-click **`UntilDawn.exe`**. At every start it checks your game files (all 7,634 of them, and the version)
and your CPU and graphics card, and tells you in plain words if something is missing or wrong.

The program creates `Logs\`, `Cache\`, `savedata\` (your saves) and `temp0\` itself when it runs.

Settings (frame-rate limit, windowed / borderless, window size) and troubleshooting:
**[docs/INSTALLATION.md](docs/INSTALLATION.md)**.

> [!NOTE]
> The first start of each new area compiles shaders for your graphics card. On the first play-through the game
> can **freeze for 10–20 seconds** at a time while this happens, mostly in the first minutes. It is not a crash —
> wait for it. Later runs use the shader cache and are much smoother.

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
- **Performance:** about 30–60 FPS depending on the scene, even on fast PCs.
- **No vibration or light bar** on DualSense / DualShock 4 yet (Xbox controllers vibrate).
- **AMD and Intel graphics are untested.**
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

The logs contain your hardware description and the folder paths you use; nothing else personal.

**Do not ask for, share or link game files** in issues. Such posts will be removed.

---

## Future development

Priorities, in order:

1. **Stability** — finding the source of the memory corruption behind the remaining crashes and character
   distortions, rather than only containing its effects.
2. **Graphical correctness** — fixing the known rendering bugs and remaining differences from the PS4 picture.
3. **Hardware compatibility** — AMD and Intel GPUs and AMD CPUs, based on your compatibility reports.
4. **Performance** — fewer shader-compile freezes and a steadier frame rate.

Later, this repository may also hold technical documentation for people interested in how the port works or in
modding the game.

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

No game files are included in this repository or its releases. You must provide your own copy of the game.
The small amount of game information the program needs to work with your copy (and its icon) is listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Do not ask for, share or upload game files.

The software is provided as-is, without any warranty.
