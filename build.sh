#!/usr/bin/env bash
# Builds build/libquark.a (the compiler and runtime, for hosts: include/quark.h), build/quark.exe (the
# command line) and build/host.exe (the embedding example) with any C11 compiler. Quark itself needs none
# to run programs.
set -e
cd "$(dirname "$0")"
CC=${CC:-gcc}
AR=${AR:-ar}
FLAGS="-std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format-truncation -Iinclude"
mkdir -p build/obj
objs=()
for f in src/*.c; do
    name=$(basename "$f" .c)
    [ "$name" = main ] && continue
    $CC $FLAGS -c "$f" -o "build/obj/$name.o"
    objs+=("build/obj/$name.o")
done
rm -f build/libquark.a
$AR rcs build/libquark.a "${objs[@]}"
$CC $FLAGS -o build/quark.exe src/main.c build/libquark.a -lm
$CC $FLAGS -o build/host.exe examples/host/host.c build/libquark.a -lm
