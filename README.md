# Quark

A small, general-purpose compiled language whose programs are also node graphs. It is lean, as fast
as C where it counts, and depends on nothing.

```quark
type Vec2 { x: Float, y: Float }

fn Vec2.plus(self, b: Vec2) -> Vec2 = Vec2(self.x + b.x, self.y + b.y)

fn Vec2.length(self) -> Float = sqrt(self.x * self.x + self.y * self.y)

fn parsePoint(line: Text) -> Vec2! {
    let xy = words(line)
    if len(xy) != 2 {
        return Err("expected two numbers: {line}")
    }
    Vec2(parseFloat(xy[0]) ?? 0, parseFloat(xy[1]) ?? 0)
}

fn main() {
    var steps: [Vec2] = []
    for line in lines(readFile("points.txt")?) {
        steps.push(parsePoint(line)?)
    }
    let end = steps.fold(Vec2(0, 0), (sum, step) => sum + step)
    print("{len(steps)} steps, ending at ({end.x:.1}, {end.y:.1}), {end.length():.2} away")
}
```

```text
quark run hello.qk -- a b   compiled to x86-64 or ARM64 in under a millisecond, then run (args() is [a, b])
quark build hello.qk        a standalone executable: runs anywhere, needs nothing
quark test hello.qk         runs its test "..." { } blocks
quark fmt hello.qk          the one canonical layout
quark lsp                   a language server: errors, types on hover and inline, definition, references, rename
quark repl                  declarations and expressions, line by line
quark object hello.qk       an object file a C host links (no code made while running: iOS, consoles), any target
quark debug hello.qk        breakpoints, stepping, the names' values
quark run hello.qk --trace  the last value of every node, for node editors
quark graph hello.qk        the same program as a node graph (JSON)
quark text hello.json       ... and back, to the same text
quark diff a.qk b.qk        what changed, by node key (added, removed, changed inputs, moved)
quark merge base a b        a three-way merge by node key; conflicts named by key
```

**Ready to embed.** A C program links `build/libquark.a`, gives Quark its API as `extern fn`s, and
calls scripts and node graphs: see [docs/EMBEDDING.md](docs/EMBEDDING.md). An application, an
editor, a server, a tool or any other C program can use it as its scripting language.

## The idea

- **Elegant.** 26 keywords. camelCase for values and PascalCase for types, enforced. One canonical
  layout. Values that never change behind your back. See [docs/LANGUAGE.md](docs/LANGUAGE.md).
- **Whole.** Inferred static types, and:
  - records with `with` copies, compared with `==` field by field;
  - generic records and enums (`type Pair<A, B>`, `enum Tree<T>`), and types that hold themselves
    (trees, lists, expressions);
  - functions of a type (`fn Vec2.length(self)`, called `v.length()`), and operators through them:
    `fn Vec2.plus` makes `a + b` work, `fn Version.compare` makes `<` and `sort` work;
  - interfaces (`type Speaker { fn speak() -> Text }`): any type with those functions goes into one;
  - enums whose cases carry data, taken apart by `match`, with patterns for ranges (`90..`),
    texts (`"quit"`) and guards (`Some(n) if n > 0`);
  - optionals (`T?`) and results (`T!`, an error with a message), with `??`, `?` that passes a
    failure on, `let Some(x) = ... else { return }`, and `x == None`; `Never` for what never comes
    back (`exit`, `panic`), so `x ?? panic("...")` works anywhere;
  - generic functions (`fn top<T>(...)`) and functions that take functions (`f: (T) -> Int`), both
    specialized where they are used, so they cost nothing; and functions kept as values, in a `let`,
    a field or a map (`onPress: (Int) -> Int`), for callbacks;
  - arrays that grow (`push`) and join (`xs + ys`), maps (`[Text: Int]`, loop sources of
    `Entry(key, value)` too), slices (`xs[2..]`, `t[..3]`);
  - UTF-8 text that joins, compares, interpolates and formats (`"{x:.2}"`), splits, searches,
    replaces and parses, with `upper` / `lower` that know accents, Greek and Cyrillic;
  - files that use files (`use geometry`, `pub fn ...`);
  - `parallelMap`: a pure function on every element, on all the processor's threads;
  - take, skip, countBy, mean, sort and sortBy (either way: `descending: true`), reverse, first, last,
    count, minBy, maxBy, contains, indexOf, join;
  - constants, named and default arguments;
  - files, folders, the environment, standard input and output, the command line, exit codes;
  - tests (`test "..." { assert(...) }`, run by `quark test`).

  The compiler works out which functions change the world (print, files, the clock, a host function
  marked `effect`...), the way it works out types: nothing is written, and nothing is paid when the
  program runs. Everything else is pure and can be fused, cached or run in parallel, and an editor
  can tell actions from data at a glance.
- **Fast.** Quark compiles to machine code itself, with no C compiler, no LLVM and no interpreter. A
  pipeline (`map`, `filter`, `fold`...) becomes one fused loop. Lambdas and small
  functions are inlined. Records live in registers.
- **Lean on memory.** Allocating is a pointer bump in an arena. A loop that makes garbage on every
  turn gives it back as it goes, keeping only what its variables hold: no collector, no pauses, and
  a long-running program stays small.
- **A node graph.** Every `let` is a named node and every call a node with input pins. The order of
  statements is the execution wire. Text → graph → text gives back the same text, comments included.
  This is checked for every file by the tests.
- **No dependencies.** `quark.exe` is a few hundred KB of C11. It parses, checks, compiles and runs
  programs in memory, and writes standalone executables.

## Speed

Each benchmark runs the same work in Quark, in C compiled with `gcc -O2`, and in Lua 5.4 (a common
embedded scripting language). Measured on Windows x64, best of five:

| | C (gcc -O2) | **Quark** | Lua 5.4 |
| --- | --- | --- | --- |
| Counting: 82,944 rows of three values counted four ways into 256 buckets, 200 times ([counting.qk](bench/counting.qk)) | 28.7 ms | **34.1 ms** | 915 ms |
| Mandelbrot, 1000 × 1000, 100 steps ([mandelbrot.qk](bench/mandelbrot.qk)) | 32.2 ms | **34.9 ms** | 256 ms |

Quark is within 8–19% of optimized C and 7–27× faster than Lua 5.4. It also checks every array
index and rounds integer division down, which the C code does not. Compiling a program takes
about 0.2 ms.

Where the speed comes from:

- **Loops fuse.** `map` / `filter` / `fold` / `sum` become one machine loop with the test at
  the bottom, and loop tops are aligned.
- **Small functions are inlined**, block bodies with `return`s included. Lambdas always are, and so
  are functions that take functions; generic functions get a copy per type.
- **Registers.** A linear-scan allocator weighs loops more. Values that no call crosses go to
  volatile registers, so nothing is saved for them. Records live in registers, and their
  operators (`a + b` through `Vec2.plus`) inline to plain float instructions.
- **The optimizer:**
  - constants are folded;
  - common subexpressions are computed once (`i * 3` for the three loads `xs[i * 3]`, `xs[i * 3 + 1]`, `xs[i * 3 + 2]`);
  - copies are folded into the instruction that computes them;
  - unused code is dropped;
  - float constants are hoisted and int constants become immediates.
- **Instruction choice:**
  - division by a constant becomes a multiplication by a magic number;
  - `x / d` and `x % d` share one;
  - `x * 3` becomes one `lea`, and `x * 2.0` becomes `x + x`;
  - `counts[k] += 1` becomes one `add [mem], 1`;
  - a compare and its branch become one instruction pair;
  - conversions go straight to their register, without the false dependency of `cvtsi2sd`;
  - three-operand AVX is used when the processor has it (`--no-avx` for one that does not).
- **Error paths** (bounds, division by zero, budget) sit out of line, so loops fall through.
- **Memory only where it is made.** A loop that allocates nothing (all the benchmarks' hot loops)
  carries no memory bookkeeping at all.

Next steps for speed:

- range analysis, to drop bounds checks and sign fixes proven unneeded;
- unboxed small element types (`[Byte]`, `[Float32]`);
- SIMD for `map`;
- threads for `map` and `fold`;
- `counts[k] += 1` loops as tight as C's: the counting benchmark spends most of its 19% there.

## Build

The compiler is C11 with no library beyond the C runtime. Any C compiler builds it, but Quark
programs never need one.

```bash
./build.sh            # build/libquark.a, build/quark.exe, build/host.exe (or build.bat with MSYS2)
tests/run.sh          # programs, errors, panics, canonical text, graph round trips, quark build, tests, the language server, System V calls, the host
```

## Embedding

```c
QkVm* vm = qkVmNew();
qkVmPrelude(vm, "extern type Item\n"
                "extern effect fn moveBy(i: Item, dx: Float, dy: Float)\n");
qkVmHost(vm, "moveBy", (void*)moveBy);               // void moveBy(int64_t i, double dx, double dy)
QkModule* m = qkCompile(vm, "item.qk", text, len);   // NULL: qkDiagnosticAt says why
QkValue args[2] = {{.i = id}, {.f = 1.0 / 60}};
if (!qkCall(m, qkFind(m, "tick"), args, 2, NULL, 0))
    puts(qkLastError(vm));                            // a script's panic never ends the host
```

The API covers:

- host functions with any number of arguments, struct results, text and handles;
- a budget per call, and panics that stop the call instead of the host, with the calls they went
  through;
- an arena per machine, and hot reload (compile, swap the module);
- graphs both ways, with `"pos"` from a layout file and errors keyed to nodes;
- a catalog of every node an editor can offer;
- capabilities: what each program may call (`qkVmCapabilities(vm, "Item.* print")`), checked before
  it runs, so a third-party plugin can be given less than the application's own scripts.

[`examples/host/host.c`](examples/host/host.c) is a small application whose items run scripts; it uses
all of it, and the tests check its output.

## Inside

| File | |
| --- | --- |
| `src/lexer.c` | text → tokens; names are camelCase / PascalCase; line breaks end statements |
| `src/parser.c` | tokens → syntax tree, comments attached |
| `src/check.c` | types (inferred), conversions, which loops fuse; generic functions and functions that take functions made into instances |
| `src/clone.c` | copies of a syntax tree: a template's body for each instance |
| `src/lower.c` | tree → register IR: fused loops, inlining, values as 8-byte slots, loops that give memory back |
| `src/opt.c` | constants folded, common subexpressions, shared divisions, copies folded, dead code dropped |
| `src/x64.c` | IR → x86-64 bytes: linear scan, SSE / AVX encoding, magic division, Windows x64 and System V calls, no assembler |
| `src/arm64.c` | IR → ARM64 words: the same linear scan over x9-x15 / x19-x28 and d8-d31, AAPCS64 calls (records back in x0-x1, d0-d3 or through x8) |
| `src/runtime.c` | what compiled code calls: print, text, maps, the arena and its loop collections, files, panics with their calls; the loader |
| `src/format.c` | the canonical text |
| `src/graph.c` | node graph (JSON) out and in |
| `src/collab.c` | versions side by side: diffs and three-way merges by node key |
| `src/api.c` | the embedding API (`include/quark.h`): machines, preludes, host functions, calls, catalog |
| `src/lsp.c` | the language server |
| `src/module.c` | `use`: the files a program names, read and joined to it |
| `src/main.c` | the command line; executables carry their machine code at their end |
| `examples/host/` | a small host application: its prelude, an item's script, the C host |

There are two backends, and the build picks the one of the processor it is built on:

- **x86-64**, for two calling conventions: Windows x64, and System V (Linux, macOS on Intel). The
  System V code is tested on Windows by calling the runtime through adapters (`quark run --abi sysv`)
  and a host whose functions use GCC's `sysv_abi`.
- **ARM64** (Linux, macOS on Apple Silicon, Android, Raspberry Pi), with AAPCS64 calls. Every test
  passes on ARM64 Linux, run in an emulated machine by `tools/arm64/vm.py` (QEMU and Alpine Linux:
  `boot`, then `test`). On Apple Silicon the code goes into `MAP_JIT` memory; that path is written but
  not yet run on a Mac.

`quark code file.qk -o code.bin` writes the raw machine code and each function's offset, for a
disassembler (`objdump -D -b binary -m aarch64` or `-m i386:x86-64`).

## Status and next steps

Done:

- the language and a native compiler;
- text with formats, optionals and results with `?`, `let ... else` and `==`, enums with data,
  patterns with ranges, texts and guards, `with`, constants, named and default arguments, arrays,
  maps, slices, interfaces, `Never`, and effects (inferred);
- generic functions and generic types, by specialization; types that hold themselves;
- functions that take functions (inlined), and functions kept as values (called through);
- functions of a type, user-defined operators and order;
- files that use files (`use`, `pub`);
- a standard library for UTF-8 text, arrays, maps, files, folders, the environment and the command
  line;
- memory that loops give back as they go, and an arena per machine;
- `parallelMap` on the processor's threads;
- the formatter, node graphs both ways (for external node editors), tests (`quark test`), a language server
  (`quark lsp`), and panics that name the calls they went through, too-deep recursion included;
- standalone executables;
- the embedding API, with host functions, capabilities, safe calls, hot reload, node keys and layout,
  and the catalog;
- two backends: x86-64 (Windows x64 and System V calls) and ARM64 (AAPCS64), every test passing on
  both.

Next:

1. A WebAssembly backend (the browser).
2. Run on Apple machines: the Mach-O objects link with LLVM's linker and their loads land on their table,
   but the runtime has not yet been built and run on a Mac.
3. Windows on ARM.
4. `fold` across threads, SIMD for `map`, small element types (`[Byte]`, `[Float32]`).

Node editors are tools of their own, outside the language: they read and write the files quark gives them
(`quark graph`, `quark text`, `quark catalog`, `quark diff`, `quark merge`, `--layout`, `--trace`).
