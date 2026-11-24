#!/bin/sh
# Linux host build of ud_langscan (tests only): sh source/runtime/tests/build_langscan_host.sh -> build/ud_langscan_host
set -e
cd "$(dirname "$0")/.."
B=${UD_BUILD_DIR:-../../build}; mkdir -p "$B"
awk '/^\/\* -+ SHA-256/{on=1} /^\/\* -+ the supported game version/{on=0} on' ud_launch.c > tests/langscan_host_extract.h
awk '/^static int sfo_get\(/{on=1} on{print} on&&/^}/{on=0}' ud_launch.c >> tests/langscan_host_extract.h
cc -O2 -static -Wall -Wno-unused-function -Wno-format-truncation -DUD_LANGSCAN_HOST -I. -Itests ud_langscan.c -o "$B/ud_langscan_host"
echo "built $B/ud_langscan_host"
