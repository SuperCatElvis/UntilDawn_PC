# Until Dawn (2015) — Native PC Recompilation

**Play the original PlayStation 4 version of Until Dawn natively on Windows, without console emulation.**

This project brings the original PlayStation 4 version of Until Dawn to PC using the game's original x86-64 code, a custom Windows runtime and a Vulkan renderer.

The game can be played from start to finish, although development is ongoing. A full playthrough was completed on an earlier build; the latest release has not yet had a full playthrough test.

> [!WARNING]
> **Work in progress.** Crashes, graphical issues, stuttering and low frame rates may still occur. Performance varies depending on your hardware and the scene.

> [!IMPORTANT]
> **You need your own legally obtained copy of Until Dawn (2015) for PS4, CUSA02636, with update 1.03 applied.** No game files are included.

This is an unofficial fan project, not affiliated with Sony Interactive Entertainment or Supermassive Games.

## Download

**[Download the latest release](https://github.com/SuperCatElvis/UntilDawn_PC/releases/latest)**

**Current release:** [v0.4.1](https://github.com/SuperCatElvis/UntilDawn_PC/releases/tag/v0.4.1)

Download `UntilDawn_PC_v0.4.1.zip` from the release's **Assets** section.

### What's new in v0.4.1

- Added automatic low-memory mode when the graphics driver reports a small system-memory budget.
- Reduced the upload buffer and reserved more video memory in low-memory mode.
- Added a `LOW MEMORY` warning to `Logs\UntilDawn.log`.

This does **not** yet resolve all crashes on PCs with 16 GB RAM.

See the [full v0.4.1 changelog](docs/releases/v0.4.1.md). Older changes are covered in the [v0.4.0](docs/releases/v0.4.0.md), [v0.3.2](docs/releases/v0.3.2.md), [v0.3.1](docs/releases/v0.3.1.md) and [v0.3.0](docs/releases/v0.3.0.md) changelogs.

### v0.5.0 Currently In Progress

Development of v0.5.0 is continuing, with a range of improvements and new features already implemented.

**Completed Improvements**
- **Language Pack Installer** – Added support for installing additional language packs.
- **Additional Languages** – Expanded language support beyond the default English version.
- **Automatic Updater** – Added automatic update functionality.
- **Depth of Field Setting** – Added an option to control depth of field.
- **Low-Memory Optimisations** – Introduced automatic optimisations for systems with limited memory.
- **Memory Management** – Improved memory handling and resource management.
- **Performance Improvements** – CPU optimisations and reduced processing bottlenecks.
- **Improved Audio** – Fixed audio playback issues and clipping.
- **Controller Improvements** – Further improvements to controller handling.
- **Bug Fixes** – Fixed various issues, including the Dr. Hill book interaction.

**Ongoing Development**
- Further renderer and graphics pipeline optimisation.
- Continued CPU and multithreading improvements.
- Additional stability, performance and memory optimisations.

## Features

- The original PS4 story, chapters and endings.
- Native Windows execution using the game's original code.
- Custom Vulkan renderer with 1080p, 1440p, 4K and other resolutions.
- Borderless fullscreen by default, with windowed mode available.
- Frame-rate limits of 30, 60, 120, 240 or uncapped.
- Shader preparation and caching to reduce compilation stutters.
- DualShock 4, DualSense, DualSense Edge, Xbox controllers, keyboard and mouse.
- Touch pad, motion controls and Don't Move sequences.
- Local saves and configurable settings.
- Optional original **Halloween Hunt 2015** event.
- Support for **The Road Not Taken** bonus chapter if you own the DLC.

## System requirements

| | Minimum | Recommended |
|---|---|---|
| **OS** | Windows 10/11, 64-bit | Windows 10/11, 64-bit |
| **Processor** | 6-core x86-64 CPU with AVX2 (Intel Core i5-10400F / AMD Ryzen 5 3600 or equivalent) | 8-core / 16-thread CPU or better (Intel Core i7-10700K / AMD Ryzen 7 5700X or better) |
| **Memory** | 16 GB RAM | 32 GB RAM |
| **Graphics** | NVIDIA GeForce GTX 1660 Ti / AMD Radeon RX 5600 XT or equivalent (**6 GB VRAM**) | NVIDIA GeForce RTX 3060 Ti / AMD Radeon RX 6700 XT or equivalent (**8 GB+ VRAM**) |
| **Graphics API** | Vulkan 1.3 | Vulkan 1.3 |
| **Storage** | 56 GB free | 56 GB free |
| **Game** | CUSA02636, update 1.03 | CUSA02636, update 1.03 |

**Important:**

- **6 GB VRAM is the current minimum.** Cards with 4 GB, including the GTX 970, are below minimum.
- **8 GB system RAM is not currently supported.** Lower-memory support is being investigated.
- Meeting the minimum requirements does not guarantee stable frame rates or compatibility.
- Keep the Windows page file enabled.
- Your graphics driver must support the Vulkan features and extensions checked at startup, including `VK_EXT_external_memory_host` and `VK_KHR_push_descriptor`.
- NVIDIA GPUs have received the most testing. AMD support is improving; Intel GPUs remain largely untested.

### Tested hardware

These results include gameplay and compatibility checks from different development builds. Passing a compatibility check does not guarantee a trouble-free playthrough.

| CPU | GPU | RAM | Result |
|---|---|---|---|
| Intel Core i9-12900KF | RTX 3090 Ti | 64 GB | Full playthrough; compatibility checks passed |
| Intel Core Ultra 9 275HX | RTX 5070 Laptop | 32 GB | Compatibility checks passed |
| AMD Ryzen 7 5700G | RTX 3060 Ti | 24 GB | Compatibility checks passed |
| AMD Ryzen 5 7500F | RTX 3080 | 32 GB | Compatibility checks passed |
| AMD Ryzen 7 5700X | RTX 5070 | 16 GB | Compatibility checks passed |
| AMD Ryzen 5 9600X | Radeon RX 9070 XT | 32 GB | Compatibility checks passed on WIP 0.1.3 |
| AMD Ryzen 7 5800X | RTX 3090 | 32 GB | Compatibility checks passed |
| AMD Ryzen (model not reported) | RTX 5080 | 56 GB | Compatibility checks passed |
| Intel Core i5-9400F | GTX 1660 Ti | 16 GB | Compatibility checks passed on WIP 0.1.4; gameplay reported |

The AMD RX 7800 XT black-screen issue was fixed in v0.3.2 with a contribution from [Pedro-Castro31](https://github.com/Pedro-Castro31). Results on other AMD and Intel cards are still welcome.

## Installation

### 1. Download and extract

Download the latest ZIP from [Releases](https://github.com/SuperCatElvis/UntilDawn_PC/releases) and extract it somewhere convenient, such as `D:\Games\UntilDawn`.

Avoid `Program Files`, as the program keeps its settings, saves, logs and shader cache in its own folder.

### 2. Add your game files

Copy your own decrypted **CUSA02636, update 1.03** game files into the `Game` folder beside `UntilDawn.exe`.

The folder should contain files and folders such as:

```text
Game\
├── eboot.bin
├── all_programs.prx
├── sce_sys\
├── localcachepink\
├── Movies\
└── audio\
```

Alternatively, point to an existing folder in `Config\settings.ini`:

```ini
game_dir = D:\PS4\CUSA02636
```

If you own **The Road Not Taken** DLC, include its `UDAWNDLC00000001` folder.

### 3. Launch

Run `UntilDawn.exe`. The program checks your game files and hardware before starting.

The first launch prepares shaders, usually around **2–3 minutes on a modern 8-core CPU**, though this varies. Later launches are faster because shaders are cached. Skipping preparation may cause stutters while playing.

See the [Installation Guide](docs/INSTALLATION.md) for more help.

### Updating

Replace the program files with those from the new release.

**Keep your existing `Game\`, `savedata\` and `Config\settings.ini`.** Your game files, saves and settings should carry over. A new version may need to prepare shaders again on first launch.

## Settings and controls

Most options are in `Config\settings.ini`.

| Setting | What it does |
|---|---|
| `optimised_rendering` | Enables or disables Fast Mode; on by default |
| `frame_rate_limit` | 30, 60, 120, 240 or uncapped (up to 240 FPS) |
| `display_mode` | Borderless fullscreen or windowed; `Alt+Enter` switches |
| `render_resolution` | Monitor resolution automatically, or a fixed resolution |
| `shader_preparation` | Prepares shaders before gameplay |
| `gpu_coherence` | Keeps CPU and GPU memory synchronised |
| `gpu` | Selects your graphics card |
| `present_mode` | Selects frame presentation mode |
| `halloween_hunt` | Set to `1` to enable the 2015 Halloween event |

PlayStation and Xbox controllers are supported, as well as keyboard and mouse. Mouse aiming can be toggled with **M**.

See the [Controls Guide](docs/CONTROLS.md) for all key bindings and the [Installation Guide](docs/INSTALLATION.md) for more settings.

## Known issues

This is still a work in progress. Current issues include:

- Occasional crashes, including some demanding scenes on PCs with 16 GB RAM.
- Low frame rates and stuttering in heavier scenes or while entering new areas.
- Some lighting, UI and character-rendering issues.
- Motion-control drift during certain Don't Move sequences.
- Some performance and display problems on laptops with multiple GPUs.
- No PlayStation controller vibration or light-bar support yet.
- Keyboard bindings cannot currently be remapped.
- No online or PlayStation Network functionality.

Some Chapter 10 graphical issues still need further testing. See [Known Issues](docs/KNOWN_ISSUES.md) for more information.

## Compatibility testing and bug reports

To run the hardware compatibility tester:

1. Open the `compat` folder.
2. Run `run_compat.bat`.
3. Attach the generated `compat_report.zip` to a [compatibility report](https://github.com/SuperCatElvis/UntilDawn_PC/issues/new/choose).

AMD and Intel GPU results are particularly helpful.

If you encounter a bug, open a [GitHub issue](https://github.com/SuperCatElvis/UntilDawn_PC/issues) and include what happened, where it occurred, whether it happens again, and your CPU and GPU. A screenshot or video, `Logs\UntilDawn.log` and the compatibility report can also help.

Check logs for personal information before sharing them. **Do not upload or request copyrighted game files.**

## How it works

This project runs the original Until Dawn (2015) game code natively on Windows, using a custom-built runtime and Vulkan renderer.

The project includes:

- **Native PC Runtime:** Runs the original game's x86-64 code directly on Windows.
- **Custom Vulkan Renderer:** Handles the game's original graphics and shaders, with support for modern PC resolutions and hardware.
- **Shader Caching:** Reduces compilation times and stuttering during gameplay.
- **Modding and Development:** Provides access to the project's source code and technical documentation, allowing modders and developers to explore the game's systems, create modifications and contribute improvements.

The original game files are provided by your own legally obtained PS4 copy of Until Dawn.

For more technical information, including the game's architecture, memory, graphics and file formats, see the [Technical Overview](https://github.com/SuperCatElvis/UntilDawn_PC/blob/main/docs/TECHNICAL_OVERVIEW.md).

## Future development

The main priorities are **stability, performance, graphical accuracy and hardware compatibility**.

Other features being explored include more languages, a proper in-game settings menu, accessibility improvements, DLSS/FSR, freecam, first-person mode and better modding support. These are goals, not confirmed release features.

## Source code and license

This project is open source. The Windows runtime, renderer, shader translator and supporting tools are available in the [source folder](source/README.md).

See [LICENSE](LICENSE) and [Third-party Notices](THIRD_PARTY_NOTICES.md). The license covers this project's code, not the original game or assets.

## Disclaimer

Until Dawn PC Recompilation is an unofficial, non-commercial fan project. It is not affiliated with, endorsed by or supported by Sony Interactive Entertainment or Supermassive Games.

All original game files, characters, music, artwork and related intellectual property belong to their respective owners. **No original game files are provided.** You must own a legally obtained copy of the PS4 game.

This software is provided as-is, without warranty.
