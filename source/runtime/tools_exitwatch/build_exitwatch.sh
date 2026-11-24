#!/bin/sh
# w28_int: build ud_exitwatch.exe (test-harness tool: describes a process hung after its exit report). sh build_exitwatch.sh
set -e
cd "$(dirname "$0")"
B=${UD_BUILD_DIR:-../../../build}; mkdir -p "$B/obj"; B=$(cd "$B" && pwd); O=$B/obj
clang --target=x86_64-pc-windows-msvc -O1 -ffreestanding -fno-builtin -mno-stack-arg-probe -fno-stack-protector -Wall -c ud_exitwatch.c -o $O/ud_exitwatch.obj
llvm-dlltool -m i386:x86-64 -d kernel32_ew.def -l $O/kernel32_ew.lib
lld-link /subsystem:console /entry:entry /nodefaultlib /stack:0x200000 $O/ud_exitwatch.obj $O/kernel32_ew.lib /out:$B/ud_exitwatch.exe
echo "built $B/ud_exitwatch.exe"
