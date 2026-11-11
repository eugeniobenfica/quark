#!/usr/bin/env bash
# Builds build/quark.exe with any C11 compiler (gcc or clang). Quark itself needs none to run programs.
cd "$(dirname "$0")"
CC=${CC:-gcc}
mkdir -p build
$CC -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format-truncation \
    -o build/quark.exe src/*.c -lm
