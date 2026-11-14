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
for f in tests/run/*.qk; do # the same programs without AVX (SSE2 only)
    if "$Q" run "$f" --no-avx > "$tmp/out" 2>&1 && cmp -s "$tmp/out" "${f%.qk}.out"; then ok; else bad "$f --no-avx"; fi
done
for f in tests/run/*.qk; do # the same programs calling C the System V way (Linux, macOS; through adapters on Windows)
    if "$Q" run "$f" --abi sysv > "$tmp/out" 2>&1 && cmp -s "$tmp/out" "${f%.qk}.out"; then ok; else bad "$f --abi sysv"; fi
done
for f in tests/errors/*.qk; do
    want=$(head -1 "$f" | sed 's|^// error: ||')
    if "$Q" check "$f" > "$tmp/out" 2>&1; then bad "$f (no error)"; elif grep -qF -- "$want" "$tmp/out"; then ok; else bad "$f"; cat "$tmp/out"; fi
done
for f in tests/panics/*.qk; do
    want=$(head -1 "$f" | sed 's|^// panic: ||')
    if "$Q" run "$f" --fuel 1000000 > "$tmp/out" 2>&1; then bad "$f (no panic)"; elif grep -qF -- "$want" "$tmp/out"; then ok; else bad "$f"; cat "$tmp/out"; fi
done
for f in examples/*.qk bench/*.qk tests/run/*.qk std/*.qk; do
    "$Q" fmt "$f" > "$tmp/a.qk"
    if cmp -s "$f" "$tmp/a.qk"; then ok; else bad "$f is not in canonical form (quark fmt $f --write)"; fi
    if "$Q" graph "$f" > "$tmp/g.json" && "$Q" text "$tmp/g.json" > "$tmp/b.qk" && cmp -s "$tmp/a.qk" "$tmp/b.qk"; then ok; else bad "$f graph round trip"; diff "$tmp/a.qk" "$tmp/b.qk" | head -5; fi
done
for f in examples/*.qk; do
    if "$Q" run "$f" > /dev/null 2>&1; then ok; else bad "$f does not run"; fi
done
# a memory limit stops a program that keeps everything it makes
printf 'fn main() {
    var xs: [Text] = []
    while true {
        xs.push(repeat("x", 1000))
    }
}
' > "$tmp/oom.qk"
if "$Q" run "$tmp/oom.qk" --memory 64 2>&1 | grep -qF "used more memory than it is given"; then ok; else bad "--memory"; fi
# --checked: Int arithmetic past its limits stops the program
printf 'fn grow(n: Int) -> Int = n * 1_000_000

fn main() {
    var x = 3
    while true {
        x = grow(x)
    }
}
' > "$tmp/ovf.qk"
if "$Q" run "$tmp/ovf.qk" --checked 2>&1 | grep -qF "went past its limits"; then ok; else bad "--checked"; fi
# the REPL: declarations and lets kept, an expression's value printed
if printf 'fn sq(x: Int) -> Int = x * x
let n = 7
sq(n) + 1
:q
' | "$Q" repl 2>&1 | grep -qF "50"; then ok; else bad "quark repl"; fi
# the profiler: where the time goes, by function
if "$Q" run bench/mandelbrot.qk --profile 2>&1 | grep -qE "samples|no samples"; then ok; else bad "--profile"; fi
# an object file, compiled ahead of time and linked into a C host (needs a C compiler: $CC or gcc)
CCX=${CC:-gcc}
if command -v "$CCX" > /dev/null 2>&1; then
    if "$Q" object tests/object/demo.qk -o "$tmp/demo.o" --symbol demo && "$CCX" -Iinclude tests/object/host.c "$tmp/demo.o" "$(dirname "$Q")/libquark.a" -lm -lpthread -o "$tmp/objhost" 2> /dev/null || "$CCX" -Iinclude tests/object/host.c "$tmp/demo.o" "$(dirname "$Q")/libquark.a" -lm -o "$tmp/objhost"; then
        if "$tmp/objhost" 2>&1 | tr -d '\r' | cmp -s - tests/object/host.out; then ok; else bad "quark object"; fi
    else bad "quark object (link)"; fi
fi
# a graph whose let is wired into its own value: an error, not a crash
if "$Q" text tests/graphs/selfWired.json 2>&1 | grep -qF "wired into itself"; then ok; else bad "graph wired into itself"; fi
# the debugger: breakpoints, locals, calls, stepping; and the trace an editor shows on its wires
if printf 'l\nbt\nc\np total\ns\nq\n' | "$Q" debug tests/debug/demo.qk --break 6 --break 16 2>&1 | tr -d '\r' | cmp -s - tests/debug/session.out; then ok; else bad "quark debug"; fi
if "$Q" run tests/debug/demo.qk --trace "$tmp/trace.json" > /dev/null && tr -d '\r' < "$tmp/trace.json" | cmp -s - tests/debug/trace.json; then ok; else bad "--trace"; fi
# objects for other machines (cross-compiled): the right format, and, when LLVM's linkers are here, linked with
# the table loads landing on the table (QkStaticProgram + 0x70: its first entry, after the 14-field header)
for t in arm64-macos x86_64-macos arm64-linux x86_64-linux x86_64-windows; do
    if "$Q" object tests/object/demo.qk --target $t --symbol demo -o "$tmp/demo-$t.o"; then
        magic=$(od -An -tx1 -N4 "$tmp/demo-$t.o" | tr -d ' \n')
        case $t in
            *macos) want=cffaedfe ;;
            *linux) want=7f454c46 ;;
            *) want=6486 ;;
        esac
        case $magic in "$want"*) ok ;; *) bad "quark object --target $t (format)" ;; esac
    else bad "quark object --target $t"; fi
done
if command -v ld64.lld > /dev/null 2>&1 && command -v llvm-objdump > /dev/null 2>&1; then
    if ld64.lld -arch arm64 -platform_version macos 11.0 11.0 -dylib -undefined dynamic_lookup -o "$tmp/demo.dylib" "$tmp/demo-arm64-macos.o" &&
        llvm-objdump -d "$tmp/demo.dylib" | grep -A1 adrp | grep -q "ldr.*#0x70\]"; then ok; else bad "Mach-O ARM64 linked"; fi
    if ld64.lld -arch x86_64 -platform_version macos 11.0 11.0 -dylib -undefined dynamic_lookup -o "$tmp/demo64.dylib" "$tmp/demo-x86_64-macos.o" &&
        llvm-objdump -d "$tmp/demo64.dylib" | grep -q "<_demo+0x70>"; then ok; else bad "Mach-O x86-64 linked"; fi
fi
# capabilities: a program may call only what it is given
if "$Q" check examples/words.qk --allow print 2>&1 | grep -qF "args is not given to this program"; then ok; else bad "--allow refuses"; fi
if "$Q" check examples/words.qk --allow "print, args, files, exit" > /dev/null 2>&1; then ok; else bad "--allow allows"; fi
# a standalone executable runs without the compiler's command line
if "$Q" build tests/run/loops.qk -o "$tmp/loops.exe" 2> /dev/null && "$tmp/loops.exe" > "$tmp/out" 2>&1 && cmp -s "$tmp/out" tests/run/loops.out; then ok; else bad "quark build"; fi
# quark test: passing and failing tests, each on its own
if "$Q" test tests/test/math.qk 2>&1 | sed 's/ ([0-9.]* ms)$//' > "$tmp/out"; cmp -s "$tmp/out" tests/test/math.out; then ok; else bad "quark test"; diff tests/test/math.out "$tmp/out" | head -5; fi
# the language server: diagnostics, hover, definition, symbols, formatting
if bash tests/lsp/session.sh "$Q" > "$tmp/out" 2>&1 && cmp -s "$tmp/out" tests/lsp/session.out; then ok; else bad "quark lsp"; diff tests/lsp/session.out "$tmp/out" | head -5; fi
# packages: quark get from git repositories the script makes (no network), use pkg.module, quark.lock, the errors
if command -v git > /dev/null 2>&1; then
    if bash tests/packages/session.sh "$Q" > "$tmp/out" 2>&1 && cmp -s "$tmp/out" tests/packages/session.out; then ok; else bad "quark get"; diff tests/packages/session.out "$tmp/out" | head -5; fi
fi
# System V calls into a host: records back in registers or through memory, arguments on the stack
if "$(dirname "$Q")/sysv-hosts.exe" 2>&1 | tr -d '\r' > "$tmp/out" && cmp -s "$tmp/out" tests/sysv/hosts.out; then ok; else bad "System V host calls"; fi
# embedding: the host example (host functions, events, hot reload, panics, budget, graphs, catalog)
H=$(dirname "$Q")/host.exe
if "$H" 2>&1 | tr -d '\r' > "$tmp/out" && cmp -s "$tmp/out" examples/host/host.out; then ok; else bad "host example"; diff examples/host/host.out "$tmp/out" | head -10; fi
P="--prelude examples/host/app.qk"
"$Q" fmt examples/host/item.qk > "$tmp/a.qk"
if cmp -s examples/host/item.qk "$tmp/a.qk"; then ok; else bad "item.qk is not in canonical form"; fi
if "$Q" graph examples/host/item.qk $P > "$tmp/g.json" && "$Q" text "$tmp/g.json" > "$tmp/b.qk" && cmp -s "$tmp/a.qk" "$tmp/b.qk"; then ok; else bad "item.qk graph round trip"; fi
if "$Q" catalog - $P | grep -q '"name": "moveBy", "kind": "extern fn", "host": true'; then ok; else bad "catalog"; fi
"$Q" fmt examples/host/app.qk > "$tmp/a.qk"
if cmp -s examples/host/app.qk "$tmp/a.qk"; then ok; else bad "app.qk is not in canonical form"; fi
# versions side by side: diffs by node key (text, JSON), three-way merges, conflicts by key (tests/collab)
C=tests/collab
for v in ours theirs moved; do
    "$Q" diff $C/base.qk $C/$v.qk > "$tmp/out"
    if [ $? -eq 1 ] && cmp -s "$tmp/out" $C/$v.diff; then ok; else bad "quark diff $v"; diff $C/$v.diff "$tmp/out" | head -5; fi
done
if "$Q" diff $C/base.qk $C/theirs.qk --json > "$tmp/out"; [ $? -eq 1 ] && cmp -s "$tmp/out" $C/theirs.json; then ok; else bad "quark diff --json"; fi
if "$Q" diff $C/base.qk $C/base.qk > "$tmp/out" && [ ! -s "$tmp/out" ]; then ok; else bad "quark diff (no changes)"; fi
if "$Q" merge $C/base.qk $C/ours.qk $C/theirs.qk -o "$tmp/m.qk" && cmp -s "$tmp/m.qk" $C/merged.qk; then ok; else bad "quark merge"; fi
if "$Q" merge $C/base.qk $C/moved.qk $C/theirs.qk -o "$tmp/m.qk" && cmp -s "$tmp/m.qk" $C/merged-moved.qk; then ok; else bad "quark merge (moved)"; fi
for v in base ours theirs; do "$Q" graph $C/$v.qk > "$tmp/$v.json"; done # graphs in, a graph out
if "$Q" merge "$tmp/base.json" "$tmp/ours.json" "$tmp/theirs.json" -o "$tmp/m.json" && "$Q" text "$tmp/m.json" | cmp -s - $C/merged.qk; then ok; else bad "quark merge (graphs)"; fi
for v in conflict removed; do
    "$Q" merge $C/base.qk $C/ours.qk $C/$v.qk -o "$tmp/m.qk" 2> "$tmp/err"
    if [ $? -eq 1 ] && tr -d '\r' < "$tmp/err" | cmp -s - $C/$v.out && cmp -s "$tmp/m.qk" $C/$v-merged.qk; then ok; else bad "quark merge ($v)"; cat "$tmp/err"; fi
done
"$Q" merge $C/base.qk $C/ours.qk $C/conflict.qk --json -o "$tmp/m.qk" 2> "$tmp/err"
if tr -d '\r' < "$tmp/err" | cmp -s - $C/conflict.json; then ok; else bad "quark merge --json"; fi
if command -v "$CCX" > /dev/null 2>&1; then # the embedding API's qkDiff and qkMerge
    if "$CCX" -Iinclude $C/api.c "$(dirname "$Q")/libquark.a" -lm -o "$tmp/collab" && "$tmp/collab" $C | tr -d '\r' | cmp -s - $C/api.out; then ok; else bad "qkDiff, qkMerge"; fi
fi
# the catalog's pins as data: a built-in's generic loop, a program's generic record
if "$Q" catalog - | grep -qF '"pins": [{"name": "source", "shape": {"kind": "loop", "element": {"kind": "param", "name": "T"}}}, {"name": "f", "shape": {"kind": "fn", "params": [{"kind": "param", "name": "T"}, {"kind": "int"}], "returns": {"kind": "param", "name": "U"}}}], "result": {"kind": "loop", "element": {"kind": "param", "name": "U"}}'; then ok; else bad "catalog pins"; fi
printf 'type Pair<A, B> { first: A, second: B }

fn swap<A, B>(p: Pair<A, B>) -> Pair<B, A> = Pair(p.second, p.first)
' > "$tmp/pair.qk"
if "$Q" catalog "$tmp/pair.qk" | grep -qF '"returns": "Pair<B, A>", "result": {"kind": "named", "name": "Pair", "args": [{"kind": "param", "name": "B"}, {"kind": "param", "name": "A"}]}'; then ok; else bad "catalog pins (a program's)"; fi
rm -rf "$tmp"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
