#!/bin/sh
# package_release.sh - lays out and zips a release folder from a build (v0.2.1). Ours.
#
#   sh source/tools/package_release.sh VERSION [BUILD_DIR] [OUT_DIR]
#
# BUILD_DIR (default build/): UntilDawn.exe and ud_compat.exe from build_windows_from_linux.sh / build_compat.sh.
# OUT_DIR (default dist/): gets UntilDawn_<VERSION>/ and UntilDawn_<VERSION>.zip.
#
# Layout (docs/INSTALLATION.md section 2):
#   UntilDawn.exe, pipelines.udm (data/pipelines.udm: the shader preparation manifest, next to UntilDawn.exe where
#   the program looks for it), Config/settings.ini (the program's own defaults, from ud_launch.c), Game/ (empty, with
#   PUT_YOUR_GAME_FILES_HERE.txt), compat/ (run_compat.bat, README_COMPAT.txt, ud_compat.exe), and the current
#   documentation (Markdown text): README.txt (README.md), INSTALLATION.txt (docs/INSTALLATION.md), CHANGELOG.txt
#   (docs/releases/v<VERSION>.md), Controls.txt (docs/CONTROLS.md), KNOWN_ISSUES.txt (docs/KNOWN_ISSUES.md),
#   THIRD_PARTY_NOTICES.txt. The release notes must exist, and the installation guide and known issues must name the
#   version, or nothing is packaged (an old text would otherwise go out with a new program).
# The manifest is checked for its header (format 2) and size; its contents are checked against the game files with
# source/tools/ud_manifest/udm_check (no shader code in it) when UDM_CHECK_GAME_DIR is set. No game file is copied.
set -e
V=${1:?usage: package_release.sh VERSION [BUILD_DIR] [OUT_DIR]}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
B=$(cd "${2:-$ROOT/build}" && pwd)
OUT=${3:-$ROOT/dist}; mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
D="$OUT/UntilDawn_$V"
for f in "$B/UntilDawn.exe" "$B/ud_compat.exe" "$ROOT/data/pipelines.udm" "$ROOT/docs/releases/v$V.md"; do [ -f "$f" ] || { echo "missing: $f"; exit 1; }; done
for f in docs/INSTALLATION.md docs/KNOWN_ISSUES.md docs/releases/v$V.md README.md; do
    grep -q "v$V\|UntilDawn_$V" "$ROOT/$f" || { echo "$f does not mention v$V: update the documentation first"; exit 1; }; done
python3 - "$ROOT/data/pipelines.udm" <<'EOF'
import struct, sys
d = open(sys.argv[1], 'rb').read(40)
if len(d) < 40 or d[:4] != b'UDPM' or struct.unpack_from('<I', d, 4)[0] != 2: sys.exit('data/pipelines.udm: not a format-2 pipeline manifest')
nf, npg, nt, npl = struct.unpack_from('<4I', d, 16)
print('manifest: %d programs (locations), %d translations, %d pipelines' % (npg, nt, npl))
EOF
if [ -n "$UDM_CHECK_GAME_DIR" ]; then
    T=$(mktemp -d); cc -O2 -o "$T/udm_check" "$ROOT/source/tools/ud_manifest/udm_check.c"
    "$T/udm_check" "$ROOT/data/pipelines.udm" "$UDM_CHECK_GAME_DIR" || { echo "udm_check failed: not packaged"; exit 1; }
    rm -rf "$T"
fi
rm -rf "$D"; mkdir -p "$D/Config" "$D/Game" "$D/compat"
cp "$B/UntilDawn.exe" "$D/"
cp "$ROOT/data/pipelines.udm" "$D/pipelines.udm"
# Config/settings.ini: the defaults the program writes itself when the file is missing (k_ud_ini_default)
python3 - "$ROOT/source/runtime/ud_launch.c" "$D/Config/settings.ini" <<'EOF'
import re, sys
s = open(sys.argv[1], encoding='utf-8').read()
m = re.search(r'static const char \*k_ud_ini_default =\s*((?:"(?:[^"\\]|\\.)*"\s*)+);', s)
if not m: sys.exit('k_ud_ini_default not found in ud_launch.c')
parts = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
txt = ''.join(parts).encode().decode('unicode_escape')
open(sys.argv[2], 'w', newline='').write(txt)
EOF
printf 'Put the files of your own extracted Until Dawn (CUSA02636) with the 01.03 update here,\r\nor set game_dir in Config\\settings.ini. See README.txt.\r\n' > "$D/Game/PUT_YOUR_GAME_FILES_HERE.txt"
cp "$ROOT/compat/run_compat.bat" "$ROOT/compat/README_COMPAT.txt" "$B/ud_compat.exe" "$D/compat/"

# the current documentation (Markdown, as text files)
cp "$ROOT/README.md" "$D/README.txt"
cp "$ROOT/docs/INSTALLATION.md" "$D/INSTALLATION.txt"
cp "$ROOT/docs/releases/v$V.md" "$D/CHANGELOG.txt"
cp "$ROOT/docs/CONTROLS.md" "$D/Controls.txt"
cp "$ROOT/docs/KNOWN_ISSUES.md" "$D/KNOWN_ISSUES.txt"
cp "$ROOT/THIRD_PARTY_NOTICES.md" "$D/THIRD_PARTY_NOTICES.txt"
(cd "$OUT" && rm -f "UntilDawn_$V.zip" && zip -qr "UntilDawn_$V.zip" "UntilDawn_$V")
echo "packaged $OUT/UntilDawn_$V.zip:"; (cd "$OUT" && unzip -l "UntilDawn_$V.zip" | tail -n +4 | head -n -2 | awk '{print "  " $4 "  (" $1 " bytes)"}')
