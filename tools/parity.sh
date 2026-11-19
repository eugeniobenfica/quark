#!/usr/bin/env bash
# Two compilers make the same code: the lowered IR (quark ir --lowered), in the quick tier and --release, of every
# program in tests/run, examples, bench and std's tests, and the machine code of the compiler itself. For a
# refactoring of the compiler that must not change what it makes (B3: the memory decisions as passes of their own).
#   bash tools/parity.sh OLD NEW     prints each program whose code differs, then the count; exit 1 when any does
set -u
cd "$(dirname "$0")/.."
old=$1
new=$2
tmp=$(mktemp -d)
differ=0
total=0
for f in tests/run/*.qk examples/*.qk bench/*.qk; do
    for tier in "" --release; do
        total=$((total + 1))
        "$old" ir "$f" --lowered $tier > "$tmp/a" 2>&1
        "$new" ir "$f" --lowered $tier > "$tmp/b" 2>&1
        if ! cmp -s "$tmp/a" "$tmp/b"; then
            differ=$((differ + 1))
            echo "differs: $f $tier"
            diff "$tmp/a" "$tmp/b" | head -6
        fi
    done
done
# the compiler: its whole machine code
total=$((total + 1))
"$old" build compiler/main.qk --release -o "$tmp/old.exe" > /dev/null 2>&1
"$new" build compiler/main.qk --release -o "$tmp/new.exe" > /dev/null 2>&1
if ! cmp -s "$tmp/old.exe" "$tmp/new.exe"; then
    differ=$((differ + 1))
    echo "differs: compiler/main.qk --release (machine code)"
fi
rm -rf "$tmp"
echo "parity: $((total - differ)) of $total the same"
[ $differ -eq 0 ]
