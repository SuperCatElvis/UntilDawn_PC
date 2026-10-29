# Installation and configuration

[← Back to README](../README.md)

This guide covers WIP 0.2.0 (a work-in-progress test release). You need two things: **the release download** from this project, and **your own
copy of the original PS4 game**. The download does not contain the game.

---

## 1. What you need

- Windows 10 or 11, 64-bit, and a PC that meets the [system requirements](../README.md#system-requirements).
- A current graphics driver from NVIDIA, AMD or Intel (it provides Vulkan).
- A decrypted dump of **Until Dawn for PS4, CUSA02636, with the 01.03 update applied**, made from your own
  **legally obtained** copy of the game. Other regions
  (for example the European or Asian releases) and other versions are not supported; the program checks the
  game's executables and tells you if they don't match.
- About 50 GB of free space for the game files, plus up to about 1 GB for the shader cache.

The program does not decrypt anything. It needs the game files in their decrypted, extracted form.

---

## 2. Download and extract the release

1. Download `UntilDawn_WIP_0.2.0.zip` from the
   [Releases page](https://github.com/SuperCatElvis/UntilDawn_PC/releases).
2. Extract it to a folder **you can write to**, for example `D:\Games\UntilDawn_WIP`.
   Don't use `C:\Program Files`: the program keeps its settings, logs, shader cache and saves in its own folder.

After extracting you have:

```
UntilDawn_WIP_0.2.0\
  UntilDawn.exe                     the game
  Config\settings.ini               settings (see section 5)
  Game\                             empty: your game files go here
    PUT_YOUR_GAME_FILES_HERE.txt
  compat\                           hardware compatibility tester
    run_compat.bat
    ud_compat.exe
    README_COMPAT.txt
  README.txt
  CHANGELOG.txt
  Controls.txt
  KNOWN_ISSUES.txt
  THIRD_PARTY_NOTICES.txt
```

### Updating from an earlier WIP release

Copy the new `UntilDawn.exe`, `compat\ud_compat.exe` and the text files into your old folder. **Do not
overwrite** `Config\settings.ini`, `savedata\` or `Game\` (your settings, saves and game). Or extract 0.2.0 to a
new folder and copy your `settings.ini`, `savedata` and game files over.

- Saves and settings from earlier WIP releases work as they are. WIP 0.2.0 has four new settings (`gpu`,
  `present_mode`, `heap_slot_mb`, `heap_slot_reuse`, see section 5); an older `settings.ini` doesn't have them and
  their defaults are used. To see them in the file, delete it (it is recreated with the defaults; set `game_dir`
  again) or copy the lines from the new release's `Config\settings.ini`.
- `compat\ud_compat.exe` is new in 0.2.0 (built with 0.2.0's shader translator).
- 0.2.0 translates every shader anew, so the contents of the shader cache (`Cache\`) from earlier versions no
  longer match and the first-run shader-compilation pauses return once. You can delete the old `Cache\` folder.

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
UntilDawn_WIP_0.2.0\Game\eboot.bin
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
- **The first play-through compiles shaders** for your graphics card the first time new content appears. On the
  development PC (24 threads), with an empty cache, that meant one pause of about 12 seconds and a few of 3–8
  seconds in the first minutes; on CPUs with fewer cores pauses of 10–20 seconds or more are possible. This is not a crash. Later runs use the shader cache and are much
  smoother.

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
| `renderer_thread` | `1` | `1` = the renderer runs on its own thread, alongside the game (since 0.1.4; faster). `0` = the renderer runs on the game's thread, as in 0.1.3. Try `0` if the game freezes or stutters, and report whether it helped. It does not prevent the crashes caused by overwritten game memory. Not present in settings files from 0.1.3 and older; the default then applies. |
| `gpu` | `auto` | Which graphics card to use: `auto` (normally the dedicated one), a number from the GPU list in `Logs\UntilDawn.log` (`GPU 0: …`, `GPU 1: …`), or part of a card's name, e.g. `gpu = 5050` or `gpu = Radeon`. New in 0.2.0. |
| `present_mode` | `auto` | How frames are shown: `auto`, `fifo` (in step with the display), `mailbox` or `immediate`. `auto` uses `mailbox` and switches to `fifo` by itself if frames take very long to reach the screen (seen on some laptops with two GPUs). New in 0.2.0. |
| `heap_slot_mb` | `512` | Distance between the game's heap address slots in MB. `512` keeps all of the game's memory where the GPU can read it; `2048` is the game's own layout, under which the GPU loses sight of game data loaded after about 30 minutes. Only for testing. New in 0.2.0. |
| `heap_slot_reuse` | `1` | `1` = a heap address slot the game has released is reused at once (the memory committed for the heaps stays at the level of the heaps in use); `0` = the game's own order (it grows during long sessions). Only for testing. New in 0.2.0. |
| `developer` | `0` | `1` turns on developer capture keys that write multi-gigabyte diagnostic files. Leave it at `0` unless asked. |

The game itself renders at 1920×1080, as on the PS4, and is scaled to the window.

---

## 6. Troubleshooting

| Problem | What to do |
| --- | --- |
| A file is reported missing although it is there (often a very long `localcachepink/streams/...` name) | With WIP 0.1.2 the folder path was too long (260-character Windows limit); WIP 0.1.3 and later fix this, so update. On 0.1.2, move the release so the path to `Game\` is 62 characters or fewer. See [Known issues](KNOWN_ISSUES.md#false-missing-files-message-with-a-long-folder-path--fixed-in-wip-013). |
| **One** long-named file is reported missing and it really is not in `Game\localcachepink\streams` | The copy skipped it (common when copying from a long path). Copy the game again with `robocopy "<your dump>" "<release>\Game" /E`. See [Known issues](KNOWN_ISSUES.md#one-game-file-reported-missing-although-the-game-was-copied). |
| "The game folder was not found" or "eboot.bin is missing from the game folder" | The `Game` folder (or `game_dir`) does not point at the folder that contains `eboot.bin`. Check section 3. |
| A message says the version or region does not match | You need CUSA02636 with the 01.03 update applied. Base-game files without the update, or another region, are refused. |
| A message says a file is missing or has the wrong size | Your copy is incomplete, or the update was not applied over the base game. Re-copy the files. |
| A message about the CPU | Your CPU lacks an instruction set the game's code uses (see the [requirements](../README.md#system-requirements)). This cannot be worked around. |
| A message about the graphics card or Vulkan | Update your graphics driver. If it still fails, run the [compatibility tester](COMPATIBILITY_TESTING.md) and report the result. |
| The game freezes for several seconds | Usually shader compilation on the first play-through; wait. If it lasts more than a minute, report it with the log. |
| 0.2.0 crashes, freezes or stutters where 0.1.5 did not | [Report it](../README.md#reporting-bugs) with `Logs\UntilDawn.log`. You can also try `renderer_thread = 0` in `Config\settings.ini` (see section 5) and say whether that helps. |
| Laptop with two GPUs: frames take up to a second to appear, the game hangs for several seconds and ends with "device lost", or menus flicker | Windows Settings → System → Display → Graphics: add `UntilDawn.exe` and choose **High performance**. If the laptop has a MUX switch / "discrete GPU only" mode, switch it on. You can also try `present_mode = fifo`, and `gpu` to pick the card. See [Known issues](KNOWN_ISSUES.md#laptops-with-two-gpus). |
| The game runs out of memory or Windows warns about low memory | Keep the page file on ("System managed"); close other large programs. 16 GB of RAM is recommended. |
| The game crashed | Start it again (saves are kept), then [report it](../README.md#reporting-bugs) with `Logs\UntilDawn.previous.log`. |
