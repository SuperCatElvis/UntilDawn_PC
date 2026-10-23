# Known issues — WIP 0.1.2

[← Back to README](../README.md)

This is an early test release. What is known so far is listed below. If you find something that isn't here,
please [report it](../README.md#reporting-bugs).

---

## Stability

- **Occasional crashes.** In a full play-through on the development PC the game crashed twice and had to be
  restarted; saves were kept. The cause is being investigated: some of the game's memory is occasionally
  overwritten (for example character animation data and object tables). The program contains safeguards that
  detect and repair the most common cases, but not all of them.
- **Characters can briefly deform ("explode")** for a frame or two. This is the same memory problem.
- **Collectibles:** a totem has been seen to vanish, and a clue to reappear after it was picked up.

## Graphics

- The inside of mouths can render **black** in some close-ups.
- A **red flash** over the screen while moving through the Episodes menu.
- The text at the start of the **end credits** is smeared vertically.
- The **SKIP** text in the later "Previously on Until Dawn" clips can show as blocks.
- Small differences from the PS4 picture are possible elsewhere. Please report anything that looks wrong, with a
  screenshot.

## Performance

- **Shader-compile freezes on the first play-through.** Shaders are compiled for your graphics card the first
  time each area loads. Expect stutters and occasional freezes of **10–20 seconds**, mostly in the first minutes
  of a new game (on the development PC, a fresh install froze for about 12 and 17 seconds during the prologue).
  The game is not hung; wait for it. The shader cache (`Cache\`) removes most of these on later runs.
- About **30–60 FPS** depending on the scene, even on fast PCs. Performance work comes after stability and
  correctness.
- Frame-rate limits above 60 work, but have not been checked in every scene.

## Controllers

- **No vibration and no light bar** on DualSense / DualShock 4 yet. Xbox-style controllers vibrate.
- L3 / R3 are not available on the keyboard.

## Hardware

- Tested only on: Intel Core i9-12900KF, NVIDIA GeForce RTX 3090 Ti (drivers 617.42 and 621.01), Windows 11,
  64 GB memory.
- **AMD Radeon, Intel Arc and Intel integrated graphics are untested. AMD Ryzen CPUs are untested.** Please run
  the [compatibility tester](COMPATIBILITY_TESTING.md) on these systems and send the report, even if the game
  runs fine.

## Game version and features

- Only **CUSA02636 with the 01.03 update** is supported. Other regions and versions are refused at start.
- Online / PlayStation Network features are not available (the game reports the network as disconnected).
- Trophy unlocks are recorded locally (in `savedata\` and in the log); there is no on-screen trophy notification.
