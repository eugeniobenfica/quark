# Quark

A small, general-purpose compiled language whose programs are also node graphs. It is lean, as fast
as C where it counts, and depends on nothing.

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

**Ready to embed.** A C program links `build/libquark.a`, gives Quark its API as `extern fn`s, and
calls scripts and node graphs: see [docs/EMBEDDING.md](docs/EMBEDDING.md). How Artifice's node system
maps onto it is in [docs/ARTIFICE.md](docs/ARTIFICE.md). It is not connected to Montage, Depict or
Artifice yet.

## The idea

- **Elegant.** 22 keywords. camelCase for values and PascalCase for types, enforced. One canonical
  layout. Values that never change behind your back. See [docs/LANGUAGE.md](docs/LANGUAGE.md).
- **Whole.** Inferred static types, and:
  - records with `with` copies;
  - enums whose cases carry data, taken apart by `match`;
  - optionals (`T?`, `Some`, `None`, `??`);
  - text that joins, compares and interpolates (`"lap {n}"`);
  - arrays that grow (`push`);
  - constants, and named and default arguments.

  Code that changes the world is marked `effect fn`, so everything else is pure and can be fused,
  cached or, later, run in parallel. An editor can tell actions from data at a glance.
- **Fast.** Quark compiles to machine code itself, with no C compiler, no LLVM and no interpreter. A
  pipeline (`map`, `filter`, `histogram`...) becomes one fused loop. Lambdas and small
  functions are inlined. Records and vectors live in registers.
- **A node graph.** Every `let` is a named node and every call a node with input pins. The order of
  statements is the execution wire. Text → graph → text gives back the same text, comments included.
  This is checked for every file by the tests.
- **No dependencies.** `quark.exe` is about 200 KB of C11. It parses, checks, compiles and runs
  programs in memory, and writes standalone executables.

## Speed

Each benchmark runs the same work in Quark, in C compiled with `gcc -O2`, and in Lua 5.4 (the version
Montage embeds). Measured on Windows x64, best of five:

| | C (gcc -O2) | **Quark** | Lua 5.4 |
| --- | --- | --- | --- |
| Scopes: four histograms of a 384 × 216 frame, 200 times ([scopes.qk](bench/scopes.qk)) | 28.6 ms | **29.6 ms** | 915 ms |
| Mandelbrot, 1000 × 1000, 100 steps ([mandelbrot.qk](bench/mandelbrot.qk)) | 32.3 ms | **35 ms** | 256 ms |

Quark is within 3–8% of optimized C and 7–30× faster than Lua 5.4. It also checks every array
index and rounds integer division down, which the C code does not. Compiling a program takes
about 0.2 ms.

Where the speed comes from:

- **Loops fuse.** `map` / `filter` / `histogram` / `fold` become one machine loop with the test at
  the bottom, and loop tops are aligned.
- **Small functions are inlined**, block bodies with `return`s included. Lambdas always are.
- **Registers.** A linear-scan allocator weighs loops more. Values that no call crosses go to
  volatile registers, so nothing is saved for them. Records and vectors live in registers.
- **The optimizer:**
  - constants are folded;
  - common subexpressions are computed once (`p * 3` for the three loads of a pixel);
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

Next steps for speed:

- range analysis, to drop bounds checks and sign fixes proven unneeded;
- unboxed small element types (`[Byte]`, `[Float32]`);
- SIMD for `map`;
- threads for `map` and `histogram`.

## Build

The compiler is C11 with no library beyond the C runtime. Any C compiler builds it, but Quark
programs never need one.

```bash
./build.sh            # build/libquark.a, build/quark.exe, build/host.exe (or build.bat with MSYS2)
tests/run.sh          # programs, errors, panics, canonical text, graph round trips, quark build, the host
```

## Embedding

```c
QkVm* vm = qkVmNew();
qkVmPrelude(vm, "extern type Object\n"
                "extern effect fn moveBy(o: Object, dx: Float, dy: Float)\n");
qkVmHost(vm, "moveBy", (void*)moveBy);               // void moveBy(int64_t o, double dx, double dy)
QkModule* m = qkCompile(vm, "ship.qk", text, len);   // NULL: qkDiagnosticAt says why
QkValue args[2] = {{.i = id}, {.f = 1.0 / 60}};
if (!qkCall(m, qkFind(m, "update"), args, 2, NULL, 0))
    puts(qkLastError(vm));                            // a script's panic never ends the host
```

The API covers:

- host functions with any number of arguments, struct results, text and handles;
- a budget per call, and panics that stop the call instead of the host;
- an arena per call, and hot reload (compile, swap the module);
- graphs both ways, with `"pos"` from a layout file and errors keyed to nodes;
- a catalog of every node an editor can offer.

[`examples/host/host.c`](examples/host/host.c) is a small engine in Artifice's style that uses all of
it, and the tests check its output.

## Inside

| File | |
| --- | --- |
| `src/lexer.c` | text → tokens; names are camelCase / PascalCase; line breaks end statements |
| `src/parser.c` | tokens → syntax tree, comments attached |
| `src/check.c` | types (inferred), conversions, which loops fuse |
| `src/lower.c` | tree → register IR: fused loops, inlining, values as 8-byte slots |
| `src/opt.c` | constants folded, common subexpressions, shared divisions, copies folded, dead code dropped |
| `src/x64.c` | IR → x86-64 bytes: linear scan, SSE / AVX encoding, magic division, no assembler |
| `src/runtime.c` | what compiled code calls: print, arena memory, math, panics; the loader |
| `src/format.c` | the canonical text |
| `src/graph.c` | node graph (JSON) out and in |
| `src/api.c` | the embedding API (`include/quark.h`): machines, preludes, host functions, calls, catalog |
| `src/main.c` | the command line; executables carry their machine code at their end |
| `examples/host/` | an engine in Artifice's style: its prelude, a ship script, the C host |

About 9,800 lines. Today the backend targets x86-64 Windows. System V (Linux, macOS) and ARM64
backends come next. The IR is shared, so only `x64.c`'s calling convention and a second encoder
differ.

## Status and next steps

Done:

- the language and a native compiler;
- text, optionals, enums with data, `with`, constants, named and default arguments, `push`, and
  effects;
- the formatter, and node graphs both ways;
- standalone executables;
- the embedding API, with host functions, safe calls, hot reload, node keys and layout, and the
  catalog;
- tests.

Next, in the order that leads to replacing Lua in Artifice, Montage and Depict:

1. System V and ARM64 backends: Linux, macOS and Android exports.
2. Artifice: its prelude (one `extern fn` per node) and its node editor reading Quark's graphs. See
   [docs/ARTIFICE.md](docs/ARTIFICE.md).
3. Generic functions and functions as parameters (by specialization), maps, and modules (`use`).
4. Montage's plugin API: the `draw(o, g)` of objects and panels, and commands with `doc`, given as
   capabilities.
