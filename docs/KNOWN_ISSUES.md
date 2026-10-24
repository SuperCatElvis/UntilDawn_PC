
# Known issues — WIP 0.1.2

[← Back to README](../README.md)

This is an early test release, and several issues are still being investigated. Known problems are listed below. If you find something that isn't here, please [report it](../README.md#reporting-bugs).

---

## Reported by players

| Issue | Status |
| --- | --- |
| False "missing files" message with a long folder path | **Cause confirmed, workaround available.** Fix verified in a test build; planned for the next release. |
| Corrupted textures and black blocks on some AMD GPUs | **Under investigation.** A potential fix is being tested; not confirmed yet. |

### False "missing files" message with a long folder path

WIP 0.1.2 can incorrectly report game files as missing when the path to the game folder is too long, even though the files are present.

Windows' standard file functions, which WIP 0.1.2 uses, cannot open paths longer than 260 characters. The game's longest filename is approximately 197 characters on its own, so placing the game inside several nested folders can cause the full path to exceed this limit.

**Workaround:** Move the release or game folder to a shorter location. The path to the `Game` folder, including the final `\`, should be **62 characters or fewer**.

For example:

`C:\Games\UntilDawn_WIP_0.1.2\Game\`

A common cause is extracting the ZIP into a folder of the same name, resulting in a doubled directory structure:

`...\Downloads\UntilDawn_WIP_0.1.2\UntilDawn_WIP_0.1.2\Game\`

If you've configured `game_dir` in `Config\settings.ini`, the same path-length restriction applies to that folder.

**Fix:** Long-path support has been implemented and verified in a separate test build. The same directory that previously reported 40 missing files successfully passed the file checks and ran the game normally. This fix is planned for the next release.

### Corrupted textures and black blocks on some AMD GPUs

A player using an **AMD Radeon RX 9070 XT** reported graphical corruption in WIP 0.1.2, including noisy or speckled textures on clothing and black square blocks around the edges of characters and objects. Performance was otherwise good.

**Likely cause (not yet confirmed):** Some translated pixel shaders allow neighbouring 2×2 pixel groups to follow different execution paths before texture sampling. The original PS4 shader behaviour keeps these groups together. On some GPUs, the translated behaviour can produce incorrect texture-detail calculations and graphical artefacts.

This has not been observed on the NVIDIA development GPU.

**Status:** A test build has been prepared that preserves the 2×2 pixel groups on AMD and Intel GPUs, alongside additional AMD compatibility corrections.

The fix is still awaiting confirmation from the affected AMD hardware. This issue will only be marked resolved once the results have been verified.

If you're using an AMD GPU and encounter similar graphical problems, please submit screenshots and a [compatibility report](COMPATIBILITY_TESTING.md).

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
- **AMD Radeon:** Initial player reports have been received. An AMD Radeon RX 9070 XT has demonstrated graphical corruption in WIP 0.1.2 (see [Reported by players](#reported-by-players)).
- **AMD Ryzen:** The game has been run on a Ryzen 5 9600X, while a Ryzen 7 5700X has passed the CPU portion of the compatibility test. Wider AMD CPU compatibility has not yet been established.
- **Intel GPUs:** Intel Arc and Intel integrated graphics remain untested.

Please run the [compatibility tester](COMPATIBILITY_TESTING.md) on AMD and Intel systems and send your results, even if the game runs correctly.

## Game version and features

- Only **CUSA02636 with the 01.03 update** is supported. Other regions and versions are refused at startup.
- Online and PlayStation Network features are unavailable; the game reports the network as disconnected.
- Trophy unlocks are recorded locally in `savedata\` and the log, but there are currently no on-screen trophy notifications.
