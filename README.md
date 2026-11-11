# Quark

A small compiled language whose programs are also node graphs. It is lean, as fast as C where it
counts, and depends on nothing.

```quark
fn luma(r: Int, g: Int, b: Int) -> Int = round(0.2126 * r + 0.7152 * g + 0.0722 * b)

fn main() {
    let frame = (0..384 * 216 * 3).map(i => i * 7 % 256)
    let light = (0..384 * 216).histogram(256, p => luma(frame[p * 3], frame[p * 3 + 1], frame[p * 3 + 2]))
    print(light.max(), "pixels share the most common light")
}
```

```text
quark run hello.qk          compiled to x86-64 in under a millisecond, then run
quark build hello.qk        hello.exe: runs anywhere, needs nothing
quark graph hello.qk        the same program as a node graph (JSON)
quark text hello.json       ... and back, to the same text
quark fmt hello.qk          the one canonical layout
```

**MVP.** This is the first version. It is the language's core and its compiler, and it is not yet
connected to Montage, Depict or Artifice.

## The idea

- **Elegant.** 19 keywords. camelCase for values and PascalCase for types, enforced. One canonical
  layout. Values that never change behind your back. See [docs/LANGUAGE.md](docs/LANGUAGE.md).
- **Fast.** Quark compiles to machine code itself, with no C compiler, no LLVM and no interpreter. A
  pipeline (`map`, `filter`, `histogram`...) becomes one fused loop. Lambdas and one-expression
  functions are inlined. Records and vectors live in registers.
- **A node graph.** Every `let` is a named node and every call a node with input pins. The order of
  statements is the execution wire. Text → graph → text gives back the same text, comments included.
  This is checked for every file by the tests.
- **No dependencies.** `quark.exe` is about 200 KB of C11. It parses, checks, compiles and runs
  programs in memory, and writes standalone executables.

## Speed

Each benchmark runs the same work in Quark, in C compiled with `gcc -O2`, and in Lua 5.4 (the version
Montage embeds). Measured on Windows x64, best of three:

| | C (gcc -O2) | **Quark** | Lua 5.4 |
| --- | --- | --- | --- |
| Scopes: four histograms of a 384 × 216 frame, 200 times ([scopes.qk](bench/scopes.qk)) | 29 ms | **39 ms** | 915 ms |
| Mandelbrot, 1000 × 1000, 100 steps ([mandelbrot.qk](bench/mandelbrot.qk)) | 32 ms | **38 ms** | 256 ms |

Quark is 1.2–1.4× the time of optimized C and 7–23× faster than Lua 5.4. It also checks every
array index, which the C code does not. Compiling a program takes about 0.2 ms.

Where the speed comes from:

- Loops are fused, and lambdas and one-line functions are inlined.
- A linear-scan register allocator weighs loops more.
- Integer constants become immediates and float constants are hoisted.
- A comparison followed by a jump becomes one compare-and-branch.
- Error paths (bounds, division by zero, budget) sit out of line.

Next steps for speed: a smarter allocator (splitting), unboxed small element types, SIMD, and threads
for `map` and `histogram`.

## Build

The compiler is C11 with no library beyond the C runtime. Any C compiler builds it, but Quark
programs never need one.

```bash
./build.sh            # or build.bat (MSYS2 gcc), or: cc -std=c11 -O2 -o build/quark.exe src/*.c -lm
tests/run.sh          # programs, errors, panics, canonical text, graph round trips, quark build
```

## Inside

| File | |
| --- | --- |
| `src/lexer.c` | text → tokens; names are camelCase / PascalCase; line breaks end statements |
| `src/parser.c` | tokens → syntax tree, comments attached |
| `src/check.c` | types (inferred), conversions, which loops fuse |
| `src/lower.c` | tree → register IR: fused loops, inlining, values as 8-byte slots |
| `src/opt.c` | copies folded, dead code dropped, constants as immediates |
| `src/x64.c` | IR → x86-64 bytes: linear scan, instruction encoding, no assembler |
| `src/runtime.c` | what compiled code calls: print, arena memory, math, panics; the loader |
| `src/format.c` | the canonical text |
| `src/graph.c` | node graph (JSON) out and in |
| `src/main.c` | the command line; executables carry their machine code at their end |

About 6,900 lines. Today the backend targets x86-64 Windows. System V (Linux, macOS) and ARM64
backends come next. The IR is shared, so only `x64.c`'s calling convention and a second encoder
differ.

## Status and next steps

The MVP is done: the language core, a native compiler, the formatter, node graphs both ways,
standalone executables, and tests.

Next, in the order that leads to replacing Lua in Montage, Depict and Artifice:

1. An embedding API (`quark.h`: compile, find a function, call it with a struct), with host functions
   declared by `extern` and capabilities (`effect`) in place of the Lua sandbox.
2. The plugin API: the `draw(o, g)` of objects and panels, and commands with `doc`.
3. A node editor view in Cascade, reading and writing `quark graph` / `quark text`.
4. System V and ARM64 backends.
