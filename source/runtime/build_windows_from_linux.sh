#!/bin/sh
# Build UntilDawn.exe and e6_loader.exe (the same program with a console window) for Windows x64 on Linux, without any
# Windows SDK (clang + lld-link + llvm-dlltool + llvm-rc).
#
#   sh source/runtime/build_windows_from_linux.sh          (from any folder)
#
# Output: build/ at the top of the repository (ignored by git), or $UD_BUILD_DIR. Intermediate files go to <out>/obj.
# Program icon: <out>/untildawn.ico or source/runtime/untildawn.ico if present (python3 make_icon.py <game folder>
# <out>/untildawn.ico makes it from your own copy of the game); without it UntilDawn.exe has no icon.
# Reproducible builds: set TZ=UTC SOURCE_DATE_EPOCH=<seconds> (the build date the program prints) and
# UD_LINK_TIMESTAMP=<value> (the PE link timestamp); with the same compiler version the output is then identical.
set -e
cd "$(dirname "$0")"
B=${UD_BUILD_DIR:-../../build}; mkdir -p "$B/obj"; B=$(cd "$B" && pwd); O=$B/obj
TS=${UD_LINK_TIMESTAMP:+/timestamp:$UD_LINK_TIMESTAMP}
clang --target=x86_64-pc-windows-msvc -O1 -ffreestanding -fno-builtin -mno-stack-arg-probe -fno-stack-protector \
      -Wall -Wno-unused-function -Ivk -c e3_loader.c -o $O/e6_loader.obj
clang --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin -mno-stack-arg-probe -fno-stack-protector \
      -Wno-unused-function -c gcn2spv.c -o $O/gcn2spv.obj
# our native renderer (RENDERER_PLAN 12): the renderer core, linked in; selected at run time with --renderer native
R=../renderer; RCF="--target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin -mno-stack-arg-probe -fno-stack-protector -Wall -Wno-unused-function -Wno-missing-field-initializers -Wno-misleading-indentation -Ivk -D_WIN32"
UDR_OBJS=""; for f in udr_prod udr_tess udr_tile udr_shaderc udr_vk udr_shader_res udr_shader_sig udr_bindplan; do clang $RCF -c $R/$f.c -o $O/$f.obj; UDR_OBJS="$UDR_OBJS $O/$f.obj"; done
for d in kernel32 msvcrt user32 gdi32 winmm; do llvm-dlltool -m i386:x86-64 -d $d.def -l $O/$d.lib; done
LIBS="$O/kernel32.lib $O/msvcrt.lib $O/user32.lib $O/gdi32.lib $O/winmm.lib"
lld-link $TS /subsystem:console /entry:entry /nodefaultlib /stack:0x400000 /map:$B/e6_loader.map $O/e6_loader.obj $O/gcn2spv.obj $UDR_OBJS $LIBS /out:$B/e6_loader.exe
# The player-facing program: the same code as a Windows-subsystem executable (no console window). Started without
# arguments it reads Config\settings.ini, checks the Game folder and starts the game (ud_launch.c).
ICO=; [ -f untildawn.ico ] && ICO=$(pwd)/untildawn.ico; [ -f $B/untildawn.ico ] && ICO=$B/untildawn.ico
if [ -n "$ICO" ]; then sed "s#\"untildawn.ico\"#\"$ICO\"#" untildawn.rc > $O/untildawn.rc; llvm-rc /fo $O/untildawn.res $O/untildawn.rc; RES=$O/untildawn.res
else RES=; echo "no untildawn.ico (python3 make_icon.py <game folder> $B/untildawn.ico): UntilDawn.exe without icon"; fi
lld-link $TS /subsystem:windows /entry:entry /nodefaultlib /stack:0x400000 /map:$B/UntilDawn.map $RES $O/e6_loader.obj $O/gcn2spv.obj $UDR_OBJS $LIBS /out:$B/UntilDawn.exe
# The shader preparation manifest (v0.2.1, data/pipelines.udm): next to UntilDawn.exe, where the program looks for it
if [ -f ../../data/pipelines.udm ]; then cp ../../data/pipelines.udm "$B/pipelines.udm"; else echo "no data/pipelines.udm: the build has no shader preparation manifest"; fi
echo "built $B/UntilDawn.exe and $B/e6_loader.exe"
