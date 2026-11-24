#!/bin/sh
# w26_update: build ud_update.exe (the updater; ud_update.c + ud_update_core.h + ud_update_main.h) for Windows x64 with
# clang + lld-link, no SDK, no C runtime (msvcrt only for malloc / free). Same conventions as build_compat.sh.
#   sh source/runtime/build_update.sh          -> build/ud_update.exe
# Accepts UntilDawn_PC_v<ver>.zip (preferred) and UntilDawn_<ver>.zip; UD_UPDATE_CFLAGS=-DUP_ACCEPT_LEGACY_NAME=0: the PC_v name only.
# Linux test build of the same logic: cc -O1 -o ud_update_posix source/runtime/ud_update_posix.c (tests/update/).
set -e
cd "$(dirname "$0")"
B=${UD_BUILD_DIR:-../../build}; mkdir -p "$B/obj"; B=$(cd "$B" && pwd); O=$B/obj
TS=${UD_LINK_TIMESTAMP:+/timestamp:$UD_LINK_TIMESTAMP}
V=$(sed -n 's/^#define UD_VERSION "\([^"]*\)".*/\1/p' e3_platform.h)
clang --target=x86_64-pc-windows-msvc -O1 -ffreestanding -fno-builtin -mno-stack-arg-probe -fno-stack-protector -Wall -Wno-unused-function \
      -DUD_VERSION="\"$V\"" $UD_UPDATE_CFLAGS -c ud_update.c -o $O/ud_update.obj
llvm-dlltool -m i386:x86-64 -d upd_kernel32.def -l $O/upd_kernel32.lib
llvm-dlltool -m i386:x86-64 -d upd_user32.def -l $O/upd_user32.lib
llvm-dlltool -m i386:x86-64 -d winhttp.def -l $O/winhttp.lib
llvm-dlltool -m i386:x86-64 -d msvcrt.def -l $O/msvcrt.lib
lld-link $TS /subsystem:windows /entry:entry /nodefaultlib /stack:0x200000 $O/ud_update.obj $O/upd_kernel32.lib $O/upd_user32.lib $O/winhttp.lib $O/msvcrt.lib /out:$B/ud_update.exe
echo "built $B/ud_update.exe (version $V)"
