#!/usr/bin/env bash
# Every test of Quark: programs against their recorded output, wrong programs against their message, panics
# against theirs, and for every .qk the canonical text and its text -> graph -> text round trip.
#   tests/run.sh [path/to/quark.exe]
cd "$(dirname "$0")/.."
Q=${1:-build/quark.exe}
tmp=$(mktemp -d)
pass=0
fail=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL $1"; }

for f in tests/run/*.qk; do
    if "$Q" run "$f" > "$tmp/out" 2>&1 && cmp -s "$tmp/out" "${f%.qk}.out"; then ok; else bad "$f"; diff "${f%.qk}.out" "$tmp/out" | head -5; fi
done
for f in tests/errors/*.qk; do
    want=$(head -1 "$f" | sed 's|^// error: ||')
    if "$Q" check "$f" > "$tmp/out" 2>&1; then bad "$f (no error)"; elif grep -qF -- "$want" "$tmp/out"; then ok; else bad "$f"; cat "$tmp/out"; fi
done
for f in tests/panics/*.qk; do
    want=$(head -1 "$f" | sed 's|^// panic: ||')
    if "$Q" run "$f" --fuel 1000000 > "$tmp/out" 2>&1; then bad "$f (no panic)"; elif grep -qF -- "$want" "$tmp/out"; then ok; else bad "$f"; cat "$tmp/out"; fi
done
for f in examples/*.qk bench/*.qk tests/run/*.qk; do
    "$Q" fmt "$f" > "$tmp/a.qk"
    if cmp -s "$f" "$tmp/a.qk"; then ok; else bad "$f is not in canonical form (quark fmt $f --write)"; fi
    if "$Q" graph "$f" > "$tmp/g.json" && "$Q" text "$tmp/g.json" > "$tmp/b.qk" && cmp -s "$tmp/a.qk" "$tmp/b.qk"; then ok; else bad "$f graph round trip"; diff "$tmp/a.qk" "$tmp/b.qk" | head -5; fi
done
for f in examples/*.qk; do
    if "$Q" run "$f" > /dev/null 2>&1; then ok; else bad "$f does not run"; fi
done
# a standalone executable runs without the compiler's command line
if "$Q" build tests/run/loops.qk -o "$tmp/loops.exe" 2> /dev/null && "$tmp/loops.exe" > "$tmp/out" 2>&1 && cmp -s "$tmp/out" tests/run/loops.out; then ok; else bad "quark build"; fi
rm -rf "$tmp"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
