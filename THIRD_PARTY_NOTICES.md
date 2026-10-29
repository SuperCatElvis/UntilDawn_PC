# Third-party notices

[← Back to README](README.md)

## What the release contains

`UntilDawn.exe` and `compat\ud_compat.exe` are built from this project's own source code ([source/](source/README.md);
from WIP 0.2.1 on — for earlier releases see [below](#earlier-releases-wip-012--020)).
They link only against libraries that are part of Windows (`kernel32`, `user32`, `gdi32`, `winmm`, `msvcrt`). At run
time they load, from your own system:

- the Vulkan loader installed with your graphics driver (`vulkan-1.dll`), and
- if present, `hid.dll`, `setupapi.dll` and `xinput*.dll` (controllers).

No third-party DLLs are shipped.

## Khronos Vulkan headers

The programs are compiled with the Vulkan API headers (`vulkan_core.h` and related files, in `source/runtime/vk/`):

```
Copyright 2014-2026 The Khronos Group Inc.
SPDX-License-Identifier: Apache-2.0 OR MIT
```

Used under the MIT license:

```
Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions
of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.
```

## Microsoft D3D11 reference tessellator

The CPU tessellator of the native renderer (`source/renderer/udr_tess.c`, part of `UntilDawn.exe`)
follows the structure and several constant names of Microsoft's D3D11 reference tessellator:

```
Copyright (c) Microsoft Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions
of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.
```

## The game

None of Until Dawn's files are included in this repository or its releases: no executables, libraries, data
archives, movies, audio, textures or shaders. The program reads them at run time from the copy of the game you
provide.

To work with that copy, the source and `UntilDawn.exe` contain a small amount of information about the game:

- the names and sizes of the game's files and checksums of its two executables, to check that the right version is
  installed;
- addresses inside the game's executable, and short sequences of its instruction bytes (a few bytes each) that are
  checked before each in-memory compatibility patch is applied;
- the sizes and checksums of three of the game's UI movies and the locations of two button pictures in the game
  data, so that the Skip prompt (this project's own code) can be added to those movies in memory, using the
  pictures and text from your copy of the game;
- hashes of three very small GPU programs of the game, to recognise them and replace them with faster Vulkan copy
  operations (the programs themselves are not included);
- a few names of the game's functions and classes in comments and log messages, describing what is patched or
  traced (for example `SkinnedMeshComponent::SetVisible`).

The names of the PS4 system functions in the source are those this project implements or handles; the program finds
the game's imports by comparing their hashed IDs (NIDs) with these names. Engine class names used by developer
diagnostics are read from the game's own type information at run time.

The program icon is the game's own icon (`sce_sys/icon0.png` of CUSA02636), © Sony Interactive Entertainment, used
only to identify the game the program runs. It is not in the repository; the build makes it from your own copy.

## Earlier releases (WIP 0.1.2 – 0.2.0)

The executables of WIP 0.1.2 to WIP 0.2.0 also contained:

- a table of the names of the 748 system functions the game imports, resolved with the NID table
  `src/core/aerolib/aerolib.inl` of [shadPS4](https://github.com/shadps4-emu/shadPS4) (commit `805e12c`;
  Copyright 2024 shadPS4 Emulator Project, GPL-2.0-or-later);
- three very small GPU programs from the game (228 bytes in total) and 2,565 engine class names (used only by a
  developer diagnostic).

From WIP 0.2.1 these are no longer part of the program (see above). Those releases also contain the Microsoft
tessellator code and the Khronos headers described above, under the same notices.

The complete source code of WIP 0.1.3, 0.1.4, 0.1.5 and 0.2.0 (the exact code each release was built from, with build
instructions, under the GPL-2.0) is available on request: please open an
[issue](https://github.com/SuperCatElvis/UntilDawn_PC/issues).

"Until Dawn", "PlayStation", "DualSense" and "DualShock" are trademarks of Sony Interactive Entertainment. This
project is not affiliated with or endorsed by Sony Interactive Entertainment or Supermassive Games.
