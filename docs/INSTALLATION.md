# Installation and configuration

[← Back to README](../README.md)

This guide covers WIP 0.1.2. You need two things: **the release download** from this project, and **your own
copy of the original PS4 game**. The download does not contain the game.

---

## 1. What you need

- Windows 10 or 11, 64-bit, and a PC that meets the [system requirements](../README.md#system-requirements).
- A current graphics driver from NVIDIA, AMD or Intel (it provides Vulkan).
- Your own decrypted dump of **Until Dawn for PS4, CUSA02636, with the 01.03 update applied**. Other regions
  (for example the European or Asian releases) and other versions are not supported; the program checks the
  game's executables and tells you if they don't match.
- About 50 GB of free space for the game files, plus up to about 1 GB for the shader cache.

The program does not decrypt anything. It needs the game files in their decrypted, extracted form.

---

## 2. Download and extract the release

1. Download `UntilDawn_WIP_0.1.2.zip` from the
   [Releases page](https://github.com/SuperCatElvis/UntilDawn_PC/releases).
2. Extract it to a folder **you can write to**, for example `D:\Games\UntilDawn_WIP`.
   Don't use `C:\Program Files`: the program keeps its settings, logs, shader cache and saves in its own folder.

After extracting you have:

```
UntilDawn_WIP_0.1.2\
  UntilDawn.exe                     the game
  Config\settings.ini               settings (see section 5)
  Game\                             empty: your game files go here
    PUT_YOUR_GAME_FILES_HERE.txt
  compat\                           hardware compatibility tester
    run_compat.bat
    ud_compat.exe
    README_COMPAT.txt
  README.txt
  Controls.txt
  KNOWN_ISSUES.txt
  THIRD_PARTY_NOTICES.txt
```

---

## 3. Add your game files

The game folder must contain the base game **with the 01.03 update applied on top** (the update's files replace
the base game's). These are the parts the program uses:

```
eboot.bin
all_programs.prx
sce_sys\param.sfo
localcachepink\lumps\
localcachepink\streams\
Movies\
audio\
```

Other files and folders from your copy can stay; they are ignored.

Choose **one** of these three ways:

### Option A — copy the game into `Game`

Copy the contents of your game folder into the release's `Game` folder, so that this file exists:

```
UntilDawn_WIP_0.1.2\Game\eboot.bin
```

### Option B — point `settings.ini` at your game

Leave the game where it is and open `Config\settings.ini` in a text editor. Set `game_dir` to the folder that
contains `eboot.bin`, for example:

```ini
game_dir = D:\PS4\CUSA02636
```

### Option C — link the folder instead of copying it

This saves disk space. Delete the release's `Game` folder (it only holds a note), open a Command Prompt in the
release folder and run:

```bat
mklink /J Game "D:\PS4\CUSA02636"
```

Replace `D:\PS4\CUSA02636` with your own game folder. This creates a link only; to remove it later, use
`rmdir Game`, which deletes the link and not your game.

### Optional: "The Road Not Taken" DLC

If you own the pre-order bonus chapter, copy its folder **`UDAWNDLC00000001`** from your own copy (it contains
only `sce_sys\`) into the game folder, next to `eboot.bin`:

```
<game folder>\eboot.bin
<game folder>\UDAWNDLC00000001\sce_sys\...
```

It adds an extra section in Chapter 2 and two extra Mystery Man clues. When it is found, the log
(`Logs\UntilDawn.log`) contains a line ending in `-> owned (found in the game folder)`.

---

## 4. Start the game

Double-click **`UntilDawn.exe`**.

- At every start the program checks all 7,634 game files (presence, sizes and the version of the executables).
  If something is wrong, a message says which file is missing or which version was found, and the game does
  not start.
- It also checks your CPU and graphics card. If a requirement is missing, a message explains what it is.
- **The first play-through compiles shaders** for your graphics card as new areas load. Expect stutters, and
  occasional **freezes of 10–20 seconds**, mostly in the first minutes. This is not a crash. Later runs use the
  shader cache and are much smoother.

The program creates these folders next to `UntilDawn.exe` when it runs:

| Folder | Contents |
| --- | --- |
| `savedata\` | Your saves (and locally recorded trophy unlocks). Back this folder up if you care about your progress. |
| `Cache\` | Compiled shaders for your graphics card. Rebuilt automatically if deleted. |
| `Logs\` | `UntilDawn.log` for the current session, the logs of the last five sessions, and diagnostic files in `Logs\captures\`. |
| `temp0\` | The game's temporary data. |

To quit, close the game window.

---

## 5. Settings (`Config\settings.ini`)

The file is created with these defaults if it is missing. Delete it to restore the defaults.

| Setting | Default | Values |
| --- | --- | --- |
| `game_dir` | *(empty)* | Folder with the game files. Empty = the `Game` folder next to `UntilDawn.exe`. |
| `frame_rate_limit` | `60` | `30`, `60` (like the PS4), `120`, `144`, `165`, any other number, or `0` for no limit. Limits above 60 work but have not been checked in every scene. |
| `display_mode` | `windowed` | `windowed`, or `borderless` (a window covering the whole primary monitor). |
| `window_width` / `window_height` | `1280` / `720` | Window size in windowed mode. The window can also be resized with the mouse. |
| `developer` | `0` | `1` turns on developer capture keys that write multi-gigabyte diagnostic files. Leave it at `0` unless asked. |

The game itself renders at 1920×1080, as on the PS4, and is scaled to the window.

---

## 6. Troubleshooting

| Problem | What to do |
| --- | --- |
| "The game folder was not found" or "eboot.bin is missing from the game folder" | The `Game` folder (or `game_dir`) does not point at the folder that contains `eboot.bin`. Check section 3. |
| A message says the version or region does not match | You need CUSA02636 with the 01.03 update applied. Base-game files without the update, or another region, are refused. |
| A message says a file is missing or has the wrong size | Your copy is incomplete, or the update was not applied over the base game. Re-copy the files. |
| A message about the CPU | Your CPU lacks an instruction set the game's code uses (see the [requirements](../README.md#system-requirements)). This cannot be worked around. |
| A message about the graphics card or Vulkan | Update your graphics driver. If it still fails, run the [compatibility tester](COMPATIBILITY_TESTING.md) and report the result. |
| The game freezes for several seconds | Usually shader compilation on the first play-through; wait. If it lasts more than a minute, report it with the log. |
| The game crashed | Start it again (saves are kept), then [report it](../README.md#reporting-bugs) with `Logs\UntilDawn.previous.log`. |
