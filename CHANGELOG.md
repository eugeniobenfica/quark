# Changelog

## 0.9.1

Fixes found by the first runs of the CI on every platform.

- **System V (Linux and macOS on x86-64).** C may change every xmm register, while Quark's own calls keep
  xmm8-15: around each call into C all eight now wait in the frame, whole, not only the ones the calling function
  uses. Before, a caller's Float in one of them could come back changed (the 0.9.0 compiler for x86-64 Linux failed
  on its own build).
- **libquark on Windows** is linked with `ws2_32` (its sockets).
- **The C API's module table** (`qkModule*`) lists the module's functions only, not the language's own
  (`compiler/prelude.qk`).

## 0.9.0

The first public release: the language, its compiler written in Quark, and what the 1.0 will keep stable
([STABILITY.md](docs/STABILITY.md)).

- **Compiler.** Written in Quark and built by itself to the same bytes; a quick tier and an optimizing one
  (`--release`); x86-64 and ARM64; PE, ELF and Mach-O executables with no linker; `wasm32`.
- **Language.** Inferred static types, records, enums with data, exhaustive `match` (constants as patterns),
  `T?`, `T!`, `T ! E` and `T?!`, generics, interfaces, closures, inferred effects, holes (`_`).
- **Memory.** An arena per call and a collection per loop's turn, with a side table that finds what was copied.
- **Concurrency.** Tasks in the background (`go`, `runTasks`), channels, sockets that wait only their own task;
  `parallelMap` and `parallelFold`.
- **C.** `extern fn` both ways, C function values, `quark bind` for C headers, libquark for C and C++ hosts.
- **Graph.** QGN and JSON, the round trip to text, `quark diff` and `quark merge` by node.
- **Library.** `json`, `http`, `net`, `files`, `csv`, `regex`, `time`, `math`, `collections`, `encoding`,
  `hash`, `args`, `path`, `testing`.
- **Tools.** `fmt`, `test` (`--filter`), `lsp`, `dap`, `debug`, `doc`, `repl`, `bind`, `get`, `version`, `clean`.
