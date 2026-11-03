#!/usr/bin/env bash
# Builds both NROs into SD_CARD/. Run it in devkitPro's MSYS2, or in the
# devkitpro/devkita64 container: anywhere DEVKITPRO is set.
set -e
: "${DEVKITPRO:?DEVKITPRO is not set}"
cd "$(dirname "$0")"
BUILD_DIR="${BUILD_DIR:-build}"
"${DEVKITPRO}/portlibs/switch/bin/aarch64-none-elf-cmake" -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=None "$@"
cmake --build "$BUILD_DIR"
