
# Known issues — WIP 0.1.3

[← Back to README](../README.md)

This is an early test release, and several issues are still being investigated. Known problems are listed below. If you find something that isn't here, please [report it](../README.md#reporting-bugs).

---

## Reported by players

| Issue | Status |
| --- | --- |
| False "missing files" message with a long folder path | **Fixed in WIP 0.1.3** (verified). |
| Corrupted textures, black blocks, missing foliage and stepped lighting on AMD GPUs | **Fixed in WIP 0.1.3** (confirmed on an RX 9070 XT by the player who reported it). |
| One game file reported missing although the game was copied | Not a port bug: the copy skipped the file. See below. |

### False "missing files" message with a long folder path — fixed in WIP 0.1.3

WIP 0.1.2 could incorrectly report game files as missing when the path to the game folder was too long, even though the files were present.

Windows' standard file functions, which WIP 0.1.2 used, cannot open paths longer than 260 characters. The game's longest filename is approximately 197 characters on its own, so a game folder path of more than about 62 characters (for example a ZIP extracted into a folder of the same name, `...\Downloads\UntilDawn_WIP_0.1.2\UntilDawn_WIP_0.1.2\Game\`) pushed the full path over the limit.

**Fix (WIP 0.1.3):** long paths are now opened with Windows' extended-length path support. The same folder that reported 40 missing files with WIP 0.1.2 passed the file check and ran the game normally.

**If you stay on WIP 0.1.2:** move the release or game folder so that the path to the `Game` folder, including the final `\`, is **62 characters or fewer**, e.g. `C:\Games\UntilDawn_WIP_0.1.2\Game\`. The same limit applies to `game_dir` in `Config\settings.ini`.

### One game file reported missing although the game was copied

If the message names **one** file (often the very long `...mainlodge_garage_mountainbikeyellow_b001_c002_mat_0_textureset.corestream`), check whether it is really in your `Game\localcachepink\streams` folder. Windows Explorer shortens long names, and a similar-looking file with a shorter name sits next to it.

The usual cause is the **copy**: when the folder you copied the game *from* has a long path, Explorer or an unzip tool can skip the game's longest file names. Copy the game again with `robocopy` (it handles long paths and copies only what is missing), from a short folder:

```
robocopy "D:\path\to\your\CUSA02636 dump" "C:\Games\UntilDawn_WIP_0.1.3\Game" /E
```

### AMD GPUs: corrupted textures, black blocks, missing foliage and stepped lighting — fixed in WIP 0.1.3

A player using an **AMD Radeon RX 9070 XT** reported graphical corruption in WIP 0.1.2: noisy or speckled textures on clothing, black square blocks around characters and objects, trees and bushes partly missing, and lighting that ended in square steps (for example along a character's sleeve or tree edges). Performance was otherwise good.

**What was found** (with test builds and frame captures from that PC, replayed and compared on the development PC):

- One compute shader of the game could not be compiled on GPUs with 32 KB of shared memory (it needed 12 bytes more). It now fits exactly.
- Pixel shaders now keep each 2×2 pixel group together when they branch before texture sampling, as on the PS4.
- **Main cause:** the translated pixel shaders emulated a PS4 detail for every shader — the extra "helper" pixels a GPU runs next to the real ones (for texture detail calculations) started switched off and were tracked separately. On the NVIDIA development GPU this was harmless; on the AMD driver, whole 8×8-pixel blocks of alpha-tested pixels (leaves, needles, branches) were lost. The missing foliage then also put the per-tile lighting out of step. WIP 0.1.3 keeps that emulation only in the few shaders where it matters (95 of about 3,900) and runs the helper pixels normally everywhere else.

This is a change in the shader translation itself, not an AMD-only switch. On NVIDIA it changes only a few pixels on outlines.

**Status: confirmed.** On the same RX 9070 XT, WIP 0.1.3 now renders the same as NVIDIA — including the same remaining known issues listed below. If you have a different AMD GPU, reports with screenshots and a [compatibility report](COMPATIBILITY_TESTING.md) are still very welcome.

## Stability

- **Occasional crashes.** During a full play-through on the development PC, the game crashed twice and had to be restarted; saves were kept. The cause is being investigated: some of the game's memory is occasionally overwritten (for example, character animation data and object tables). The program contains safeguards that detect and repair the most common cases, but not all of them.
- **Character deformation and vertex glitches.** Characters may briefly deform or have their geometry distort when the shader table overfills. Closing and reopening the game resolves this issue most of the time.
- **Collectibles disappearing.** Clues and totems may become invisible when picked up. A previously collected clue has also been observed reappearing.
- **Chapter 5 — Mike's lantern.** Mike's arm holding the lantern can become detached.

## Graphics

- **Prologue snow effects.** The falling snow effect may stop if the player remains stationary for an extended period.
- **Character introduction sequences.** Some lighting and camera effects do not render correctly.
- **Character mouths.** The inside of mouths can render black in some close-ups.
- **Episodes menu.** A red flash can appear across the screen when navigating the Episodes menu.
- **End credits.** Text at the beginning of the end credits may appear vertically smeared.
- **Previously on Until Dawn recaps.** The on-screen **SKIP** text may appear as blocks during later recap videos.
- **Cross (X) button and subtitles** can occasionally appear with a glitchy appearance
- Small graphical differences from the original PS4 version may occur elsewhere. Please report anything unusual with a screenshot.

## Audio

- **Prologue — Dr Hill session.** Some audio plays out of sync or is delayed during the Dr Hill session.
- **Prologue — Hannah and Beth chase.** Audio can become delayed during the chase sequence.

## Gameplay

- **Dr Hill — Session 2.** The fear book can repeatedly display the same fear images rather than the expected selections.

## Performance

- **Initial loading black screens.** When first launching the game, a black screen may appear before the main menu. A similar delay can occur when starting a new story after the opening lodge shots. These are caused by shaders being generated and compiled. Loading the same scene again, or selecting New Story after the shaders have been generated, is generally much faster. Improvements are planned for future releases.
- **Shader compilation freezes.** Shaders are compiled for your graphics card when an area is first loaded. Expect stutters and occasional freezes lasting **10–20 seconds**, particularly during the beginning of a new game. On the development PC, a fresh installation produced freezes of approximately 12 and 17 seconds during the prologue. The game may not be frozen permanently, so allow additional time for shader compilation. Subsequent runs benefit from the shader cache (`Cache\`).
- **Frame-rate drops.** Frame rates vary significantly depending on the scene. Many sections reach 30–60 FPS on the development PC, but demanding sections can drop into the **single digits**, even on powerful hardware. Performance improvements are planned for future releases, following stability and graphical correctness work.
- **Higher frame-rate limits.** Frame-rate limits above 60 FPS are available but have not been thoroughly tested across all scenes. The default 60 FPS cap does not guarantee a consistent 60 FPS.

## Controllers

- **DualSense / DualShock 4 motion controls.** During "Don't Move" sequences, the motion indicator may continuously drift to the left, making it difficult to remain still.
- **DualSense / DualShock 4 vibration and light bar.** Vibration and light-bar functionality are not yet implemented. Xbox-style controllers support vibration.
- **Keyboard controls.** L3 / R3 are not currently available on the keyboard.

## Hardware

- **Tested development configuration:** Intel Core i9-12900KF, NVIDIA GeForce RTX 3090 Ti (drivers 617.42 and 621.01), Windows 11, 64 GB RAM.
- **AMD Radeon:** an AMD Radeon RX 9070 XT showed graphical corruption in WIP 0.1.2; it is fixed in WIP 0.1.3 and confirmed on that card, where all compatibility checks pass (see [Reported by players](#reported-by-players)).
- **Other NVIDIA GPUs:** compatibility tests passed on the RTX 5070 Laptop GPU, RTX 3060 Ti, RTX 3080, RTX 5070 and RTX 3090 (see the [tested hardware](../README.md#tested-hardware) table).
- **AMD Ryzen:** The game has been run on a Ryzen 5 9600X, and the compatibility test has passed on Ryzen 7 5700G, Ryzen 5 7500F, Ryzen 7 5700X and Ryzen 7 5800X systems.
- **Intel GPUs:** Intel Arc and Intel integrated graphics remain untested.

Please run the [compatibility tester](COMPATIBILITY_TESTING.md) on AMD and Intel systems and send your results, even if the game runs correctly.

## Game version and features

- Only **CUSA02636 with the 01.03 update** is supported. Other regions and versions are refused at startup.
- Online and PlayStation Network features are unavailable; the game reports the network as disconnected.
- Trophy unlocks are recorded locally in `savedata\` and the log, but there are currently no on-screen trophy notifications.
