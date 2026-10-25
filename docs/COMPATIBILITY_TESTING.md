# Compatibility testing

[← Back to README](../README.md)

The game has been played in full on the development PC (Intel CPU, NVIDIA GPU), and players have sent
compatibility reports from several other NVIDIA and AMD systems (see the [tested hardware](../README.md#tested-hardware)
table). The included tester tells us quickly whether other hardware behaves the same way. **Reports from AMD and
Intel graphics are the most useful right now — even when everything passes.** For WIP 0.1.4, please also say
whether the game itself ran well, and whether you changed `renderer_thread` in `Config\settings.ini`.

---

## What it does

`compat\run_compat.bat` checks your PC against what the port needs and runs the port's own code on your
hardware. It does not install anything and does not change your game or Windows. A small empty window opens and
closes while it runs.

It checks:

- **Windows, CPU and memory:** the instruction sets the game's code needs, the port's replacement code for
  AMD-only instructions, the game clock, game memory at PS4 addresses, threads, locks and memory ordering.
- **Vulkan:** every graphics card and driver, its limits and features, the texture and render-target formats the
  game uses, and whether the card can run the renderer.
- **GPU correctness:** about 40 small GPU programs translated by the port's shader translator, run on your GPU
  and compared with what the PS4's GPU computes.
- **The game's shaders** (if your game folder is found): every one of the game's 6,778 GPU programs is
  translated and compiled by your driver. This takes a few minutes more.

`INFO` lines record legitimate differences between drivers; they are not failures.

---

## How to run it

1. Set up the release as described in [INSTALLATION.md](INSTALLATION.md). The tester finds the game
   automatically from `Config\settings.ini` (`game_dir`) or the `Game` folder. It also works without the game,
   but then skips the shader part.
2. Double-click **`compat\run_compat.bat`** and wait for "Press any key" (about 1–5 minutes).
3. The console shows the result:
   - `RESULT: ALL CHECKS PASSED on this PC`, or
   - `FAIL` lines naming what didn't work.
4. The tester creates **`compat\compat_report.zip`** with:
   - `ud_compat_report.txt` — the results,
   - `ud_compat_console.txt` — the console output,
   - `UntilDawn.log` — the game's last log, if you have started the game.

   If the ZIP could not be made, use the two `.txt` files from the `compat` folder instead.

---

## Sending the report

1. Open a **[compatibility report](https://github.com/SuperCatElvis/UntilDawn_PC/issues/new/choose)** on GitHub.
2. Attach `compat_report.zip`.
3. Say whether you also played the game, and if so how far and what you saw.

The report contains your Windows version, CPU and GPU models, driver versions, memory size and the folder paths
you use. It contains no game data.

**Privacy:** folder paths can include your Windows user name (for example `C:\Users\<name>\...`), and the
included `UntilDawn.log` has the same kind of paths. Please look through the files and remove anything you don't
want to post publicly before attaching them.

---

## Reading the result yourself

| Result | Meaning |
| --- | --- |
| All checks passed | Your PC behaves like the development PC in everything the tester covers. The game should start; bugs specific to your hardware are still possible. |
| `FAIL` about the CPU | A required instruction set is missing. The game cannot run on this CPU. |
| `FAIL` about memory | Not enough physical memory or page file for the game's memory layout. Close other programs, or increase the page file. |
| `FAIL` about Vulkan or the GPU | Update your graphics driver first. If it still fails, please send the report. |
| `FAIL` in the GPU program checks or the shader corpus | Your driver computes or compiles something differently. This is exactly what we need reports about. |
