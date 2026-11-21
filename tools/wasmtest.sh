#!/usr/bin/env bash
# The suite in WebAssembly: tests/run's programs built for wasm32-wasi (quark build --target wasm32-wasi) and run by
# a WASI runtime, against their recorded output; tests/panics's against their message. What WebAssembly has not
# (tasks on stacks of their own, threads, other programs, sockets, C's libraries) is in tests/wasm-skip.txt, with why.
#   bash tools/wasmtest.sh [path/to/quark.exe]     WASM_RUN: the runtime's command (default: wasmtime run --dir=.)
cd "$(dirname "$0")/.."
Q=${1:-build/quark.exe}
RUN=${WASM_RUN:-wasmtime run --dir=.}
# a program that waits on nothing it can have (stdin, a task) stops after 30 s
if command -v timeout > /dev/null 2>&1; then RUN="timeout 30 $RUN"; fi
tmp=$(mktemp -d)
pass=0
fail=0
skipped=0
skip() { grep -q "^$1 " tests/wasm-skip.txt; }
for f in tests/run/*.qk; do
    if skip "$f"; then skipped=$((skipped + 1)); continue; fi
    if "$Q" build --target wasm32-wasi "$f" -o "$tmp/t.wasm" > "$tmp/build" 2>&1 && $RUN "$tmp/t.wasm" < /dev/null > "$tmp/out" 2>&1 &&
        cmp -s "$tmp/out" "${f%.qk}.out"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL $f"
        cat "$tmp/build" | grep -v "^quark: wrote" | head -3
        if grep -q "^Error: " "$tmp/out"; then grep -v "^ *$" "$tmp/out" | head -16; else diff "${f%.qk}.out" "$tmp/out" | head -6; fi
    fi
done
for f in tests/panics/*.qk; do
    if skip "$f"; then skipped=$((skipped + 1)); continue; fi
    want=$(head -1 "$f" | sed 's|^// panic: ||')
    flags=$(sed -n '2s|^// flags: ||p' "$f")
    if ! "$Q" build --target wasm32-wasi "$f" --fuel 1000000 $flags -o "$tmp/t.wasm" > "$tmp/build" 2>&1; then
        fail=$((fail + 1))
        echo "FAIL $f (build)"
        head -3 "$tmp/build"
    elif $RUN "$tmp/t.wasm" < /dev/null > "$tmp/out" 2>&1; then
        fail=$((fail + 1))
        echo "FAIL $f (no panic)"
    elif grep -qF -- "$want" "$tmp/out"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL $f"
        head -5 "$tmp/out"
    fi
done
rm -rf "$tmp"
echo "wasm32: $pass passed, $fail failed, $skipped not for WebAssembly (tests/wasm-skip.txt)"
[ "$fail" = 0 ]
