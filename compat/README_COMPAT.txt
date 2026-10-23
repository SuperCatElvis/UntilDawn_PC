Until Dawn native PC port - compatibility test (for testers)
=============================================================

What it does
------------
run_compat.bat checks whether your PC can run the port and collects the facts we need to find hardware-specific
problems. It does not install anything and does not change your game or Windows. A small empty window opens and
closes while it runs. It takes one to two minutes; with the game folder found it also compiles every shader of
the game on your graphics driver, which takes a few minutes more (the game data is read once).

How to run it
-------------
1. Double-click run_compat.bat (it lives in the compat\ folder next to UntilDawn.exe; if the game folder is set
   in Config\settings.ini or is the Game\ folder, it is found automatically).
2. Wait for "Press any key". The console shows PASS/FAIL lines and the result.
3. Send us compat_report.zip from this folder (it holds ud_compat_report.txt, the console output and the game's
   last log). If the zip was not made, send ud_compat_report.txt and ud_compat_console.txt.

What the port needs
-------------------
  CPU      Intel Core from 2013 (Haswell) or AMD Ryzen / Excavator or newer (AVX, F16C, BMI1, LZCNT, POPCNT, MOVBE)
  Memory   16 GB recommended, 12 GB minimum
  GPU      a Vulkan 1.3 driver with VK_EXT_external_memory_host and VK_KHR_push_descriptor: NVIDIA (Maxwell or
           newer), AMD (GCN 4 / RDNA or newer, 2022+ driver), Intel (Arc; Gen9+ integrated with a 2022+ driver)
  Windows  10 or 11, 64-bit
If a requirement is missing the report says so in plain words (a FAIL line), and the game itself shows the same
message when started.

What is in the report
---------------------
Every line is either a fact ("key = value") or a check (PASS / FAIL / INFO), so reports from different PCs can be
compared line by line:
  - Windows version, CPU model and features, memory, logical processors
  - CPU checks: the game's instruction sets; the port's EXTRQ replacement code and emulation against a reference
    (and against the real instruction on AMD CPUs); the 1594 MHz game clock; the thread-local storage slot the
    patched game uses; game memory at PS4 addresses; write-watch; a 2 GB commit; FP control words; threads, locks
    and memory ordering; cache line size
  - every Vulkan GPU with driver version, limits, subgroup and float-control properties, format support, and a
    usable / not usable verdict with the reason
  - about 40 small GPU programs translated by the port's shader translator and run on your GPU, each compared
    with what the PS4's GPU computes (shifts, conversions, NaN handling, fused multiply-add, transcendentals, bit
    operations, cross-lane swizzles, atomics, denormals). INFO lines record legitimate driver differences
    (denormal handling, multiply-add contraction) - they are information, not failures.
  - with the game folder: how many of the game's 6,778 GPU programs were found, translated, and accepted by your
    driver as modules and pipelines; any refusal names the program

Exit code 0 means every check passed. FAIL lines are the ones to look at.

Options (for us): --no-gpu, --fpctl, --game <folder>, --float-controls, --exact-mad.
