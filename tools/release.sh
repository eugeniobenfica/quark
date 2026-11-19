#!/usr/bin/env bash
# A release's compilers, one for each machine, all made here (quark build --target): what build.sh starts from where
# no quark is installed (the CI downloads them from the repository's latest release).
#   bash tools/release.sh [OUT]     default build/release: quark-x86_64-windows.exe, quark-arm64-windows.exe,
#                                   quark-x86_64-linux, quark-arm64-linux (glibc), quark-x86_64-linux-musl,
#                                   quark-arm64-linux-musl, quark-x86_64-macos, quark-arm64-macos, and SHA256SUMS
# QUARK: the quark that makes them, default build/quark.exe (build it first: ./build.sh --check).
set -e
cd "$(dirname "$0")/.."
quark=${QUARK:-build/quark.exe}
out=${1:-build/release}
mkdir -p "$out"
make() { # make TARGET MAIN NAME [LIBC]
    local libc=
    [ -n "$4" ] && libc="--libc $4"
    "$quark" build "$2" --release --target "$1" $libc -o "$out/$3" > /dev/null
    echo "$out/$3"
}
make x86_64-windows compiler/main.qk quark-x86_64-windows.exe
make arm64-windows compiler/main.qk quark-arm64-windows.exe
make x86_64-linux compiler/mainlinux.qk quark-x86_64-linux glibc
make arm64-linux compiler/mainlinux.qk quark-arm64-linux glibc
make x86_64-linux compiler/mainlinux.qk quark-x86_64-linux-musl musl
make arm64-linux compiler/mainlinux.qk quark-arm64-linux-musl musl
make x86_64-macos compiler/mainmacos.qk quark-x86_64-macos
make arm64-macos compiler/mainmacos.qk quark-arm64-macos
(cd "$out" && sha256sum quark-* > SHA256SUMS)
echo "$out/SHA256SUMS"
