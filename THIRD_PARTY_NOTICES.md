# Third-party notices

[← Back to README](README.md)

## What the release contains

`UntilDawn.exe` and `compat\ud_compat.exe` are built from this project's own source code. They link only
against libraries that are part of Windows (`kernel32`, `user32`, `gdi32`, `winmm`, `msvcrt`). At run time they
load, from your own system:

- the Vulkan loader installed with your graphics driver (`vulkan-1.dll`), and
- if present, `hid.dll`, `setupapi.dll` and `xinput*.dll` (controllers).

No third-party DLLs are shipped.

## Khronos Vulkan headers

The programs are compiled with the Vulkan API headers (`vulkan_core.h` and related files):

```
Copyright 2015-2026 The Khronos Group Inc.
SPDX-License-Identifier: Apache-2.0 OR MIT
```

Used under the MIT license:

```
Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions
of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.
```

## The game

None of Until Dawn's files are included in this repository or its releases: no executables, libraries, data archives,
movies, audio, textures or shaders. The program reads them at run time from the copy of the game you provide.

To work with that copy, `UntilDawn.exe` contains a small amount of information about the game:

- the names and sizes of the game's files and checksums of its two executables, to check that the right version is
  installed;
- the names of the PS4 system functions the game calls (Sony's interface names), to connect those calls to this
  project's replacements;
- addresses inside the game's executable, and short sequences of its instruction bytes that are checked before
  the in-memory compatibility patches are applied;
- the names of the game engine's object classes, used only for a developer diagnostic;
- three very small GPU programs from the game (228 bytes in total), used to recognise those programs and replace
  them with faster Vulkan copy operations.

The program icon is the game's own icon (`sce_sys/icon0.png` of CUSA02636), © Sony Interactive Entertainment,
used only to identify the game the program runs.

"Until Dawn", "PlayStation", "DualSense" and "DualShock" are trademarks of Sony Interactive Entertainment. This
project is not affiliated with or endorsed by Sony Interactive Entertainment or Supermassive Games.
