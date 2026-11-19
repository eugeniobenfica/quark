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
for f in tests/run/*.qk; do # the optimizing tier: the same output (its SIMD loops on AVX2, and on SSE without AVX)
    if "$Q" run "$f" --release > "$tmp/out" 2>&1 && cmp -s "$tmp/out" "${f%.qk}.out"; then ok; else bad "$f --release"; diff "${f%.qk}.out" "$tmp/out" | head -5; fi
    if "$Q" run "$f" --release --no-avx > "$tmp/out" 2>&1 && cmp -s "$tmp/out" "${f%.qk}.out"; then ok; else bad "$f --release --no-avx"; fi
done
# C's own functions, from the libraries the system has: run natively (the C library is called the platform's way,
# so not with --abi sysv on Windows), with the optimizing tier, and without AVX
for f in tests/c/*.qk; do
    for flags in "" "--release" "--no-avx" "--release --no-avx"; do
        if "$Q" run "$f" $flags > "$tmp/out" 2>&1 && cmp -s "$tmp/out" "${f%.qk}.out"; then ok; else bad "$f $flags"; diff "${f%.qk}.out" "$tmp/out" | head -5; fi
    done
done
# ... and built: an executable loads them when it starts
if "$Q" build tests/c/basics.qk -o "$tmp/cbasics.exe" 2> /dev/null && "$tmp/cbasics.exe" > "$tmp/out" 2>&1 && cmp -s "$tmp/out" tests/c/basics.out; then ok; else bad "quark build of a program that calls C"; fi
# a C function that is not there: said when the program is loaded, before it runs
printf 'extern fn quarkSurelyMissing(n: Int32) -> Int32\nextern fn sinh(x: Float64) -> Float64 in "quarkNoSuchLibrary"\n\nfn main() {\n    print("never")\n    print(quarkSurelyMissing(1))\n}\n' > "$tmp/missing.qk"
if "$Q" run "$tmp/missing.qk" 2>&1 | grep -qF "cannot find quarkSurelyMissing in the C library"; then ok; else bad "a missing C function"; fi
printf 'extern fn sinh(x: Float64) -> Float64 in "quarkNoSuchLibrary"\n\nfn main() {\n    print(sinh(1))\n}\n' > "$tmp/nolib.qk"
if "$Q" run "$tmp/nolib.qk" 2>&1 | grep -qF "cannot load the library quarkNoSuchLibrary"; then ok; else bad "a missing C library"; fi
# the runtime's table of functions is as long as the compiler's list of them (one more there, one more here)
if [ "$(sed -n 's/^const runtimeCount = \([0-9]*\).*/\1/p' compiler/runtime/start.qk)" = "$(awk '/^pub const runtimeNames = \[/,/^\]/' compiler/ir.qk | grep -o '"[A-Za-z0-9]*"' | wc -l | tr -d ' ')" ]; then ok; else bad "runtimeCount is not the number of runtimeNames"; fi
# a C function value C gives back (GetProcAddress), kept, passed on and called (Windows)
case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*)
    for f in tests/cwin/*.qk; do
        for flags in "" "--release" "--no-avx"; do
            if "$Q" run "$f" $flags > "$tmp/out" 2>&1 && cmp -s "$tmp/out" "${f%.qk}.out"; then ok; else bad "$f $flags"; fi
        done
    done ;;
esac
for f in tests/errors/*.qk; do
    want=$(head -1 "$f" | sed 's|^// error: ||')
    if "$Q" check "$f" > "$tmp/out" 2>&1; then bad "$f (no error)"; elif grep -qF -- "$want" "$tmp/out"; then ok; else bad "$f"; cat "$tmp/out"; fi
done
for f in tests/panics/*.qk; do
    want=$(head -1 "$f" | sed 's|^// panic: ||')
    flags=$(sed -n '2s|^// flags: ||p' "$f") # options it runs with (--memory 32)
    if "$Q" run "$f" --fuel 1000000 $flags > "$tmp/out" 2>&1; then bad "$f (no panic)"; elif grep -qF -- "$want" "$tmp/out"; then ok; else bad "$f"; cat "$tmp/out"; fi
    if "$Q" run "$f" --fuel 1000000 $flags --release > "$tmp/out" 2>&1; then bad "$f --release (no panic)"; elif grep -qF -- "$want" "$tmp/out"; then ok; else bad "$f --release"; cat "$tmp/out"; fi
done
for f in examples/*.qk bench/*.qk tests/run/*.qk tests/c/*.qk tests/object/*.qk std/*.qk; do
    "$Q" fmt "$f" > "$tmp/a.qk"
    if cmp -s "$f" "$tmp/a.qk"; then ok; else bad "$f is not in canonical form (quark fmt $f --write)"; fi
    if "$Q" graph "$f" > "$tmp/g.json" && "$Q" text "$tmp/g.json" > "$tmp/b.qk" && cmp -s "$tmp/a.qk" "$tmp/b.qk"; then ok; else bad "$f graph round trip"; diff "$tmp/a.qk" "$tmp/b.qk" | head -5; fi
done
for f in examples/*.qk; do
    if "$Q" run "$f" > /dev/null 2>&1; then ok; else bad "$f does not run"; fi
done
# an error is said once, however often the checker meets it
if [ "$("$Q" check tests/errors/mapKey.qk 2>&1 | grep -c "a map's key")" = 1 ]; then ok; else bad "an error said twice"; fi
# options go before or after the file
if "$Q" run --release tests/run/loops.qk > "$tmp/out" 2>&1 && cmp -s "$tmp/out" tests/run/loops.out; then ok; else bad "options before the file"; fi
# a line that starts with a binary operator goes on with the statement (the formatter joins it); .Case starts a value
printf 'enum E { A, B }

fn main() {
    let ok = 1 < 2
        and 2 < 3
        or false
    let n = 1
        + 2 * 3
        | 8
    let e: E = .A
    print(ok, n, e)
}
' > "$tmp/lines.qk"
if [ "$("$Q" run "$tmp/lines.qk" 2>&1 | tr -d '\r')" = "true 15 E.A" ] && "$Q" fmt "$tmp/lines.qk" | grep -qF "let ok = 1 < 2 and 2 < 3 or false"; then ok; else bad "lines that start with an operator"; fi
# parentheses nested past any program's need: an error, not a crash
{ printf 'fn main() {
    print('; for i in $(seq 1 200000); do printf '('; done; printf '1'; for i in $(seq 1 200000); do printf ')'; done; printf ')
}
'; } > "$tmp/deep.qk"
if "$Q" check "$tmp/deep.qk" 2>&1 | grep -qF "nested too deep"; then ok; else bad "nested too deep"; fi
# a memory limit stops a program that keeps everything it makes
printf 'fn main() {
    var xs: [Text] = []
    while true {
        xs.push("x" * 1000)
    }
}
' > "$tmp/oom.qk"
if "$Q" run "$tmp/oom.qk" --memory 64 2>&1 | grep -qF "used more memory than it is given"; then ok; else bad "--memory"; fi
# ... and so does the executable built with it
if "$Q" build "$tmp/oom.qk" --memory 64 -o "$tmp/oom.exe" > /dev/null 2>&1 && "$tmp/oom.exe" 2>&1 | grep -qF "(64 MB)"; then ok; else bad "build --memory"; fi
# a loop that only grows a var from before it makes no collection; one that starts the var again each turn does
if "$Q" run tests/run/loopGrowth.qk --memory 16 > "$tmp/out" 2>&1 && cmp -s "$tmp/out" tests/run/loopGrowth.out &&
    "$Q" run tests/run/loopGrowth.qk --memory 16 --release > "$tmp/out" 2>&1 && cmp -s "$tmp/out" tests/run/loopGrowth.out; then ok; else bad "growth-only loops"; fi
# a map that keys go into and out of a million times holds what it has, not what it had (its removed entries go)
if "$Q" run tests/run/mapChurn.qk --memory 16 > "$tmp/out" 2>&1 && cmp -s "$tmp/out" tests/run/mapChurn.out; then ok; else bad "a map's removed entries are compacted"; fi
# tasks allocate in arenas of their own: a live task (one parked, one stepped as the loop goes) does not keep a
# loop from giving back what its turns made
printf 'fn parked() {
    yield()
    print("never")
}

fn ticker(name: Text) {
    var log = ""
    var n = 0
    while true {
        n += 1
        log = "{name} {n}"
        yield()
    }
}

fn main() {
    let t = spawn(() => parked())
    let _ = step(t)
    let busy = spawn(() => ticker("tick"))
    var total = 0
    var out = ""
    for i in 0..3_000_000 {
        let s = "item {i} " + "x" * (i %% 50)
        total += len(s)
        out = "{s}!"
        if i %% 100 == 0 {
            let _ = step(busy)
        }
    }
    print(total, len(out))
}
' > "$tmp/tasks.qk"
if [ "$("$Q" run "$tmp/tasks.qk" --memory 64 2>&1 | tr -d '\r')" = "111388890 63" ]; then ok; else bad "--memory with live tasks"; fi
# building a text or a list a piece at a time is linear, whatever else each turn makes (a sanity bound: quadratic
# would take minutes)
printf 'fn main() {
    var out = ""
    var names: [Text] = []
    for i in 0..1_000_000 {
        out += "mov r{i %% 16}, {i}\\n"
        names.push("x{i}")
    }
    print(len(out), len(names))
}
' > "$tmp/linear.qk"
if [ "$(timeout 30 "$Q" run "$tmp/linear.qk" 2>&1 | tr -d '\r')" = "15263890 1000000" ]; then ok; else bad "building a text or a list is linear"; fi
# a parser's loops: an outer loop that keeps what it makes (a million turns) calling functions with loops of their own
# that make memory too, and an inner loop that collects at each call while it adds to the outer loop's array, are
# linear: four times the turns take about four times as long (quadratic: sixteen)
cat > "$tmp/nested.qk" <<'EOF'
type Tok { kind: Int, text: Text, line: Int }

type Node { kind: Int, name: Text, kids: [Int], line: Int }

type P { toks: [Tok], nodes: [Node], pos: Int, errs: [Text], junk: Int }

fn lexWord(i: Int) -> [Tok] {
    var out: [Tok] = []
    var k = 0
    while k < 4 {
        out.push(Tok(k, "w{i}_{k}", i))
        k += 1
    }
    out
}

fn P.args(var self, i: Int) -> [Int] {
    var kids: [Int] = []
    var k = 0
    while k < 3 {
        self.nodes.push(Node(1, "arg{k}", [k], i))
        kids.push(len(self.nodes) - 1)
        k += 1
    }
    kids
}

fn P.run(var self, n: Int) {
    var i = 0
    while i < n {
        let ts = lexWord(i)
        self.toks.push(ts[0])
        let kids = self.args(i)
        self.nodes.push(Node(ts[1].kind, ts[1].text, kids, i))
        i += 1
    }
}

fn P.churn(var self, i: Int) {
    var k = 0
    while k < 100 {
        let s = "g{k} {i}" * 200
        self.junk += len(s)
        if k == 0 {
            self.nodes.push(Node(0, "n{i}", [], i))
        }
        k += 1
    }
}

fn P.churnAll(var self, n: Int) {
    var i = 0
    while i < n {
        self.churn(i)
        i += 1
    }
}

fn main() {
    var times: [Float] = []
    for n in [100000, 400000] {
        var p = P([], [], 0, [], 0)
        let t0 = monotonicTime()
        p.run(n)
        times.push(monotonicTime() - t0)
        print(n, len(p.nodes), len(p.toks), p.nodes[len(p.nodes) - 1].name)
    }
    for n in [5000, 20000] {
        var p = P([], [], 0, [], 0)
        let t0 = monotonicTime()
        p.churnAll(n)
        times.push(monotonicTime() - t0)
    }
    print(if times[1] < times[0] * 6 { "linear" } else { "{times[1] / times[0]} times" })
    print(if times[3] < times[2] * 6 { "linear" } else { "{times[3] / times[2]} times" })
}
EOF
want=$(printf '100000 400000 100000 w99999_1
400000 1600000 400000 w399999_1
linear
linear')
got=$(timeout 300 "$Q" run "$tmp/nested.qk" 2>&1 | tr -d '')
if [ "$got" = "$want" ]; then ok; else bad "nested loops that make memory are linear"; echo "$got" | tail -2; fi
# ... and what loops give back is nothing the vars still point to (QK_CHECK_LOOPS: the runtime looks after each
# collection, and stops the program when one does)
for f in tests/run/memoryInPlace.qk tests/run/selfInPlace.qk; do
    for flags in "" "--release"; do
        if QK_CHECK_LOOPS=1 "$Q" run "$f" $flags > "$tmp/out" 2>&1 && cmp -s "$tmp/out" "${f%.qk}.out"; then ok; else bad "QK_CHECK_LOOPS $f $flags"; fi
    done
done
# inlining has a budget: parse levels that each call the next three times stay small (inlined 8 deep: 32 MB)
{ printf 'type P { toks: [Int], pos: Int, a: Int, b: Int, c: Int }\n\nfn P.peek(self) -> Int = if self.pos < len(self.toks) { self.toks[self.pos] } else { -1 }\n'
    for k in $(seq 0 11); do
        n=$((k + 1))
        if [ $k -lt 11 ]; then
            printf '\nfn P.level%d(var self) -> Int {\n    var a = self.level%d()\n    while self.peek() == %d {\n        self.pos += 1\n        a = a + self.level%d()\n    }\n    if self.peek() == 99 {\n        a = a - self.level%d()\n    }\n    a\n}\n' $k $n $k $n $n
        else
            printf '\nfn P.level%d(var self) -> Int {\n    let t = self.peek()\n    self.pos += 1\n    t\n}\n' $k
        fi
    done
    printf '\nfn main() {\n    var toks: [Int] = []\n    for i in 0..100000 {\n        toks.push(i %% 20)\n    }\n    var p = P(toks, 0, 0, 0, 0)\n    var s = 0\n    while p.pos < len(p.toks) {\n        s += p.level0()\n    }\n    print(s)\n}\n'
} > "$tmp/chain.qk"
bytes=$(timeout 60 "$Q" build "$tmp/chain.qk" --release -o "$tmp/chain.exe" 2>&1 | sed -n 's/.*(\([0-9]*\) bytes of machine code).*/\1/p')
if [ -n "$bytes" ] && [ "$bytes" -lt 200000 ] && [ "$("$tmp/chain.exe" | tr -d '\r')" = "800005" ]; then ok; else bad "inlining stays within its budget ($bytes bytes)"; fi
# a large var self comes by address (the caller's place, changed in place): one parameter, no slots back
if "$Q" ir tests/run/selfInPlace.qk | grep -qF "fn Cur.words (1 params, 1 results" &&
    "$Q" ir tests/run/selfInPlace.qk --release | grep -qF "fn Cur.next (1 params, 1 results"; then ok; else bad "a large var self by address"; fi
# quark graph writes Quark Graph Notation: a value of std.graph's types (a program takes it as a constant), the same
# graph as the JSON export (each said again in the other notation), and diff, merge and text read it
for f in tests/run/language.qk tests/run/features.qk std/json.qk; do
    "$Q" graph "$f" > "$tmp/g.qgn"
    { printf 'use std.graph\n\nconst g: Graph = '; cat "$tmp/g.qgn"; } > "$tmp/gconst.qk"
    "$Q" graph "$f" --json > "$tmp/g1.json"
    "$Q" graph "$tmp/g.qgn" --json > "$tmp/g2.json"
    "$Q" graph "$tmp/g1.json" > "$tmp/g2.qgn"
    if "$Q" check "$tmp/gconst.qk" > /dev/null 2>&1 && cmp -s "$tmp/g1.json" "$tmp/g2.json" && cmp -s "$tmp/g.qgn" "$tmp/g2.qgn" &&
        "$Q" text "$tmp/g.qgn" | cmp -s - <("$Q" fmt "$f"); then ok; else bad "$f in Quark Graph Notation"; fi
done
for v in base ours theirs; do "$Q" graph tests/collab/$v.qk > "$tmp/$v.qgn"; done
if "$Q" merge "$tmp/base.qgn" "$tmp/ours.qgn" "$tmp/theirs.qgn" -o "$tmp/m.qgn" && head -c 6 "$tmp/m.qgn" | grep -q '^Graph(' &&
    "$Q" text "$tmp/m.qgn" | cmp -s - tests/collab/merged.qk && "$Q" diff "$tmp/base.qgn" "$tmp/ours.qgn" > /dev/null; [ $? -eq 1 ]; then ok; else bad "quark merge and diff of .qgn graphs"; fi
# quark bind: a C header's declarations as Quark's (its macros expanded, its second header read, what Quark cannot say
# left as a note); for each target's long; and the C library called through declarations bound from its header
if "$Q" bind tests/bind/shapes.h --prefix shapes_ --lib shapes --target x86_64-windows | tr -d '\r' | cmp -s - tests/bind/shapes.qk &&
    "$Q" check tests/bind/shapes.qk > /dev/null 2>&1; then ok; else bad "quark bind"; fi
if [ "$("$Q" bind tests/bind/shapes.h --target x86_64-linux | grep -c 'weight: Int64\|room: UInt64) -> UInt64')" = 2 ]; then ok; else bad "quark bind --target (C's long)"; fi
mkdir -p "$tmp/bind"
cp tests/bind/uselibc.qk "$tmp/bind/"
"$Q" bind tests/bind/libc.h -o "$tmp/bind/libc.qk"
for flags in "" "--release"; do
    if "$Q" run "$tmp/bind/uselibc.qk" $flags 2>&1 | tr -d '\r' | cmp -s - tests/bind/uselibc.out; then ok; else bad "the C library through quark bind $flags"; fi
done
# --release keeps each function's optimized code by content (QUARK_CACHE=on: a program this small too): a second build
# takes all of it and runs the same; after an edit above them (lines, data and functions moved) the unchanged functions
# are still taken, and it runs right
printf 'fn greet(n: Int) -> Text = "hello {n}"\n\nfn total(xs: [Int]) -> Int = xs.map(x => x * 3 + 1).sum()\n\nfn main() {\n    print(greet(2), total([1, 2, 3]))\n}\n' > "$tmp/kept.qk"
QUARK_CACHE=on "$Q" run "$tmp/kept.qk" --release > "$tmp/kept1.txt" 2>&1
kept=$(QUARK_CACHE=on "$Q" run "$tmp/kept.qk" --release --time 2> "$tmp/keptTime.txt")
all=$(grep -o "([0-9]* of [0-9]* functions kept" "$tmp/keptTime.txt" | awk '{print ($1 == "(" $3) ? "all" : "some"}')
printf 'fn opening(n: Int) -> Text = "a new one {n}"\n\n' > "$tmp/kept2.qk"
cat "$tmp/kept.qk" >> "$tmp/kept2.qk"
sed -i 's/print(greet(2), total(\[1, 2, 3\]))/print(opening(1), greet(2), total([1, 2, 3]))/' "$tmp/kept2.qk"
mv "$tmp/kept2.qk" "$tmp/kept.qk"
edited=$(QUARK_CACHE=on "$Q" run "$tmp/kept.qk" --release --time 2> "$tmp/keptTime2.txt")
some=$(grep -o "(\([0-9]*\) of" "$tmp/keptTime2.txt" | tr -d '(' | awk '{print $1}')
off=$(QUARK_CACHE=off "$Q" run "$tmp/kept.qk" --release --time 2>&1 | grep -c "functions kept")
if [ "$kept" = "$(cat "$tmp/kept1.txt")" ] && [ "$all" = "all" ] && [ "$edited" = "a new one 1 hello 2 21" ] && [ "${some:-0}" -gt 0 ] && [ "$off" = 0 ]; then ok; else bad "the optimized code kept by content"; cat "$tmp/keptTime.txt" "$tmp/keptTime2.txt"; fi
# the REPL: declarations and lets kept, an expression's value printed
if printf 'fn sq(x: Int) -> Int = x * x
let n = 7
sq(n) + 1
:q
' | "$Q" repl 2>&1 | grep -qF "50"; then ok; else bad "quark repl"; fi
# the profiler: where the time goes, by function
if "$Q" run bench/mandelbrot.qk --profile 2>&1 | grep -qE "samples|no samples"; then ok; else bad "--profile"; fi
# an object file, compiled ahead of time and linked into a C host (needs a C compiler: $CC or gcc, and libquark: none
# on macOS yet)
CCX=${CC:-gcc}
if [ ! -f "$(dirname "$Q")/libquark.a" ]; then CCX=no-libquark; fi
case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*) CLIBS="-lm -lwinmm -lws2_32" ;; *) CLIBS="-lm -lpthread -ldl" ;; esac
if command -v "$CCX" > /dev/null 2>&1; then
    if "$Q" object tests/object/demo.qk -o "$tmp/demo.o" --symbol demo && "$CCX" -Iinclude tests/object/host.c "$tmp/demo.o" "$(dirname "$Q")/libquark.a" $CLIBS -o "$tmp/objhost"; then
        if "$tmp/objhost" 2>&1 | tr -d '\r' | cmp -s - tests/object/host.out; then ok; else bad "quark object"; fi
    else bad "quark object (link)"; fi
    # a C program that calls pub extern fns by their names, with C's types and structs, and gives one of its own
    if "$Q" object tests/object/cexport.qk -o "$tmp/cexport.o" --symbol cexport && "$CCX" -Iinclude tests/object/cexport.c "$tmp/cexport.o" "$(dirname "$Q")/libquark.a" $CLIBS -o "$tmp/cexport"; then
        if "$tmp/cexport" 2>&1 | tr -d '\r' | cmp -s - tests/object/cexport.out; then ok; else bad "pub extern fn, called from C"; "$tmp/cexport" 2>&1 | head -8; fi
    else bad "pub extern fn (link)"; fi
fi
# a graph whose let is wired into its own value: an error, not a crash
if "$Q" text tests/graphs/selfWired.json 2>&1 | grep -qF "wired into itself"; then ok; else bad "graph wired into itself"; fi
# the debugger: breakpoints, locals, calls, stepping; and the trace an editor shows on its wires
if printf 'l\nbt\nc\np total\ns\nq\n' | "$Q" debug tests/debug/demo.qk --break 6 --break 16 2>&1 | tr -d '\r' | cmp -s - tests/debug/session.out; then ok; else bad "quark debug"; fi
if "$Q" run tests/debug/demo.qk --trace "$tmp/trace.json" > /dev/null && tr -d '\r' < "$tmp/trace.json" | cmp -s - tests/debug/trace.json; then ok; else bad "--trace"; fi
# objects for other machines (cross-compiled): the right format, and, when LLVM's linkers are here, linked with
# the table loads landing on the table (QkStaticProgram + 0x78: its first entry, after the 15-field header)
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
        llvm-objdump -d "$tmp/demo.dylib" | grep -A1 adrp | grep -q "ldr.*#0x78\]"; then ok; else bad "Mach-O ARM64 linked"; fi
    if ld64.lld -arch x86_64 -platform_version macos 11.0 11.0 -dylib -undefined dynamic_lookup -o "$tmp/demo64.dylib" "$tmp/demo-x86_64-macos.o" &&
        llvm-objdump -d "$tmp/demo64.dylib" | grep -q "<_demo+0x78>"; then ok; else bad "Mach-O x86-64 linked"; fi
fi
# capabilities: a program may call only what it is given
if "$Q" check examples/words.qk --allow print 2>&1 | grep -qF "args is not given to this program"; then ok; else bad "--allow refuses"; fi
if "$Q" check examples/words.qk --allow "print, args, files, exit" > /dev/null 2>&1; then ok; else bad "--allow allows"; fi
# runProcess is the group process: a program given only print may not start one
if "$Q" check tests/run/process.qk --allow "print, files" 2>&1 | grep -qF "runProcess is not given to this program (the host allows: print, files); it is in the group process"; then ok; else bad "--allow and the group process"; fi
# a type error inside len(...) is said once
printf 'fn main() {\n    let xs = [1, 2]\n    print(len(xs + "a"), len(1 + "b"))\n}\n' > "$tmp/len.qk"
if [ "$("$Q" check "$tmp/len.qk" 2>&1 | grep -c "len.qk:[0-9]")" = 2 ]; then ok; else bad "an error inside len said twice"; fi
# --trace keeps what a push or a var self call changed
printf 'type C { n: Int }\n\nfn C.bump(var self) {\n    self.n += 1\n}\n\nfn main() {\n    var xs: [Int] = []\n    xs.push(4)\n    var c = C(0)\n    c.bump()\n}\n' > "$tmp/changes.qk"
if "$Q" run "$tmp/changes.qk" --trace "$tmp/changes.json" && grep -qF '"main/xs": {"value": "[4]"' "$tmp/changes.json" && grep -qF '"main/c": {"value": "C(n: 1)"' "$tmp/changes.json"; then ok; else bad "--trace of a push and a var self call"; fi
# a standalone executable runs without the compiler's command line
if "$Q" build tests/run/loops.qk -o "$tmp/loops.exe" 2> /dev/null && "$tmp/loops.exe" > "$tmp/out" 2>&1 && cmp -s "$tmp/out" tests/run/loops.out; then ok; else bad "quark build"; fi
if "$Q" build tests/run/simd.qk --release -o "$tmp/simd.exe" 2> /dev/null && "$tmp/simd.exe" > "$tmp/out" 2>&1 && cmp -s "$tmp/out" tests/run/simd.out; then ok; else bad "quark build --release"; fi
# Executable entry points return status or a typed error.
printf 'fn main() -> Int = 7\n' > "$tmp/status.qk"
"$Q" run "$tmp/status.qk" > "$tmp/out" 2>&1
if [ $? -eq 7 ] && [ ! -s "$tmp/out" ]; then ok; else bad "main returns Int"; fi
if "$Q" build "$tmp/status.qk" -o "$tmp/status.exe"; then
    "$tmp/status.exe"
    if [ $? -eq 7 ]; then ok; else bad "standalone main returns Int"; fi
else
    bad "build with Int main"
fi
printf 'fn close() -> Unit! = Err("failed")\nfn main() -> Unit! {\n    defer { print("cleanup") }\n    close()?\n    ()\n}\n' > "$tmp/result-main.qk"
"$Q" run "$tmp/result-main.qk" --release > "$tmp/out" 2> "$tmp/err"
status=$?
printf 'cleanup\n' > "$tmp/expected"
if [ "$status" -eq 1 ] && cmp -s "$tmp/out" "$tmp/expected" && grep -qx 'failed' "$tmp/err"; then ok; else bad "main propagates a result after cleanup"; fi
if "$Q" build "$tmp/status.qk" --target nonexistent -o "$tmp/wrong-target.exe" > "$tmp/out" 2>&1; then
    bad "build rejects unsupported targets"
elif grep -qF -- "--target is x86_64-windows" "$tmp/out"; then ok; else bad "invalid build target diagnostic"; fi
# quark build --target: an executable for another machine, its header that system's (ELF, PE32+, Mach-O)
for t in "x86_64-linux 7f454c46" "arm64-linux 7f454c46" "x86_64-windows 4d5a" "arm64-windows 4d5a" "x86_64-macos cffaedfe" "arm64-macos cffaedfe"; do
    set -- $t
    if "$Q" build "$tmp/status.qk" --target "$1" -o "$tmp/cross-$1" > /dev/null 2>&1 && od -An -tx1 -N4 "$tmp/cross-$1" | tr -d ' \n' | grep -q "^$2"; then ok; else bad "quark build --target $1"; fi
done
printf '{"quark":999,"decls":[]}\n' > "$tmp/unknown-graph.json"
if "$Q" text "$tmp/unknown-graph.json" > "$tmp/out" 2>&1; then
    bad "reserved graph version"
elif grep -qF "unsupported Quark graph version" "$tmp/out"; then ok; else bad "graph version diagnostic"; fi
printf 'fn main() {\n    print(1)\n}\n' > "$tmp/uid-before.qk"
printf 'fn main() {\n    print(0)\n    print(2)\n}\n' > "$tmp/uid-after.qk"
printf '{"main/@0":{"pos":[8,9],"uid":"printed"}}\n' > "$tmp/layout-before.json"
printf '{"main/@1":{"pos":[8,9],"uid":"printed"}}\n' > "$tmp/layout-after.json"
if "$Q" graph "$tmp/uid-before.qk" --layout "$tmp/layout-before.json" --json > "$tmp/uid-before.json" &&
   "$Q" graph "$tmp/uid-after.qk" --layout "$tmp/layout-after.json" --json > "$tmp/uid-after.json"; then
    "$Q" diff "$tmp/uid-before.json" "$tmp/uid-after.json" --json > "$tmp/out"
    if [ $? -eq 1 ] && grep -qF '"uid": "printed"' "$tmp/uid-after.json" &&
       grep -qF '"change": "changed", "key": "main/#uid/printed"' "$tmp/out" &&
       "$Q" merge "$tmp/uid-before.json" "$tmp/uid-before.json" "$tmp/uid-after.json" -o "$tmp/uid-merged.json" &&
       grep -qE '"uid"[[:space:]]*:[[:space:]]*"printed"' "$tmp/uid-merged.json"; then
        ok
    else
        bad "persistent graph node identity"
    fi
else
    bad "graph layout with persistent node identity"
fi
# quark test: passing and failing tests, each on its own
if "$Q" test tests/test/math.qk 2>&1 | sed 's/ ([0-9.]* ms)$//' > "$tmp/out"; cmp -s "$tmp/out" tests/test/math.out; then ok; else bad "quark test"; diff tests/test/math.out "$tmp/out" | head -5; fi
if "$Q" test tests/test/math.qk --release 2>&1 | sed 's/ ([0-9.]* ms)$//' > "$tmp/out"; cmp -s "$tmp/out" tests/test/math.out; then ok; else bad "quark test --release"; fi
# the release tier's SIMD loops for ARM64 (NEON): made, in an object of the right format
if "$Q" object tests/run/simd.qk --release --target arm64-linux -o "$tmp/simd-arm64.o" && [ "$(od -An -tx1 -N4 "$tmp/simd-arm64.o" | tr -d ' 
')" = 7f454c46 ]; then ok; else bad "quark object --release --target arm64-linux"; fi
# the language server: diagnostics, hover, definition, symbols, formatting
if bash tests/lsp/session.sh "$Q" > "$tmp/out" 2>&1 && cmp -s "$tmp/out" tests/lsp/session.out; then ok; else bad "quark lsp"; diff tests/lsp/session.out "$tmp/out" | head -5; fi
# packages: quark get from git repositories the script makes (no network), use pkg.module, quark.lock, the errors
if command -v git > /dev/null 2>&1; then
    if bash tests/packages/session.sh "$Q" > "$tmp/out" 2>&1 && cmp -s "$tmp/out" tests/packages/session.out; then ok; else bad "quark get"; diff tests/packages/session.out "$tmp/out" | head -5; fi
fi
# ... and while writing: fields, cases and named values completed; quick fixes for the errors that say what to write
if bash tests/lsp/assist.sh "$Q" > "$tmp/out" 2>&1 && cmp -s "$tmp/out" tests/lsp/assist.out; then ok; else bad "quark lsp (completion, quick fixes)"; diff tests/lsp/assist.out "$tmp/out" | head -5; fi
# the debug adapter: breakpoints, stepping over, in and out, the calls, a frame's names, the program's output
if bash tests/dap/session.sh "$Q" > "$tmp/out" 2>&1 && cmp -s "$tmp/out" tests/dap/session.out; then ok; else bad "quark dap"; diff tests/dap/session.out "$tmp/out" | head -5; fi
# documentation: the pub declarations of a file and the files it uses, with their comments (Markdown, HTML)
if "$Q" doc tests/doc/shapes.qk | tr -d '\r' | cmp -s - tests/doc/shapes.md; then ok; else bad "quark doc"; fi
if "$Q" doc tests/doc/shapes.qk -o "$tmp/shapes.html" && grep -qF '<pre><code>pub fn Point.distance(self, other: Point) -&gt; Float</code></pre>' "$tmp/shapes.html"; then ok; else bad "quark doc -o .html"; fi
# System V calls into a host: records back in registers or through memory, arguments on the stack
# (only with a C compiler: build.sh linked them with this libquark, not an older one)
if [ "$CCX" != no-libquark ] && command -v "$CCX" > /dev/null 2>&1; then
    if "$(dirname "$Q")/sysv-hosts.exe" 2>&1 | tr -d '\r' > "$tmp/out" && cmp -s "$tmp/out" tests/sysv/hosts.out; then ok; else bad "System V host calls"; diff tests/sysv/hosts.out "$tmp/out" | head -10; fi
    # embedding: the host example (host functions, events, hot reload, panics, budget, graphs, catalog)
    H=$(dirname "$Q")/host.exe
    if "$H" 2>&1 | tr -d '\r' > "$tmp/out" && cmp -s "$tmp/out" examples/host/host.out; then ok; else bad "host example"; diff examples/host/host.out "$tmp/out" | head -10; fi
fi
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
    if "$CCX" -Iinclude $C/api.c "$(dirname "$Q")/libquark.a" $CLIBS -o "$tmp/collab" && "$tmp/collab" $C | tr -d '\r' | cmp -s - $C/api.out; then ok; else bad "qkDiff, qkMerge"; fi
fi
# the catalog's pins as data: a built-in's generic loop, a program's generic record
if "$Q" catalog - | grep -qF '"pins": [{"name": "source", "shape": {"kind": "loop", "element": {"kind": "param", "name": "T"}}}, {"name": "f", "shape": {"kind": "fn", "params": [{"kind": "param", "name": "T"}], "returns": {"kind": "param", "name": "U"}}}], "result": {"kind": "loop", "element": {"kind": "param", "name": "U"}}'; then ok; else bad "catalog pins"; fi
# a node's hash is its content's: a statement inserted above it changes its key (its place), not its hash
printf 'fn main() {\n    print(1)\n}\n' > "$tmp/ha.qk"
printf 'fn main() {\n    print(0)\n    print(1)\n}\n' > "$tmp/hb.qk"
ha=$("$Q" graph "$tmp/ha.qk" --json | grep -o '"key": "main/@0", "hash": "[0-9a-f]*"' | sed 's/.*"hash": //')
hb=$("$Q" graph "$tmp/hb.qk" --json | grep -o '"key": "main/@1", "hash": "[0-9a-f]*"' | sed 's/.*"hash": //')
if [ -n "$ha" ] && [ "$ha" = "$hb" ]; then ok; else bad "a node's hash is its content's"; fi
# ... and a layout's entry that says it follows the node when its key moves (not to the node now at the old key)
printf '{"main/@0": {"pos": [10, 20], "hash": %s}}\n' "$ha" > "$tmp/lay.json"
moved=$("$Q" graph "$tmp/hb.qk" --layout "$tmp/lay.json" --json | grep -c '"key": "main/@1", "hash": [^,]*, "line": 3, "pos": \[10, 20\]')
stayed=$("$Q" graph "$tmp/hb.qk" --layout "$tmp/lay.json" --json | grep -c '"key": "main/@0", [^}]*"pos"')
if [ "$moved" = 1 ] && [ "$stayed" = 0 ]; then ok; else bad "a layout entry follows its node by hash"; fi
# syntax errors: each one said at once, and what could be read is still a program, and a graph (a hole where a
# statement could not be read)
if "$Q" check tests/recover/errors.qk 2>&1 | grep -v '^  ' | tr -d '\r' | cmp -s - tests/recover/errors.out; then ok; else bad "every syntax error said"; fi
"$Q" graph tests/recover/errors.qk > "$tmp/re.json" 2> /dev/null
if [ -s "$tmp/re.json" ] && "$Q" text "$tmp/re.json" | tr -d '\r' | cmp -s - tests/recover/errors.text; then ok; else bad "the graph of a program with errors"; fi
printf 'type Pair<A, B> { first: A, second: B }

fn swap<A, B>(p: Pair<A, B>) -> Pair<B, A> = Pair(p.second, p.first)
' > "$tmp/pair.qk"
if "$Q" catalog "$tmp/pair.qk" | grep -qF '"returns": "Pair<B, A>", "result": {"kind": "named", "name": "Pair", "args": [{"kind": "param", "name": "B"}, {"kind": "param", "name": "A"}]}'; then ok; else bad "catalog pins (a program's)"; fi
# the docs' examples compile, in canonical form (tests/docs.sh)
if bash tests/docs.sh "$Q" > "$tmp/docs.txt" 2>&1; then ok; else bad "the docs' examples"; grep -v '^docs:' "$tmp/docs.txt" | head -10; fi
rm -rf "$tmp"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
