#!/bin/sh
# Build ud_compat.exe (hardware compatibility test, STATUS 88) for Windows x64 with clang + lld-link, no SDK.
#
#   sh source/runtime/build_compat.sh          (from any folder)
#
# Output: build/ at the top of the repository (ignored by git), or $UD_BUILD_DIR; see build_windows_from_linux.sh for
# reproducible builds (SOURCE_DATE_EPOCH, UD_LINK_TIMESTAMP).
set -e
cd "$(dirname "$0")"
B=${UD_BUILD_DIR:-../../build}; mkdir -p "$B/obj"; B=$(cd "$B" && pwd); O=$B/obj
TS=${UD_LINK_TIMESTAMP:+/timestamp:$UD_LINK_TIMESTAMP}
CF="-Ivk --target=x86_64-pc-windows-msvc -O1 -ffreestanding -fno-builtin -mno-stack-arg-probe -fno-stack-protector -Wno-unused-function"
clang $CF -c ud_compat.c -o $O/ud_compat.obj
clang $CF -O2 -c gcn2spv.c -o $O/gcn2spv_compat.obj
clang $CF -O2 -Wno-missing-field-initializers -D_WIN32 -c ../renderer/udr_shader_sig.c -o $O/udr_shader_sig_compat.obj
for d in kernel32 msvcrt user32 gdi32 winmm; do llvm-dlltool -m i386:x86-64 -d $d.def -l $O/$d.lib; done
lld-link $TS /subsystem:console /entry:entry /nodefaultlib /stack:0x400000 $O/ud_compat.obj $O/gcn2spv_compat.obj $O/udr_shader_sig_compat.obj $O/kernel32.lib $O/msvcrt.lib $O/user32.lib $O/gdi32.lib $O/winmm.lib /out:$B/ud_compat.exe
echo "built $B/ud_compat.exe"
