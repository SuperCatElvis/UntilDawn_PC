
# Known issues — WIP 0.1.5

[← Back to README](../README.md)

This is still an early, work-in-progress test release, and several issues are still being investigated. Known problems are listed below. If you find something that isn't here, please [report it](../README.md#reporting-bugs).

---

## Not fixed yet: game memory that is occasionally overwritten

This is the main cause of the remaining crashes, and it is **not fixed in WIP 0.1.5**.

- Now and then a single byte of the game's memory is set to zero. In the cases examined it is the **lowest byte of
  a pointer at the start of a 4 KB memory page** — often an object's type (vtable) pointer, for example
  `…5fab0` becoming `…5fa00`. The game crashes later, when it uses that pointer: typical crash reports name the
  "Lump Loader" or a "JobSched" thread, or a character's animation data is damaged.
- It has been seen on the development PC and on players' PCs (Intel and AMD CPUs, NVIDIA GPUs), with WIP 0.1.3
  and 0.1.4, and again in the final test of 0.1.5 (twice in one session, detected by the safeguards, no crash).
  One recent case was in game memory that is not shared with the graphics driver, which rules out one of the
  earlier suspects (the way game memory is imported into Vulkan).
- The program's safeguards detect the known kinds of damage and repair some of them. The log then contains lines
  such as `EBOOT POINTER LOW BYTE CLEARED`. Please include `Logs\UntilDawn.log` (or `UntilDawn.previous.log` after a
  restart) when you report a crash — these lines help find the cause.
- Saves are kept; start the game again.

## Laptops with two GPUs

On a laptop with an **NVIDIA GeForce RTX 5050 Laptop GPU and AMD Radeon 740M integrated graphics**, where the
integrated GPU drives the screen, WIP 0.1.4 showed ([issue #2](https://github.com/SuperCatElvis/UntilDawn_PC/issues/2)):

- presenting a frame sometimes took 0.5–1 second;
- in several runs, a hang of about 8–10 seconds while presenting, after which the graphics driver reported
  "device lost" and the game ended (the GPU itself had no unfinished work: the hang is in passing frames between
  the two GPUs);
- flickering UI in the Status, Collectibles and character status menus.

This is not fixed yet. Things to try:

1. **Windows Settings → System → Display → Graphics:** add `UntilDawn.exe` and choose **High performance** (the
   NVIDIA GPU).
2. If the laptop has a **MUX switch / "discrete GPU only" mode** (in the NVIDIA Control Panel or the laptop maker's
   app), switch it on, so the NVIDIA GPU drives the screen directly.

Please report whether either helped, with `Logs\UntilDawn.log`.

## New in WIP 0.1.5

WIP 0.1.5 changes how the program uses memory, compiles compute shaders in parallel, sends less redundant state to
the graphics driver, checks game memory for changes more cheaply and reuses textures the game moves in memory (see
the [release notes](releases/v0.1.5-wip.md)). Every shader is translated exactly as in 0.1.4, and test frames give
the same images within the small run-to-run differences the GPU itself shows. These changes have so far been
**played on the development PC only** (NVIDIA RTX 3090 Ti, Intel Core i9-12900KF). Nothing in them is written for
one GPU make, but they are untested on AMD and Intel graphics. If WIP 0.1.5 crashes, freezes or stutters on your PC
where 0.1.4 did not, please [report it](../README.md#reporting-bugs) with `Logs\UntilDawn.log`.

The setting `renderer_thread = 0` in `Config\settings.ini` (since 0.1.4) runs the renderer on the game's thread
instead of its own; it is still available if the game freezes or stutters. It does not prevent the crashes from
overwritten game memory (both players who reported them used it). The first line of the log starting with
`settings:` shows which mode was used.

## Fixed and improved

| Issue | Status |
| --- | --- |
| High memory use (about 12.7 GB in RAM and 22 GB committed in the prologue on the development PC; the game's memory grew during play; PCs with 16 GB could run out) | **Improved in WIP 0.1.5:** at most about 8.2 GB in RAM and 17 GB committed on the same route; the game's memory grows by about 44 MB instead of 640 MB in ten minutes. |
| Shader-compilation freezes of 30 seconds and more on CPUs with few cores (compute shaders were compiled one after another) | **Improved in WIP 0.1.5:** compute shaders are compiled in parallel like the others. Freezes on the first play-through remain (see [Performance](#performance)). |
| Fixed video-memory budgets (6 GB of buffer copies and a 3 GB mirror on every GPU) | **Changed in WIP 0.1.5:** sized to the GPU (a quarter and an eighth of its video memory). |
| Hair looked hazy or see-through (fog and light shafts behind characters were drawn over it, e.g. Hannah in the prologue) | **Fixed in WIP 0.1.4** (confirmed in play). |
| The butterfly-effect tutorial, the movie that opens the prologue and the Chapter 1 bus ride could not be skipped | **Added in WIP 0.1.4**, with the same Skip prompt as the intro logos and the end credits (press any button, then Cross / Enter / Space). On the first two the prompt's "Skip" text shows as boxes (see Graphics). |
| Very low frame rate in demanding scenes (single digits in the prologue's "stranger" shots) | **Much improved in WIP 0.1.4:** about 21–25 FPS in those shots on the development PC (about 5 FPS in 0.1.3); about 22–27 FPS with 0.1.5. See [Performance](#performance). |

## Reported by players

| Issue | Status |
| --- | --- |
| False "missing files" message with a long folder path | **Fixed in WIP 0.1.3** (verified); still fixed in 0.1.5. |
| Corrupted textures, black blocks, missing foliage and stepped lighting on AMD GPUs | **Fixed in WIP 0.1.3** (confirmed on an RX 9070 XT by the player who reported it). 0.1.4 and 0.1.5 translate the shaders exactly as 0.1.3 does; they have not yet been played on AMD. |
| Very slow frame presentation, "device lost" and flickering menus on a laptop with two GPUs | **Open** — see [Laptops with two GPUs](#laptops-with-two-gpus). |
| Character's arm and the inspected item invisible during item inspection (GTX 1660 Ti, WIP 0.1.4) | **Open** — not reproduced yet ([issue #3](https://github.com/SuperCatElvis/UntilDawn_PC/issues/3)). |
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
robocopy "D:\path\to\your\CUSA02636 dump" "C:\Games\UntilDawn_WIP_0.1.5\Game" /E
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

- **Occasional crashes.** During a full play-through on the development PC (WIP 0.1.3), the game crashed twice and had to be restarted; saves were kept. Players have reported similar crashes with WIP 0.1.4. The cause is game memory that is occasionally overwritten — see [Not fixed yet](#not-fixed-yet-game-memory-that-is-occasionally-overwritten). The two changes WIP 0.1.4 made for it did not stop it.
- **Character deformation and vertex glitches.** Characters may briefly deform or have their geometry distort for a frame or two (seen, for example, on Sam in the prologue). This is most likely the same overwritten-memory problem as the crashes.
- **Collectibles disappearing.** Clues and totems may become invisible when picked up. A previously collected clue has also been observed reappearing.
- **Chapter 5 — Mike's lantern.** Mike's arm holding the lantern can become detached.

## Graphics

- **Prologue snow effects.** The falling snow effect may stop if the player remains stationary for an extended period.
- **Character introduction sequences.** Some lighting and camera effects do not render correctly.
- **Prologue — second "stranger" shot.** Fog and haze can disappear for a frame or two just after the stranger walks past. Rarely, lighting looks wrong for a frame or two elsewhere.
- **Prologue — Emily's eyes and the kitchen wall.** Emily's eyes can show light squares for a couple of seconds, and the wall on the right as the friends leave the kitchen for the prank can show dark square shadows (also in 0.1.4).
- **Item inspection.** On one PC (GTX 1660 Ti) the character's arm and the inspected item were invisible.
- **Menus on a laptop with two GPUs.** Flickering UI in the Status, Collectibles and character status menus (see [Laptops with two GPUs](#laptops-with-two-gpus)).
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
- **Shader compilation freezes.** Shaders are compiled for your graphics card when an area is first loaded. Expect stutters and freezes of several seconds, particularly during the beginning of a new game. WIP 0.1.5 compiles compute shaders in parallel too (before, one after another: a 38-second freeze reported with 0.1.4 on a 6-thread CPU held 30 seconds of them; with no shader cache the time spent waiting for compiles in two test scenes on the development PC fell by 16% and 24%). On CPUs with few cores freezes can still last 10–20 seconds or more. The game is not frozen permanently, so allow additional time for shader compilation. Subsequent runs benefit from the shader cache (`Cache\`); a cache from 0.1.4 stays valid.
- **Frame-rate drops.** Frame rates still vary with the scene; WIP 0.1.5 is similar to 0.1.4 (about 3% less renderer time per frame in test scenes, and memory checks that cost 0.3–0.4 ms less per frame). Measured on the development PC (RTX 3090 Ti, Core i9-12900KF): light scenes often reach 60 FPS, busy scenes (2,000–3,000 draws) about 35–45 FPS, and the heaviest scenes measured, the prologue's "stranger" shots, about 22–27 FPS (about 5 FPS with 0.1.3 in the same scene). Slower PCs will see lower numbers. In the heaviest scenes the renderer's own work on the CPU is the limit; spreading that work over several CPU cores is the next planned step.
- **Memory.** On the development PC the program uses at most about 8.2 GB of RAM in the prologue and commits up to about 17 GB. Much of the committed memory is reserved by the graphics driver and not in RAM, but Windows needs room for it: keep the page file switched on ("System managed"). 16 GB of RAM is recommended.
- **Higher frame-rate limits.** Frame-rate limits above 60 FPS are available but have not been thoroughly tested across all scenes. The default 60 FPS cap does not guarantee a consistent 60 FPS.

## Controllers

- **DualSense / DualShock 4 motion controls.** During "Don't Move" sequences, the motion indicator may continuously drift to the left, making it difficult to remain still.
- **DualSense / DualShock 4 vibration and light bar.** Vibration and light-bar functionality are not yet implemented. Xbox-style controllers support vibration.
- **Keyboard controls.** L3 / R3 are not currently available on the keyboard.

## Hardware

- **Tested development configuration:** Intel Core i9-12900KF, NVIDIA GeForce RTX 3090 Ti (drivers 617.42 and 621.01), Windows 11, 64 GB RAM. WIP 0.1.5 has so far been played only on this PC.
- **Played by players with WIP 0.1.4:** Intel Core i5-9400F + GeForce GTX 1660 Ti (occasional crashes, see above); AMD Ryzen 5 220 + GeForce RTX 5050 Laptop GPU with Radeon 740M (see [Laptops with two GPUs](#laptops-with-two-gpus)).
- **AMD Radeon:** an AMD Radeon RX 9070 XT showed graphical corruption in WIP 0.1.2; it is fixed in WIP 0.1.3 and confirmed on that card, where all compatibility checks pass (see [Reported by players](#reported-by-players)).
- **Other NVIDIA GPUs:** compatibility tests passed on the RTX 5070 Laptop GPU, RTX 3060 Ti, RTX 3080, RTX 5070 and RTX 3090 (see the [tested hardware](../README.md#tested-hardware) table).
- **AMD Ryzen:** The game has been run on a Ryzen 5 9600X, and the compatibility test has passed on Ryzen 7 5700G, Ryzen 5 7500F, Ryzen 7 5700X and Ryzen 7 5800X systems.
- **Intel GPUs:** Intel Arc and Intel integrated graphics remain untested.

Please run the [compatibility tester](COMPATIBILITY_TESTING.md) on AMD and Intel systems and send your results, even if the game runs correctly.

## Game version and features

- Only **CUSA02636 with the 01.03 update** is supported. Other regions and versions are refused at startup.
- Online and PlayStation Network features are unavailable; the game reports the network as disconnected.
- Trophy unlocks are recorded locally in `savedata\` and the log, but there are currently no on-screen trophy notifications.
