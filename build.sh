#!/usr/bin/env bash
# Builds Quark with Quark: build/quark.exe, the compiler (compiler/), and build/libquark.a, the compiler and runtime
# for C hosts (include/quark.h). A Quark compiler builds it (docs/BOOTSTRAP.md): QUARK=path/to/quark, else quark on
# the PATH (a release's, or one built before). It builds stage1 (compiler/ in the quick tier), and stage1 builds
# build/quark.exe (the optimizing tier): what is built is made by today's compiler/, whatever made stage1.
#   ./build.sh            stage1, then build/quark.exe
#   ./build.sh --check    then the fixed point: build/quark.exe builds compiler/ once more (build/stage3), and the two
#                         must be the same bytes
# With a C compiler (CC, default gcc) it also links the C hosts the tests run: build/host.exe, build/sysv-hosts.exe.
set -e
cd "$(dirname "$0")"
B=${QK_OUT:-build}
CC=${CC:-gcc}
check=false
for a in "$@"; do
    case $a in
    --check) check=true ;;
    *) echo "usage: ./build.sh [--check]" >&2; exit 2 ;;
    esac
done
case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*) main=compiler/main.qk LIBS="-lm -lwinmm -lws2_32" ;;
Darwin) main=compiler/mainmacos.qk LIBS="-lm -lpthread -ldl" ;;
*) main=compiler/mainlinux.qk LIBS="-lm -lpthread -ldl" ;;
esac
quark=${QUARK:-$(command -v quark || true)}
if [ -z "$quark" ]; then
    echo "build.sh: Quark is built by a Quark compiler, and none was found: install a release's quark (or give one with" >&2
    echo "QUARK=path/to/quark). One machine's quark makes another's: quark build $main --target T" >&2
    exit 1
fi
mkdir -p "$B"
# the standard library and the runtime, as texts the compiler keeps: compiler/stdlib.qk
bash compiler/embed.sh

# stage NAME QUARK OUT [quick]: compiler/ built by QUARK into OUT, with the optimizing tier (unless quick), timed.
# stage1 runs with QUARK_IMAGES=off: built by another compiler, it has that one's runtime, which may not have the
# images today's compiler keeps its checked programs in (docs/PLANO.md B13)
stage() {
    local start=$SECONDS tier=--release
    [ "$4" = quick ] && tier=
    mkdir -p "$(dirname "$3")"
    local images=
    [ "$2" = "$B/stage1/quark.exe" ] && images=off
    if ! QUARK_IMAGES=$images "$2" build "$main" $tier -o "$3.new" > /dev/null 2> "$B/stage.err"; then
        cat "$B/stage.err" >&2
        exit 1
    fi
    rm -f "$B/stage.err"
    mv -f "$3.new" "$3"
    echo "$1: $3 ($(wc -c < "$3") bytes, $((SECONDS - start)) s)"
}

stage stage1 "$quark" "$B/stage1/quark.exe" quick
stage quark "$B/stage1/quark.exe" "$B/quark.exe"
if $check; then
    stage stage3 "$B/quark.exe" "$B/stage3/quark.exe"
    if cmp -s "$B/quark.exe" "$B/stage3/quark.exe"; then
        echo "fixed point: $B/quark.exe and $B/stage3/quark.exe are the same bytes"
    else
        echo "fixed point: $B/quark.exe and $B/stage3/quark.exe differ" >&2
        exit 1
    fi
fi
# libquark, written in Quark and compiled by the compiler just made (no C compiler, no ar)
start=$SECONDS
QUARK=$B/quark.exe bash tools/libquark.sh "$B/libquark.a"
echo "libquark: $B/libquark.a ($((SECONDS - start)) s)"
# the C hosts the tests run: the embedding example and System V calls into a host
if command -v "$CC" > /dev/null 2>&1; then
    $CC -Iinclude -o "$B/host.exe" examples/host/host.c "$B/libquark.a" $LIBS
    $CC -Iinclude -o "$B/sysv-hosts.exe" tests/sysv/hosts.c "$B/libquark.a" $LIBS
fi
