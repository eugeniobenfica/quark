# Changelog

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
