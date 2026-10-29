# Source code

This folder holds the complete source of the programs: `UntilDawn.exe` (the Windows runtime, the PS4 compatibility
layer, the Vulkan renderer and the GCN → SPIR-V shader translator) and `compat\ud_compat.exe` (the hardware
compatibility tester), with the build scripts, the tests and the tools that generate the tables the build uses. It is
the source of the releases from WIP 0.2.1 on; for earlier releases see
[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md#earlier-releases-wip-012--020).

It is licensed like the rest of the repository ([LICENSE](../LICENSE)); third-party notices are in
[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

**No game code, data or assets are included.** Everything the program needs from Until Dawn is read at run time from
the player's own copy. What the source does contain about the game — addresses of the instructions the runtime patches
in memory, short checks of the bytes it expects there, the names and sizes of the game's files for the start-up check —
is listed in THIRD_PARTY_NOTICES.md.

Comments refer to sections of the project's development log ("STATUS 88"), to test builds ("t152") and occasionally
to development tools and test programs (capture converters, analysis scripts) that are not part of this source; the
log and those tools are not published, but the comments explain the code on their own.

## Layout

| Folder | Contents |
| --- | --- |
| `runtime/` | The Windows runtime: loader and in-memory patches (`e3_loader.c`), PS4 system layer (`e3_hle.c`, `e4_threads.c`, `e5_*.c`), platform layer (`e3_platform.h`), PM4 front end and Vulkan renderer (`e5_gpu.c`, `e6_*.c`), shader translator (`gcn2spv.c`), launcher (`ud_launch.c`), compatibility tester (`ud_compat.c`), build scripts, DLL definition files, table generators |
| `runtime/tests/` | Linux-side tests (`run_host_tests.sh`) and developer tools for the program's diagnostic output (translation traces) |
| `runtime/vk/` | Khronos Vulkan API headers (Apache-2.0 OR MIT, `vk/LICENSE.md`) |
| `runtime/skipprompt_as/` | ActionScript source of the movie Skip prompt (compiled into `udskip_doabc.h`) |
| `renderer/` | The second, native renderer core (`udr_*.c`; linked in, off by default) and its compute shader |
| `renderer/tools/` | `udr_shaderscan.c`: extracts the GPU programs from your own game files (for the tests) |
| `tools/` | Python tools for PS4 ELF files used by the generators (`ps4_inspect.py`, `ps4_xref.py`, `ps4_ehframe.py`, `fself_unpack.py`) and `make_test_corpus.sh` |

## Building

The programs are built on **Linux** with LLVM, cross-compiling for Windows x64 without a Windows SDK (the Windows and C
runtime functions are imported through the DLL definition files in `runtime/`).

Requirements (tested on Ubuntu 24.04): `clang` 18, `lld` (`lld-link`), `llvm-dlltool`, `llvm-rc`, Python 3.

```sh
sh source/runtime/build_windows_from_linux.sh   # build/UntilDawn.exe and build/e6_loader.exe (the same program with a console window)
sh source/runtime/build_compat.sh               # build/ud_compat.exe
```

The scripts can be run from any folder. Everything they produce goes to `build/` at the top of the repository (ignored
by git; set `UD_BUILD_DIR` for another folder), with intermediate files in `build/obj/`. To run a build, copy
`UntilDawn.exe` (or `e6_loader.exe` for a console with the log) into the folder of an installed release, next to its
`Game\` and `Config\` folders.

**Program icon (optional):** the icon is the game's own `sce_sys/icon0.png`, so it is not in the repository. To build
with it, make it from your own copy first (needs Pillow): `python3 source/runtime/make_icon.py <game folder>
build/untildawn.ico`; the build picks it up. Without it, `UntilDawn.exe` has no icon.

**Reproducible builds:** the build embeds its date and time, and the linker writes a timestamp. To get the same bytes
again, set `TZ=UTC SOURCE_DATE_EPOCH=<seconds>` (the build date) and `UD_LINK_TIMESTAMP=<value>` (the PE link
timestamp, e.g. `0x6ae2bf8c`); with the same compiler version the output is then identical.

## Tests

```sh
sh tools/make_test_corpus.sh <game folder> <corpus folder>     # the game's GPU programs, from your own copy
UD_ELF_DIR=<folder with eboot.elf, libc.elf> sh runtime/tests/run_host_tests.sh <corpus folder>
```

Needs `gcc` and SPIRV-Tools (`spirv-val`, `spirv-dis`). `UD_ELF_DIR` is optional: plain ELFs made from your own dump
with `tools/fself_unpack.py`; with it, the import-table test and the libc comparison run too.

The tests write their files to `/tmp/ud_host_tests` (`UD_TEST_DIR` for another folder). A second argument compares the
translator's output with an earlier build: run the tests of that version (for example from a `git worktree` of an
earlier commit, outside this repository) with its own `UD_TEST_DIR`, then pass that folder's SPIR-V output as the
reference: `sh runtime/tests/run_host_tests.sh <corpus> <earlier test dir>/corp_default` (it reports how many of the
translated programs changed).

## Working on the port

Everything that is the project's own work lives in this repository; everything that comes from the game or from a
test run stays outside it:

| Where | What |
| --- | --- |
| this repository | source, build scripts, tests, tools, documentation (tracked by git) |
| `build/` (ignored) | executables, object files, maps, the icon made from your copy |
| a run folder outside the repository | an installed release (`UntilDawn.exe`, `Game\`, `Config\`) where test builds are copied and run; it also collects `Logs\`, captures and save data |
| folders outside the repository | your game dump, plain ELFs (`UD_ELF_DIR`), the GPU-program corpus, analysis files |

`.gitignore` excludes game files, extracted data, captures, logs, save data, build output and scratch files, so they
are not committed by accident even if they end up in the working tree; still keep them outside it.

## Generated files

Generated from your own copy of the game (plain ELFs made with `tools/fself_unpack.py`, disassembled with GNU
`objdump` where noted) or from this project's own data; checked in so the build does not need the game.

| File | Generator | Contents |
| --- | --- | --- |
| `e3_extrq_v100.h`, `e3_extrq_v103.h` | `gen_extrq_sites.py` (function list from `tools/ps4_ehframe.py`; `--no-typedef` for v100) | Addresses and register operands of the AMD-only EXTRQ instructions |
| `e3_rdtsc_v103.h` | `gen_rdtsc_sites.py` (objdump) | Addresses and lengths of the RDTSC (time-stamp counter) instructions |
| `rsqrt_sites.h` | `gen_rsqrt_sites.py` (objdump) | Addresses of approximate square-root instructions (diagnostic `--rsqrt-trap`) |
| `ud_manifest_v103.h` | — | Names and sizes of the 7,634 game files the start-up check looks for |
| `e3_stub_list.h` | `gen_stub_list.py` | One logging stub per import slot (no game data) |
| `gcn_ops_ci.h` | `gen_gcn_ops.py` from `gcn_opcodes_ci.json` | GCN gen2 opcode names (AMD's public Sea Islands ISA document) |
| `udskip_doabc.h` | FFDec, from `skipprompt_as/` | The compiled Skip prompt classes |
| `../renderer/shaders/udr_shaders_spv.h` | `renderer/tests/gen_shaders.py` | SPIR-V of the renderer's own compute shader (`tess_expand.spvasm`) |

Read from the game at run time instead of being stored: the list of imported system functions and their libraries
(`e3_imports.h`, named from this program's own function names by NID), and engine class names for the developer
diagnostics (from the game's own type information).
