#!/usr/bin/env bash
# libquark written in Quark (docs/BOOTSTRAP.md, phase 4): the embedding API (compiler/api.qk) and the runtime, both
# written in Quark, compiled by the compiler written in Quark into one object whose addresses the linker gives, and
# written as an archive by compiler/image.qk: no C compiler and no ar make it. A C host links it as it links the C one:
#   gcc -Iinclude host.c build/libq/libquark.a -lm   (Linux, macOS: -lm -lpthread -ldl)
#   bash tools/libquark.sh [OUT.a]     default build/libq/libquark.a (the object beside it: libquark.o)
# QUARK: the quark that builds the writer (compiler/libquark.qk), default build/quark.exe; QK_TARGET: x86_64-windows,
# x86_64-linux, arm64-linux, x86_64-macos or arm64-macos (default: this machine's).
set -e
cd "$(dirname "$0")/.."
quark=${QUARK:-build/quark.exe}
out=${1:-build/libq/libquark.a}
if [ -z "$QK_TARGET" ]; then
    case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) QK_TARGET=x86_64-windows ;;
    Darwin) case "$(uname -m)" in arm64) QK_TARGET=arm64-macos ;; *) QK_TARGET=x86_64-macos ;; esac ;;
    *) case "$(uname -m)" in aarch64 | arm64) QK_TARGET=arm64-linux ;; *) QK_TARGET=x86_64-linux ;; esac ;;
    esac
fi
mkdir -p "$(dirname "$out")" build/libq
ext=
[ "$(uname -s | cut -c1-5)" = MINGW ] || [ "$(uname -s | cut -c1-4)" = MSYS ] && ext=.exe
writer=build/libq/libquark-writer$ext
"$quark" build compiler/libquark.qk -o "$writer" >/dev/null
"$writer" --target "$QK_TARGET" -o "$out" --object "${out%.a}.o"
