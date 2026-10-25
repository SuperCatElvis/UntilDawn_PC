
# Known issues — WIP 0.1.4

[← Back to README](../README.md)

This is an early test release, and several issues are still being investigated. Known problems are listed below. If you find something that isn't here, please [report it](../README.md#reporting-bugs).

---

## New in WIP 0.1.4: the renderer thread

The renderer (which turns the game's PS4 graphics commands into Vulkan) now runs on its own thread, alongside the
game, instead of inside the game's frame. This is a large part of the frame-rate gain in 0.1.4. It has so far been
played on the development PC only (NVIDIA RTX 3090 Ti, Intel Core i9-12900KF). The change is not specific to one GPU
make, and the shader programs are the same as in 0.1.3, but other PCs have not been tested yet.

**If WIP 0.1.4 crashes, freezes or stutters on your PC where 0.1.3 did not**, open `Config\settings.ini` and set:

```ini
renderer_thread = 0
```

The renderer then runs on the game's thread, exactly as in 0.1.3 (with the rest of 0.1.4's speed-ups). Please
[report](../README.md#reporting-bugs) whether that helped, with `Logs\UntilDawn.log` from both runs. The first
line of the log starting with `settings:` shows which mode was used.

## Fixed in WIP 0.1.4

| Issue | Status |
| --- | --- |
| Hair looked hazy or see-through (fog and light shafts behind characters were drawn over it, e.g. Hannah in the prologue) | **Fixed in WIP 0.1.4** (confirmed in play). On the PS4 a compute shader writes the hair's depth into the depth buffer; the renderer now keeps that depth. |
| The butterfly-effect tutorial, the movie that opens the prologue and the Chapter 1 bus ride could not be skipped | **Added in WIP 0.1.4**, with the same Skip prompt as the intro logos and the end credits (press any button, then Cross / Enter / Space). On the first two the prompt's "Skip" text shows as boxes (see Graphics). |
| Very low frame rate in demanding scenes (single digits in the prologue's "stranger" shots) | **Much improved in WIP 0.1.4:** about 21–25 FPS in those shots on the development PC (about 5 FPS in 0.1.3). See [Performance](#performance). |

## Reported by players

| Issue | Status |
| --- | --- |
| False "missing files" message with a long folder path | **Fixed in WIP 0.1.3** (verified); still fixed in 0.1.4. |
| Corrupted textures, black blocks, missing foliage and stepped lighting on AMD GPUs | **Fixed in WIP 0.1.3** (confirmed on an RX 9070 XT by the player who reported it). 0.1.4 translates the shaders exactly as 0.1.3 does; 0.1.4 itself has not yet been played on AMD. |
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
robocopy "D:\path\to\your\CUSA02636 dump" "C:\Games\UntilDawn_WIP_0.1.4\Game" /E
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

- **Occasional crashes.** During a full play-through on the development PC (WIP 0.1.3), the game crashed twice and had to be restarted; saves were kept. The cause is being investigated: some of the game's memory is occasionally overwritten (for example, character animation data and object tables). The program contains safeguards that detect and repair the most common cases, but not all of them. WIP 0.1.4 changes two things that may be involved (new game memory is registered with the graphics driver before the game uses it; game file reads go through the program's own buffers); whether this makes crashes rarer still has to be confirmed.
- **Character deformation and vertex glitches.** Characters may briefly deform or have their geometry distort when the shader table overfills. Closing and reopening the game resolves this issue most of the time.
- **Collectibles disappearing.** Clues and totems may become invisible when picked up. A previously collected clue has also been observed reappearing.
- **Chapter 5 — Mike's lantern.** Mike's arm holding the lantern can become detached.

## Graphics

- **Prologue snow effects.** The falling snow effect may stop if the player remains stationary for an extended period.
- **Character introduction sequences.** Some lighting and camera effects do not render correctly.
- **Prologue — second "stranger" shot.** Fog and haze can disappear for a frame or two just after the stranger walks past.
- **Character mouths.** The inside of mouths can render black in some close-ups.
- **Episodes menu.** A red flash can appear across the screen when navigating the Episodes menu.
- **End credits.** Text at the beginning of the end credits may appear vertically smeared.
- **Totem visions.** Watching a totem's vision does not always draw everything around the video:
  - the video plays but the rest of the totem screen (its background and UI) is missing, for example when a totem is rewatched from the menu after it played correctly the first time;
  - sometimes the whole UI disappears and only the character and the background remain.
- **Skip prompt text as boxes.** On the butterfly-effect tutorial and the movie that opens the prologue, the Skip prompt's "Skip" text shows as white boxes (the Cross button is correct and skipping works). The same can happen in the later "Previously on Until Dawn" recap clips. The game's own text in the same overlay movie is drawn in pieces at those times, so this is a renderer fault, not the prompt; it is being investigated.
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
- **Frame-rate drops.** WIP 0.1.4 is much faster than 0.1.3, but frame rates still vary with the scene. Measured on the development PC (RTX 3090 Ti, Core i9-12900KF) with WIP 0.1.4: light scenes often reach 60 FPS, busy scenes (2,000–3,000 draws) about 35–45 FPS, and the heaviest scenes measured, the prologue's "stranger" shots, about 21–25 FPS (about 5 FPS with 0.1.3 in the same scene). Slower PCs will see lower numbers. In the heaviest scenes the renderer's own work on the CPU is now the limit; spreading that work over several CPU cores is the next planned step.
- **Higher frame-rate limits.** Frame-rate limits above 60 FPS are available but have not been thoroughly tested across all scenes. The default 60 FPS cap does not guarantee a consistent 60 FPS.

## Controllers

- **DualSense / DualShock 4 motion controls.** During "Don't Move" sequences, the motion indicator may continuously drift to the left, making it difficult to remain still.
- **DualSense / DualShock 4 vibration and light bar.** Vibration and light-bar functionality are not yet implemented. Xbox-style controllers support vibration.
- **Keyboard controls.** L3 / R3 are not currently available on the keyboard.

## Hardware

- **Tested development configuration:** Intel Core i9-12900KF, NVIDIA GeForce RTX 3090 Ti (drivers 617.42 and 621.01), Windows 11, 64 GB RAM. WIP 0.1.4 has so far been played only on this PC.
- **AMD Radeon:** an AMD Radeon RX 9070 XT showed graphical corruption in WIP 0.1.2; it is fixed in WIP 0.1.3 and confirmed on that card, where all compatibility checks pass (see [Reported by players](#reported-by-players)).
- **Other NVIDIA GPUs:** compatibility tests passed on the RTX 5070 Laptop GPU, RTX 3060 Ti, RTX 3080, RTX 5070 and RTX 3090 (see the [tested hardware](../README.md#tested-hardware) table).
- **AMD Ryzen:** The game has been run on a Ryzen 5 9600X, and the compatibility test has passed on Ryzen 7 5700G, Ryzen 5 7500F, Ryzen 7 5700X and Ryzen 7 5800X systems.
- **Intel GPUs:** Intel Arc and Intel integrated graphics remain untested.

Please run the [compatibility tester](COMPATIBILITY_TESTING.md) on AMD and Intel systems and send your results, even if the game runs correctly.

## Game version and features

- Only **CUSA02636 with the 01.03 update** is supported. Other regions and versions are refused at startup.
- Online and PlayStation Network features are unavailable; the game reports the network as disconnected.
- Trophy unlocks are recorded locally in `savedata\` and the log, but there are currently no on-screen trophy notifications.
