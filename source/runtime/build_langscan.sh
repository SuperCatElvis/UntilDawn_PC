#!/bin/sh
# Build ud_langscan.exe (w33_lang language scanner / importer, foundation) for Windows x64 with clang + lld-link, no SDK.
#   sh source/runtime/build_langscan.sh   -> build/ud_langscan.exe   (host test build: tests/build_langscan_host.sh)
set -e
cd "$(dirname "$0")"
B=${UD_BUILD_DIR:-../../build}; mkdir -p "$B/obj"; B=$(cd "$B" && pwd); O=$B/obj
TS=${UD_LINK_TIMESTAMP:+/timestamp:$UD_LINK_TIMESTAMP}
CF="-Ivk --target=x86_64-pc-windows-msvc -O1 -ffreestanding -fno-builtin -mno-stack-arg-probe -fno-stack-protector -Wno-unused-function"
clang $CF -c ud_langscan.c -o $O/ud_langscan.obj
clang $CF -O2 -c gcn2spv.c -o $O/gcn2spv_ls.obj
clang $CF -O2 -Wno-missing-field-initializers -D_WIN32 -c ../renderer/udr_shader_sig.c -o $O/udr_shader_sig_ls.obj
for d in kernel32 msvcrt user32 gdi32 winmm; do llvm-dlltool -m i386:x86-64 -d $d.def -l $O/$d.lib; done
lld-link $TS /subsystem:console /entry:entry /nodefaultlib /stack:0x400000 $O/ud_langscan.obj $O/gcn2spv_ls.obj $O/udr_shader_sig_ls.obj $O/kernel32.lib $O/msvcrt.lib $O/user32.lib $O/gdi32.lib $O/winmm.lib /out:$B/ud_langscan.exe
echo "built $B/ud_langscan.exe"
