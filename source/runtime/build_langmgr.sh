#!/bin/sh
# Build UDLanguageImporter.exe (w33_lang Language Importer window over ud_langscan's functions) for Windows x64, no SDK.
#   sh source/runtime/build_langmgr.sh   -> build/UDLanguageImporter.exe
set -e
cd "$(dirname "$0")"
B=${UD_BUILD_DIR:-../../build}; mkdir -p "$B/obj"; B=$(cd "$B" && pwd); O=$B/obj
TS=${UD_LINK_TIMESTAMP:+/timestamp:$UD_LINK_TIMESTAMP}
CF="-Ivk --target=x86_64-pc-windows-msvc -O1 -ffreestanding -fno-builtin -mno-stack-arg-probe -fno-stack-protector -Wno-unused-function"
clang $CF -c ud_langmgr.c -o $O/ud_langmgr.obj
clang $CF -O2 -c gcn2spv.c -o $O/gcn2spv_lm.obj
clang $CF -O2 -Wno-missing-field-initializers -D_WIN32 -c ../renderer/udr_shader_sig.c -o $O/udr_shader_sig_lm.obj
for d in kernel32 msvcrt user32 gdi32 winmm langmgr_user32 langmgr_gdi32 shell32 ole32; do llvm-dlltool -m i386:x86-64 -d $d.def -l $O/$d.lib; done
lld-link $TS /subsystem:windows /entry:entry /nodefaultlib /stack:0x400000 $O/ud_langmgr.obj $O/gcn2spv_lm.obj $O/udr_shader_sig_lm.obj \
    $O/kernel32.lib $O/msvcrt.lib $O/user32.lib $O/gdi32.lib $O/winmm.lib $O/langmgr_user32.lib $O/langmgr_gdi32.lib $O/shell32.lib $O/ole32.lib /out:$B/UDLanguageImporter.exe
echo "built $B/UDLanguageImporter.exe"
