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
# the standard library (std/*.qk, written in Quark) goes into the compiler as text: use std.json
{
    echo "/* Made by build.sh from the files in std: the standard library text. */"
    echo "#include <stddef.h>"
    echo "const char* const qkStdNames[] = {"
    for f in std/*.qk; do echo "    \"$(basename "$f" .qk)\","; done
    echo "    NULL};"
    echo "const char* const qkStdTexts[] = {"
    for f in std/*.qk; do
        sed 's/\\/\\\\/g; s/"/\\"/g; s/^/    "/; s/$/\\n"/' "$f"
        echo "    ,"
    done
    echo "    NULL};"
} > build/obj/stdlib.c
objs=()
$CC $FLAGS -c build/obj/stdlib.c -o build/obj/stdlib.o
objs+=(build/obj/stdlib.o)
for f in src/*.c; do
    name=$(basename "$f" .c)
    [ "$name" = main ] && continue
    $CC $FLAGS -c "$f" -o "build/obj/$name.o"
    objs+=("build/obj/$name.o")
done
rm -f build/libquark.a
$AR rcs build/libquark.a "${objs[@]}"
# a deep stack for the command line (and the executables it builds): recursion as deep as a program needs
STACK=""
LIBS="-lm -lpthread"
case "$($CC -dumpmachine)" in *mingw*|*windows*|*cygwin*) STACK="-Wl,--stack,268435456"; LIBS="-lm -lwinmm" ;; esac
$CC $FLAGS -o build/quark.exe src/main.c build/libquark.a $LIBS $STACK
$CC $FLAGS -o build/host.exe examples/host/host.c build/libquark.a $LIBS
$CC $FLAGS -o build/sysv-hosts.exe tests/sysv/hosts.c build/libquark.a $LIBS
