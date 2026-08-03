#!/usr/bin/env bash
# Cross-compile and run the controller-passthrough checks on a Linux VM.
#
# This project targets Windows (XInput / ViGEm / HidHide / raw HID), so we use
# the MinGW-w64 cross compiler and run what can run under Wine:
#   1. Compile-check the full application translation unit (validates every
#      header, including the Windows-only code paths, actually compiles).
#   2. Build the pure parse/convert unit tests and execute them under Wine.
#
# Requires: g++-mingw-w64-x86-64 and wine (see AGENTS.md).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXX=x86_64-w64-mingw32-g++
INCLUDES=(-I"$ROOT" -I"$ROOT/src" -I"$ROOT/third_party/ViGEmClient/include")
OUT="$(mktemp -d)"

echo "==> Compile-checking application (src/main.cpp)"
"$CXX" -std=c++17 "${INCLUDES[@]}" -c "$ROOT/src/main.cpp" -o "$OUT/main.o"
echo "    OK"

echo "==> Building unit tests"
"$CXX" -std=c++17 -Wall "${INCLUDES[@]}" \
    "$ROOT/tests/parse_tests.cpp" -o "$OUT/parse_tests.exe" \
    -static-libgcc -static-libstdc++

echo "==> Running unit tests under Wine"
WINEDEBUG=-all wine "$OUT/parse_tests.exe"
