# Known issues — v0.3.1

[← Back to README](../README.md)

This is still an early, work-in-progress test release, and several issues are still being investigated. Known problems are listed below. If you find something that isn't here, please [report it](../README.md#reporting-bugs).

---

## Not fixed yet: game memory that is occasionally overwritten

This is the main cause of the remaining crashes, and it is **not fixed in v0.3.0**.

- Now and then a single byte of the game's memory is set to zero — in the cases examined, the **lowest byte of a
  pointer**, often an object's type (vtable) pointer, for example `…5fab0` becoming `…5fa00`. The game crashes
  later, when it uses that pointer: typical crash reports name the "Lump Loader" or a "JobSched" thread, or a
  character's animation data is damaged.
- It has been seen on the development PC and on players' PCs (Intel and AMD CPUs, NVIDIA GPUs) with every version
  so far, including v0.3.0.
- **What is known now.** With hardware watchpoints on the damaged pages, the writer was caught: it is **the game's
  own code** writing the last member of an object it has just created, whose memory **overlaps another object that
  is still in use** — two of the game's memory allocations overlap, and the last byte of the new one lands on the
  old one's type pointer. One race of this kind was found in the game's lock-free arena allocator (two threads
  allocating from the same arena at once); 0.2.0 puts a lock around it, but the damage still occurs, so there are
  other overlapping allocations still to find. (Earlier suspects, such as the way game memory is imported into
  Vulkan, were shown with dedicated test memory not to be what damages the game's memory.)
- **Measured for v0.3.0 without playing:** the program can now start any chapter from the Episodes menu and run it
  unattended. In 44 such runs of about 2½ minutes each, the safeguards detected the damage 34 times (about 0.8 per
  run, in 21 of the runs), mostly on a handful of object types, and one run crashed from it (the game's own memory
  allocator stopped after finding its bookkeeping damaged). Earlier runs with and without GPU coherence showed the
  same rate, so coherence neither causes nor fixes it. Most detected damage does not lead to a crash.
- **One race fixed in v0.3.0, not the main cause.** The program's version of the C++ "construct this shared object only
  once" routine let two threads both construct the same object if they reached it at the same moment. This was seen to
  happen in about one run in four, at several places in the game's code, and once the damage appeared within the
  same frame. v0.3.0 has the corrected version (the second thread waits, as on the PS4). In 22 runs with it
  against 22 without, the detected damage was lower (13 events against 21), but the difference is within chance, and
  the most common damaged object type was not affected. At most it is one of several causes; other overlapping
  allocations remain to be found.
- The program's safeguards detect the known kinds of damage and repair some of them. The log then contains lines
  such as `EBOOT POINTER LOW BYTE CLEARED`. Please include `Logs\UntilDawn.log` (or `UntilDawn.previous.log` after a
  restart) when you report a crash — these lines help find the cause.
- Saves are kept; start the game again.

## Laptops with two GPUs

On a laptop with an **NVIDIA GeForce RTX 5050 Laptop GPU and AMD Radeon 740M integrated graphics**, where the
integrated GPU drives the screen, WIP 0.1.4 showed ([issue #2](https://github.com/SuperCatElvis/UntilDawn_PC/issues/2)):

- presenting a frame sometimes took 0.5–1 second;
- in several runs, a hang of about 8–10 seconds while presenting, after which the graphics driver reported
  "device lost" and the game ended;
- flickering UI in the Status, Collectibles and character status menus.

**What WIP 0.2.0 changes.** The logs showed where the hang came from: when the program could not get an image from
the display in time (the wait timed out), it carried on as if it had one — it drew into it, made the GPU wait for a
signal that never came and presented an image it did not own, so the GPU queue waited for ever until the driver
gave up ("device lost"). Now:

- a frame whose image does not arrive within 250 ms is skipped instead;
- with `present_mode = auto` (the default), if presenting is slow three times within a short while, the program
  switches once from the `mailbox` to the `fifo` presentation mode, which is the most widely supported;
- new settings `present_mode` (`auto`, `fifo`, `mailbox`, `immediate`) and `gpu` (`auto`, a number, or part of a
  GPU's name, e.g. `gpu = 5050`) — see [INSTALLATION.md](INSTALLATION.md#5-settings-configsettingsini).

This has been checked on the development PC (normal play is unchanged, including moving, minimising and switching
away from the window; the switch to `fifo` works when forced; a late image never happened there, so the skip itself
has not been seen in action) but **not yet on a laptop with two GPUs**. The slow presenting itself, which comes from passing frames between the two
GPUs, and the flickering menus may remain. Things to try:

1. **Windows Settings → System → Display → Graphics:** add `UntilDawn.exe` and choose **High performance** (the
   NVIDIA GPU).
2. If the laptop has a **MUX switch / "discrete GPU only" mode** (in the NVIDIA Control Panel or the laptop maker's
   app), switch it on, so the NVIDIA GPU drives the screen directly.
3. `present_mode = fifo` in `Config\settings.ini`.

Please report what happened, with `Logs\UntilDawn.log`.

## New in v0.3.1

v0.3.1 is a focused graphics and shader update on top of 0.3.0, described in the [release notes](releases/v0.3.1.md).
It has been tested on the development PC only (NVIDIA RTX 3090 Ti), on recorded scenes from across the game.

- **Fixed: see-through and distance-faded surfaces.** Pixel shaders were given 1 ÷ distance where the PS4 hardware gives
  the distance itself. Fixed with it: the black icicles on the lodge in the prologue, the solid liquid that hid the
  "strange skull" clue in the sanatorium (checked against a PS4 screenshot), the squares on the cable car's frosted
  windows in Chapter 1, and a pale glow along the edges of hair. The black icicles reported in Matt and Emily's scenes
  and Sam's glowing hair in the psycho chase are most likely the same fault; not re-checked in those scenes yet.
- **Fixed: draws left out in parts of Chapters 5 and 6** on NVIDIA graphics (shaders that need 33 resource slots).
  Not yet seen in play with the fix.
- **Bigger shader list:** 6,797 shader combinations (0.3.0: 4,440), recorded in a full playthrough. The first start
  prepares them for about 2¼ minutes on the development PC (0.3.0's list: 1 minute 45 seconds).
- **Reported, not yet investigated:** the lantern jumping when it hits a table (Chapter 5), the skin of Hannah's ghost,
  and strong coloured lens-flare spots from a bright window in the sanatorium boiler room. The blue light by the
  sanatorium door matches a PS4 screenshot of the same spot.

## New in v0.3.0

v0.3.0 (which replaces the planned 0.2.2) is described in the [release notes](releases/v0.3.0.md). It has been tested
on the development PC only (NVIDIA RTX 3090 Ti), largely with automated runs: every chapter started from the
Episodes menu with and without GPU coherence, 46 recorded scenes replayed both ways, and the coherence self-tests.

- **GPU coherence (new, on by default)** keeps the game's CPU-side memory and the copies on the graphics card
  consistent. It fixes the lake water disappearing and characters' geometry stretching across the screen in
  Chapter 10. **If something looks or runs worse with it**, set `gpu_coherence = 0` in `Config\settings.ini` (or
  start with `--no-gpu-coherence`) and please report both results with `Logs\UntilDawn.log`; the first lines of the
  log say whether it was on.
  - If it cannot do its work safely, the game **stops with a message** saying so and how to turn it off (exit code 22),
    rather than going on with memory that may be wrong. This never happened in testing; software that hooks into
    other programs (some security or overlay tools) is the most likely cause if it does.
  - If it cannot be set up at all, the log says so and the game runs without it (the two faults above can then
    return).
  - It has not been tested on AMD or Intel graphics yet.
- **Bigger shader list:** `pipelines.udm` holds 4,440 shader combinations (0.2.1: 1,594), recorded in play sessions from the
  prologue to the middle of Chapter 1, from the start of every chapter and from parts of Chapter 10. In automated starts of chapters 0–10, 98.9% of the
  combinations used were ready before the game started (the 2,525-combination list prepared for 0.2.2: 78.7%), and compiling during play fell from
  about 135 seconds to about 3.5 seconds in all. The first start prepares them for about 2½ minutes on the
  development PC; later starts take about half a second. Scenes further into a chapter than its opening may still
  compile some shaders the first time.
- **Fixed:** a crash when loading a chapter (`FIXED mapping moved` in the log, Lump Loader thread).
- **Fixed:** the letterbox bars of the character introductions stopped short of the right and bottom edges (a
  32-pixel strip of the scene showed there).
- **Fixed:** some small textures that the game rewrites in place kept showing old contents.
- **Optional extra: Halloween Hunt 2015** (`halloween_hunt = 1` in `Config\settings.ini` or `--halloween-hunt`; off by
  default) brings back the game's own Halloween screen and hidden pumpkins from its 28-31 October 2015 event; only the
  game's Halloween date checks see 31 October 2015. A thin yellow outline at the left edge of the Halloween screen (not
  shown on the PS4) is left out. Tested briefly (the screen and a pumpkin in the mines); not every pumpkin has been
  looked for.
- **Remaining pauses on the prologue / Chapter 1 route** are mostly scene changes that load many new textures at once
  (for example about 0.4 seconds at the door with Jess, Sam and Chris), not shader compilation.

## New in v0.2.1

v0.2.1 adds **shader preparation** and keeps every fix of 0.2.0 (see the [release notes](releases/v0.2.1.md)). It has
so far been **tested on the development PC only** (NVIDIA RTX 3090 Ti): with replays of 17 recorded scenes and a fresh
first start played through brightness, controls, intro and menu. It is untested on AMD and Intel graphics.

- **The first start takes longer:** a "Preparing shaders" screen compiles the shaders in `pipelines.udm` before the
  brightness screen (about 30–40 seconds on the development PC; longer with fewer CPU cores). Esc or Circle skips it;
  what is skipped is compiled during play, as before. Later starts take under a second unless the graphics driver
  changed. `shader_preparation = 0` in `Config\settings.ini` turns it off.
- **Not every scene is in the list yet.** Scenes nobody has recorded (for example the main-menu background of later
  chapters) still pause to compile their shaders the first time they appear, and are prepared from the next start
  on. In replays of scenes left out of the list on purpose, 85–88% of their shaders were ready.
- **A second form of a few shaders.** Some shaders read data that the graphics card itself writes every frame. Whether
  that data can be read on the CPU depends on timing, so such a shader can come out in either of two (correct) forms,
  and the second one is compiled once when it first appears. Not changed in v0.2.1: it would mean changing how every
  shader is translated.
- **Fixed in v0.2.1:** a rare hang of the renderer while new textures were converted on its helper threads (one
  helper could drop a piece of work, and the renderer then waited for it forever). Found in a test replay; the same
  hang was possible in play.
- If v0.2.1 crashes, freezes or stutters where 0.2.0 did not, please [report it](../README.md#reporting-bugs) with
  `Logs\UntilDawn.log`; `shader_preparation = 0` is worth a try to see whether the preparation is involved.

## New in WIP 0.2.0

WIP 0.2.0 is mostly fixes (see the [release notes](releases/v0.2.0-wip.md)). These changes have so far been
**played on the development PC only** (NVIDIA RTX 3090 Ti, Intel Core i9-12900KF). Nothing in them is written for
one GPU make, but they are untested on AMD and Intel graphics. If WIP 0.2.0 crashes, freezes or stutters on your PC
where 0.1.5 did not, please [report it](../README.md#reporting-bugs) with `Logs\UntilDawn.log`.

- **Every shader is translated anew** (the range of memory the GPU can see was widened, and the character shaders
  changed), so after updating, the first-play-through shader pauses return once; the old shader cache no longer matches.
- The setting `renderer_thread = 0` in `Config\settings.ini` (since 0.1.4) runs the renderer on the game's thread
  instead of its own; it is still available if the game freezes or stutters. It does not prevent the crashes from
  overwritten game memory. The first line of the log starting with `settings:` shows which mode was used.
- **Long sessions:** 0.2.0 contains a fix for a problem that started after about 25–30 minutes, but has not yet been
  played for longer than about ten minutes at a time. If something goes wrong only after a long time, please say
  how long you had been playing.

## Fixed and improved

| Issue | Status |
| --- | --- |
| Black icicles; the liquid of the "strange skull" clue solid; squares on the cable car's frosted windows; a pale glow along hair edges | **Fixed in v0.3.1** (pixel shaders get the distance the PS4 hardware gives; checked on 14 recorded scenes and against a PS4 screenshot of the clue) |
| Draws left out in parts of Chapters 5 and 6 (NVIDIA: shaders needing 33 resource slots) | **Fixed in v0.3.1** (checked on recorded scenes; not yet seen in play) |
| Chapter 10: huge stretched triangles across the screen over Sam and the scene (the Josh section) | **Fixed in v0.3.0** (GPU coherence; checked on the recorded scene and with the automated tests, not yet in a full play-through). The graphics card had moved character model data, and the CPU then copied the hair's index data from the old, outdated place. |
| Lake water missing or flickering, later in a session (seen in Chapter 10) | **Fixed in v0.3.0** (GPU coherence; checked on the recorded scenes and in automated runs of the scene change where it happens, not yet in a full play-through). A few hundred outdated bytes were copied over the ocean simulation's state on the graphics card; the simulation then produced invalid values for the rest of the session. |
| A crash while loading a chapter (`FIXED mapping moved`, Lump Loader thread; for example loading Chapter 8) | **Fixed in v0.3.0** (no crash of any kind in about 1.4 million frames of test runs since, most of them automated chapter starts). |
| Small textures that the game rewrites in place showing old contents | **Fixed in v0.3.0:** textures up to 1 MB are compared in full when the quick sampled check reports no change. |
| The character's arm and hand, and the clue or totem in it, invisible when picking up or inspecting an item ([issue #3](https://github.com/SuperCatElvis/UntilDawn_PC/issues/3); seen on all devices) | **Fixed in WIP 0.2.0** (confirmed in play). Two tables of floating-point limits the game reads from the PS4's C library (`_Flt` / `_Dbl`: the smallest, largest and "epsilon" values) were handed to the game as zeros. With a smallest value of 0, a check in the arm's two-bone IK no longer caught a zero-length direction, divided by zero and filled the arm's bones with invalid numbers, which then stayed invalid. Found by replaying the exact recorded IK call outside the game. Every other place in the game's code that reads these tables (142 reads of `_Flt` and 3 of `_Dbl` in the main executable) now gets the right values too. |
| Chapter 5: Mike's arm holding the lantern coming apart from him | **Fixed in WIP 0.2.0** (confirmed in play): the same cause as the collectible arm (the zero `_Flt` / `_Dbl` tables in the arm IK), fixed by the same change. |
| Black mouths in close-ups, in Chapter 2 and later after playing the prologue | **Fixed in WIP 0.2.0** (confirmed in play). Identical copies of a character shader at different addresses shared one translation that still read the first copy's constant table — memory the game had since reused. |
| Striped or garbled X (Cross) button prompts | **Fixed in WIP 0.2.0** (confirmed in play): a texture layout rule sheared some UI textures. |
| A white rectangle / flash on the character introduction name cards | **Fixed in WIP 0.2.0** (confirmed in play). |
| The aiming reticle (gun tutorial) shown as a flickering red square | **Changed in WIP 0.2.0** (checked on the frame capture of the problem, not yet in play): a UI target was drawn into an older, shorter image at the same address, and the reticle then read a still older, larger one. |
| Freezes of a second or more during play | **Fixed in WIP 0.2.0** (no such freeze in the test sessions since): a line written to the log could block the game or the renderer when Windows held the write (1.2 s in one measured frame). Log lines are now written by a separate thread. |
| A random crash right at start-up (`FIXED mapping moved` in the log) | **Fixed in WIP 0.2.0:** Windows sometimes placed a thread's stack inside the address range the game uses for its heaps; that range is now kept free from the start. |
| After about 25–30 minutes of play: the GPU no longer sees game data loaded after that time (most likely behind every character "exploding" at once and missing or broken graphics late in long sessions); memory committed for the game's heaps growing during long sessions | **Changed in WIP 0.2.0, not yet confirmed** (found from the logs; 0.2.0 has not yet been played for longer than about ten minutes at a time): the game's heaps had moved past the range of memory the GPU can see. They are now placed closer together (`heap_slot_mb = 512`), all inside that range, and a released heap is reused at once (`heap_slot_reuse = 1`). |
| Music that kept playing after a skipped movie (for example after skipping the Chapter 1 bus ride) | **Changed in WIP 0.2.0, not yet confirmed in play** — a skip also stops the sounds posted since the movie started. |
| Hangs ending in "device lost" on a laptop with two GPUs | **Changed in WIP 0.2.0, untested on such a laptop** — see [Laptops with two GPUs](#laptops-with-two-gpus). |
| High memory use (about 12.7 GB in RAM and 22 GB committed in the prologue on the development PC) | **Improved in WIP 0.1.5:** at most about 8.2 GB in RAM and 17 GB committed on the same route. |
| Shader-compilation freezes of 30 seconds and more on CPUs with few cores | **Improved in WIP 0.1.5:** compute shaders are compiled in parallel like the others. Pauses on the first play-through remain (see [Performance](#performance)). |
| Hair looked hazy or see-through | **Fixed in WIP 0.1.4** (confirmed in play). |
| The butterfly-effect tutorial, the movie that opens the prologue and the Chapter 1 bus ride could not be skipped | **Added in WIP 0.1.4.** On the first two the prompt's "Skip" text shows as boxes (see Graphics). |
| Very low frame rate in demanding scenes (single digits in the prologue's "stranger" shots) | **Much improved in WIP 0.1.4:** about 21–25 FPS in those shots on the development PC (about 5 FPS in 0.1.3); about 22–27 FPS since 0.1.5. See [Performance](#performance). |

## Reported by players

| Issue | Status |
| --- | --- |
| False "missing files" message with a long folder path | **Fixed in WIP 0.1.3** (verified); still fixed. |
| Corrupted textures, black blocks, missing foliage and stepped lighting on AMD GPUs | **Fixed in WIP 0.1.3** (confirmed on an RX 9070 XT by the player who reported it). 0.1.4 to 0.2.0 have not yet been played on AMD. |
| Very slow frame presentation, "device lost" and flickering menus on a laptop with two GPUs | **Partly addressed in WIP 0.2.0, untested there** — see [Laptops with two GPUs](#laptops-with-two-gpus). |
| Character's arm and the inspected item invisible during item inspection (all devices; [issue #3](https://github.com/SuperCatElvis/UntilDawn_PC/issues/3)) | **Fixed in WIP 0.2.0** — the cause was the separate collectible rig turning invisible (its arm bones became invalid numbers); see [Fixed and improved](#fixed-and-improved). |
| One game file reported missing although the game was copied | Not a port bug: the copy skipped the file. See below. |
| Totem screen flashes red when totem plays | This is thought to be tied to the episode selector doing the same thing|

### False "missing files" message with a long folder path — fixed in WIP 0.1.3

WIP 0.1.2 could incorrectly report game files as missing when the path to the game folder was too long, even though the files were present.

Windows' standard file functions, which WIP 0.1.2 used, cannot open paths longer than 260 characters. The game's longest filename is approximately 197 characters on its own, so a game folder path of more than about 62 characters (for example a ZIP extracted into a folder of the same name, `...\Downloads\UntilDawn_WIP_0.1.2\UntilDawn_WIP_0.1.2\Game\`) pushed the full path over the limit.

**Fix (WIP 0.1.3):** long paths are now opened with Windows' extended-length path support. The same folder that reported 40 missing files with WIP 0.1.2 passed the file check and ran the game normally.

**If you stay on WIP 0.1.2:** move the release or game folder so that the path to the `Game` folder, including the final `\`, is **62 characters or fewer**, e.g. `C:\Games\UntilDawn_WIP_0.1.2\Game\`. The same limit applies to `game_dir` in `Config\settings.ini`.

### One game file reported missing although the game was copied

If the message names **one** file (often the very long `...mainlodge_garage_mountainbikeyellow_b001_c002_mat_0_textureset.corestream`), check whether it is really in your `Game\localcachepink\streams` folder. Windows Explorer shortens long names, and a similar-looking file with a shorter name sits next to it.

The usual cause is the **copy**: when the folder you copied the game *from* has a long path, Explorer or an unzip tool can skip the game's longest file names. Copy the game again with `robocopy` (it handles long paths and copies only what is missing), from a short folder:

```
robocopy "D:\path\to\your\CUSA02636 dump" "C:\Games\UntilDawn_0.2.0\Game" /E
```

### AMD GPUs: corrupted textures, black blocks, missing foliage and stepped lighting — fixed in WIP 0.1.3

A player using an **AMD Radeon RX 9070 XT** reported graphical corruption in WIP 0.1.2: noisy or speckled textures on clothing, black square blocks around characters and objects, trees and bushes partly missing, and lighting that ended in square steps (for example along a character's sleeve or tree edges). Performance was otherwise good.

**What was found** (with test builds and frame captures from that PC, replayed and compared on the development PC):

- One compute shader of the game could not be compiled on GPUs with 32 KB of shared memory (it needed 12 bytes more). It now fits exactly.
- Pixel shaders now keep each 2×2 pixel group together when they branch before texture sampling, as on the PS4.
- **Main cause:** the translated pixel shaders emulated a PS4 detail for every shader — the extra "helper" pixels a GPU runs next to the real ones (for texture detail calculations) started switched off and were tracked separately. On the NVIDIA development GPU this was harmless; on the AMD driver, whole 8×8-pixel blocks of alpha-tested pixels (leaves, needles, branches) were lost. The missing foliage then also put the per-tile lighting out of step. WIP 0.1.3 keeps that emulation only in the few shaders where it matters (95 of about 3,900) and runs the helper pixels normally everywhere else.

This is a change in the shader translation itself, not an AMD-only switch. On NVIDIA it changes only a few pixels on outlines.

**Status: confirmed.** On the same RX 9070 XT, WIP 0.1.3 renders the same as NVIDIA — including the same remaining known issues listed below. If you have a different AMD GPU, reports with screenshots and a [compatibility report](COMPATIBILITY_TESTING.md) are still very welcome.

## Stability

- **Occasional crashes.** During a full play-through on the development PC (WIP 0.1.3), the game crashed twice and had to be restarted; saves were kept. Players have reported similar crashes with WIP 0.1.4. The cause is game memory that is occasionally overwritten — see [Not fixed yet](#not-fixed-yet-game-memory-that-is-occasionally-overwritten).
- **Character deformation and vertex glitches.** Characters may briefly deform or have their geometry distort for a frame or two (seen, for example, on Sam in the prologue). This is most likely the same overwritten-memory problem as the crashes. (The case where every character exploded at once late in a long session most likely had another cause, changed in 0.2.0.)
- **Collectibles.** Fixed in 0.2.0: the arm, hand and picked-up item vanishing. A previously collected clue has been observed reappearing (not re-checked with 0.2.0).

## Graphics

- **R2 choice cards and subtitles.** The R2 choice cards can show flickering spots, and subtitles (for example in Chapter 1) can flicker.
- **Character introduction sequences.** Some lighting and camera effects do not render correctly. *Fixed in v0.3.0:* the letterbox bars stopped short of the right and bottom edges, leaving a strip of the scene visible.
- **Lighting in Chapter 1.** *Fixed in v0.3.1:* pale squares on the cable car's frosted windows. Still open: the lamp at the cable car station can switch on and off as the camera moves; a short bright fog flash on Sam's path to the station; characters' skin and lips look matte compared with the PS4. Being investigated.
- **Chris's glasses.** A slight lighting effect on his glasses was seen once with 0.2.0; it may have been there before (not yet checked).
- **Main menu.** The character on the main menu animates much faster than it should; gameplay runs at the right speed.
- **Prologue snow effects.** The falling snow effect may stop if the player remains stationary for an extended period, and stops when Beth and Hannah fall from the cliff. In a capture at the cliff the game itself no longer drew the snow (the renderer received no snow draws), so the cause is probably in how the port runs the game's code rather than in the renderer; being investigated.
- **Prologue icicles.** *Fixed in v0.3.1:* icicles on the lodge during the stranger cutscene appeared solid black.
- **Prologue — second "stranger" shot.** Fog and haze can disappear for a frame or two just after the stranger walks past. Rarely, lighting looks wrong for a frame or two elsewhere.
- **Prologue — Emily's eyes and the kitchen wall.** Emily's eyes can show light squares for a couple of seconds, and the wall on the right as the friends leave the kitchen for the prank can show dark square shadows (reported with 0.1.4 and 0.1.5; not re-checked with 0.2.0).
- **Menus on a laptop with two GPUs.** Flickering UI in the Status, Collectibles and character status menus (see [Laptops with two GPUs](#laptops-with-two-gpus)).
- **Episodes menu.** A red flash can appear across the screen when navigating the Episodes menu.
- **Chapter 10.** Seen with test builds before v0.3.0 and not yet re-checked in play: a black screen in the scene with
  Mike and Sam (once, after the long route through the chapter; the same scene loaded directly was fine), red/blue
  frames in the Matt and Jess scene (not seen again after the small-texture fix), and exploded character geometry in
  other sections (possibly the same fault GPU coherence fixes). Please report them with `Logs\UntilDawn.log` if they
  still happen.
- **End credits.** Text at the beginning of the end credits may appear vertically smeared.
- **Totem visions.** Watching a totem's vision does not always draw everything around the video:
  - the video plays but the rest of the totem screen (its background and UI) is missing, for example when a totem is rewatched from the menu after it played correctly the first time;
  - sometimes the whole UI disappears and only the character and the background remain.
- **Skip prompt text as boxes.** On the butterfly-effect tutorial and the movie that opens the prologue, the Skip prompt's "Skip" text shows as white boxes (the Cross button is correct and skipping works). The same can happen in the later "Previously on Until Dawn" recap clips. The game's own text in the same overlay movie is drawn in pieces at those times, so this is a renderer fault, not the prompt; it is being investigated.
- Small graphical differences from the original PS4 version may occur elsewhere. Please report anything unusual with a screenshot.

## Audio

- **Prologue — Dr Hill session.** Some audio plays out of sync or is delayed during the Dr Hill session.
- **Prologue — Hannah and Beth chase.** Audio can become delayed during the chase sequence.

## Gameplay

- **Dr Hill — Session 2.** The fear book can repeatedly display the same fear images rather than the expected selections.

## Performance

- **Shader-compilation pauses.** The first time new content appears, the graphics driver compiles the shader combinations it needs. With an empty shader cache (a new install), the development PC (Core i9-12900KF, 24 threads) paused once for about 12 seconds and a few times for 3–8 seconds in the first minutes of the prologue, mostly while large compute shaders were compiled; once the cache holds an area, new content costs up to about 2 seconds (in one measured case, 12 pipelines compiled together took 2 seconds, and a texture upload in the same frame had to wait for them). CPUs with fewer cores pause longer; 10–20 seconds or more have been reported with earlier versions. WIP 0.2.0 does not improve this yet. Each pause happens once: the shader cache (`Cache\`) removes it on later runs. After updating from 0.1.5 the pauses return once, because every shader is translated anew. The game is not frozen permanently; allow time for it.
  *New in v0.2.1 (shader preparation):* the shaders in `pipelines.udm` and those of earlier sessions are compiled before the brightness screen (about 30–40 seconds on the development PC on a first start, under a second afterwards). On the development PC the first-start sequence (brightness, controls, intro, menu) then compiled nothing during play, and replays of scenes that were left out of `pipelines.udm` had 85–88% of their shaders ready (about 90% less waiting). Scenes nobody has recorded yet (for example the main-menu background of later chapters) still compile their shaders the first time; they are prepared from the next start on. A few shaders that read data the graphics card writes itself can come out in one of two forms depending on timing, so a second form may still be compiled once during play.
  *New in v0.3.0:* the list covers the opening of every chapter (4,440 combinations); see [New in v0.3.0](#new-in-v030).
- **Initial loading black screens.** When first launching the game, a black screen of about a second may appear before the main menu. A similar delay can occur when starting a new story after the opening lodge shots. The logs show this is mostly the game loading the scene and the textures being set up for it (in one measured start: a 1.2-second frame, about 0.3 s of it texture uploads, no shader compilation). In WIP 0.2.0 a first start also compiled the menu's shaders at that point, which made the pause longer; v0.2.1 prepares them before the game starts, so what remains is the loading itself.
- **Frame-rate drops.** Frame rates still vary with the scene; WIP 0.2.0 is similar to 0.1.5 on the same route. Measured on the development PC (RTX 3090 Ti, Core i9-12900KF): light scenes often reach 60 FPS, busy scenes (2,000–3,000 draws) about 30–45 FPS, and the heaviest scenes measured, the prologue's "stranger" shots, about 22–29 FPS (about 5 FPS with 0.1.3 in the same scene). Slower PCs will see lower numbers. In the heaviest scenes the renderer's own work on the CPU is the limit; spreading that work over several CPU cores is the next planned step.
- **Memory.** On the development PC the program used at most about 8.8 GB of RAM and committed up to about 18 GB on the prologue route (0.1.5: 8.2 GB and 17 GB), and 9.2 GB / 20 GB in a ten-minute session elsewhere in the game. Much of the committed memory is reserved by the graphics driver and not in RAM, but Windows needs room for it: keep the page file switched on ("System managed"). 16 GB of RAM is recommended.
- **Higher frame-rate limits.** Frame-rate limits above 60 FPS are available but have not been thoroughly tested across all scenes. The default 60 FPS cap does not guarantee a consistent 60 FPS.

## Controllers

- **DualSense / DualShock 4 motion controls.** During "Don't Move" sequences, the motion indicator may continuously drift to the left, making it difficult to remain still.
- **DualSense / DualShock 4 vibration and light bar.** Vibration and light-bar functionality are not yet implemented. Xbox-style controllers support vibration.
- **Keyboard controls.** Keys cannot be remapped yet, and L3 / R3 are not currently available on the keyboard.

## Hardware

- **Tested development configuration:** Intel Core i9-12900KF, NVIDIA GeForce RTX 3090 Ti (drivers 617.42 and 621.01), Windows 11, 64 GB RAM. WIP 0.1.5, 0.2.0, 0.2.1 and 0.3.0 have so far been played or tested only on this PC; GPU coherence (new in 0.3.0) is untested on AMD and Intel graphics.
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
