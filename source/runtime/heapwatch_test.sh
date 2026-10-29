#!/bin/sh
# heapwatch_test.sh: the --watch-heap detector (e6_heapwatch.c) on Linux with a planted corruption (STATUS 40).
set -e
cd "$(dirname "$0")"
T=${TMPDIR:-/tmp}/heapwatch_test.$$; mkdir -p $T
cc -O1 -Wall -Wno-unused-function -pthread -o $T/t heapwatch_test.c
$T/t | tail -1
