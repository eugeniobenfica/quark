# How Quark is built

All of Quark is written in Quark: the compiler, its tools and its runtime. A Quark compiler builds it, as a Go
compiler builds Go: an installed `quark`, a release's, or one built before. No C compiler is needed, except to link
the C hosts the tests run.

## 1. What there is

- `compiler/` holds the compiler and its tools: the command line (`quark run`, `build`, `object`, `test`, `fmt`,
  `check`, `graph`, `text`, `diff`, `merge`, `doc`, `lsp`, `dap`, `get`, `ir`, `code`, `catalog`, `repl`, `debug`),
  the lexer, parser, checker, constant evaluator, formatter, node graphs, lowering, both optimizers, the x86-64 and
  ARM64 backends, and the writers of executables and objects.
- `compiler/runtime/` holds the runtime: the arena and its loop collection, texts, arrays, maps, printing and
  formatting, files, folders, processes, the environment, random numbers, tasks and their stacks, threads for
  `parallelMap` and `parallelFold`, panics and the calls they went through, the debugger's hooks. It reaches the
  operating system only through Quark's C interface, to the system's own C library: `windows.qk`, `linux.qk` and
  `macos.qk` give the same system calls behind symbols of their own (`qkos*`), and each executable's entry.
- `quark build` writes a native executable (PE on Windows, ELF on Linux, Mach-O on macOS, for this machine or, with
  `--target`, another) that imports the C functions it calls from the system's libraries. `quark run` maps the code
  in memory and runs it.
- C hosts embed Quark: `libquark` is the compiler and runtime compiled by Quark into an object, exporting
  `include/quark.h`'s API through `pub extern fn` (Windows and Linux; none on macOS yet).

## 2. Building

```bash
./build.sh            # QUARK=path/to/quark (else quark on the PATH) builds stage1 (build/stage1/quark.exe, the quick
                      # tier), and stage1 builds build/quark.exe (--release): what is built is made by today's
                      # compiler/, whatever made stage1
./build.sh --check    # then build/quark.exe builds stage3 (build/stage3/quark.exe): the same bytes, or it fails
```

Then `tools/libquark.sh` writes `build/libquark.a` with `build/quark.exe`, and, with a C compiler, the C hosts the
tests run are linked with it (`build/host.exe`, `build/sysv-hosts.exe`). `compiler/embed.sh` writes
`compiler/stdlib.qk` (the standard library and the runtime as texts) first. To change the compiler: edit
`compiler/`, then `QUARK=build/quark.exe ./build.sh --check` before a commit, and `bash tests/run.sh`.

A machine with no quark gets one from a release, or from another machine: `quark build compiler/mainlinux.qk
--target arm64-linux` makes Linux ARM64's anywhere (`--libc musl` for Alpine's). `bash tools/release.sh` makes a
release's eight (Windows, Linux with glibc and with musl, macOS; x86-64 and ARM64) and their `SHA256SUMS`; the CI
downloads its machine's from the repository's latest release. `python tools/vm/vm.py test` builds and tests in an
Alpine VM of another processor, with a quark made here for it.

A change to how the compiler's own code is made (a calling convention, a value's layout) needs no step of its own:
stage1 is made by the old compiler with the new compiler's code, and makes build/quark.exe the new way. A change
of the language the compiler's own code then uses needs a compiler that has it: the compiler's optimizing tier runs
`parallelMap` on its functions (values holding arrays), which compilers from before that rule do not take, so the
first build after it starts from a quark that has it (`QUARK=` one built from the change, and a release made from
it for the CI).

## 3. The compiler

### Layout

```
compiler/
  main.qk mainlinux.qk mainmacos.qk   the command line on each system (cli.qk), and what running in memory needs
                                      of it (imagecli.qk's Host: memory, code made runnable, C's functions)
  source.qk        files, positions, errors
  token.qk lexer.qk
  ast.qk           Expr, Stmt, Decl, Type, Arm... as enums and records (indices into arrays, not pointers)
  parser.qk  check.qk  consteval.qk  format.qk  graph.qk  clone.qk  module.qk  package.qk
  ir.qk            IrIns, IrFn, IrProgram
  lower.qk  opt.qk  release.qk  x64.qk  arm64.qk
  image.qk         machine code into executables and objects (PE, ELF, Mach-O, COFF), and into memory
  memory.qk        compiling, the runtime mapped and run in memory, executables built (their runtime part kept)
  lsp.qk  dap.qk  doc.qk  collab.qk  api.qk (the C API, as pub extern fns)  libquark.qk (its writer)
  runtime/         the runtime
```

### Data

Nodes link to each other by indices into arrays held by one `var` (a `Program` record: `exprs: [Expr]`, `stmts:
[Stmt]`, `types: [Type]`...), changed in place through `var self` functions. A node's index is its identity, so
"the same node" is `a == b` on two indices.

The IR is words too: an instruction (`IrIns`) holds no memory of its own, its call's arguments and results being in
its function's `operands` (`IrFn.argsOf`, `retsOf`, `setOperands`). Code is copied as a block, and a loop's
collection never looks into it. `--release` optimizes each function on its own (`IrProgram.optimize`), shared out to
the processor's threads with `parallelMap`; the result is the same bytes as one after the other. Each function's
optimized IR is kept by content (`compiler/cache.qk`): its key is a 128-bit hash of the function as lowered, with
what it names (other functions, data items, C functions, inlined calls, lines) said by what they are rather than
where this build put them, the target, the interface tables and the compiler's own executable; a hit is decoded
with those references put back where this build has them. A program's entries go in one pack in the temporary
folder (`quark-cache-*.bin`, with a hash of its contents: a damaged one is not read), rewritten with the build's
own. A warm `--release` build of the compiler takes 0.5 s to optimize instead of 6.

### Running in memory

The command line keeps the standard library and the runtime as texts, so `use std.json` reads no file and
`quark build` needs no runtime beside it. `compiler/memory.qk` compiles the runtime from those texts, links it,
maps it into the compiler's own memory (kept linked in the temporary folder, `quark-runtime-<os>-<hash of its texts
and of the compiler's own executable>.bin`, so a run does not compile it again, and `quark build` keeps an
executable's runtime part the same way, `quark-exe-...`; `QUARK_RUNTIME_CACHE=off` keeps none) and drives it
through `compiler/runtime/host.qk`'s functions: the program loaded once, then main or each test called with its own
memory, its panic's text (with the calls it went through: `runtime/trace.qk`), its output, the debugger
(`runtime/debug.qk`: `quark debug`'s commands, `--trace`, the debug adapter's stops as a task's turns) and the
profiler's samples (`--profile`: a thread on Windows, `SIGPROF` on Linux and macOS).

The program calls the runtime's functions the Quark way, by number: the runtime's table holds each one's address
(`machine.fillRuntime`), and a linked object names them `qk_<name>`. The machine's context (the running thread's
state) rides in a register of its own, r15 on x86-64 and x28 on ARM64; every way in from C makes it 0, and the
runtime then finds it in the thread's own storage. `--abi sysv` on Windows (a test of the System V calls) changes
only the way in.

Quark calls Quark (and the runtime, and `std.machine`) its own way. On x86-64 the first five slots of the arguments
go in rcx, rdx, r8, r9, r10 and the rest to the caller's outgoing area (`[rsp + 8i]`, read by the callee at
`[rbp + 16 + 8i]`); results come back the same way. The allocator keeps values that no call crosses in r8-r10 too,
so each crossing (the arguments before a call, the parameters in the prologue, the results after a call and before a
return) is made as one parallel move. The ways in from outside (the trampoline the runtime calls programs through,
the stubs C calls) load and store those registers around their call. ARM64 still passes every slot in the outgoing
area.

### libquark

`compiler/api.qk` is `include/quark.h`, each function a `pub extern fn` (and `qkAbi`, `qkModuleReader` places in
the data the writer exports as C's variables). `compiler/apiwindows.qk` / `apilinux.qk` add the runtime's system
calls; `tools/libquark.sh` compiles that root (`compiler/libquark.qk`: linked, for objects), leaves out the
executables' entries (`wmain`, `main`...) and writes the object as an archive (`compiler/image.qk`'s `archive`):
`build/libquark.a`, no C compiler and no `ar`. The machine, modules and tasks live in C's memory; the compiler runs
on a thread whose stack is 256 MB and allocates in the library's arena, emptied when the next compile starts; calls
run on the runtime's loader and trampoline, in the machine's arena.
