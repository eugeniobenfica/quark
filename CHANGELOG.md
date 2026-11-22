# Changelog

## Unreleased

- **Incremental builds:** a program whose reading and checking take more than 150 ms is kept as an image after a cold
  build, with its machine code function by function. A build of the same files then comes from it (the compiler's in
  65 ms), and a build after an edit of function bodies checks and lowers again only those functions, and those that
  put one of them in line or read a fact of one that changed (the compiler's in 0.35 s). `QUARK_IMAGES=off` turns it
  off, `always` keeps every program's.
- **Fixed:** a built program's bounds or division panic crashed instead of saying where it was.
- **Arrays written into a container from before the loops running are made old for them:** `xs[k] = v` in
  `for k in ...`, `v` a new array of plain values (or a record holding such), goes straight to memory the loops' minor
  collections neither copy nor look at; a full collection of the outermost of them takes back what of it is
  overwritten. `records` (an array of records rewritten in place) went from 84 to about 68 ms on Windows. A program
  with no such write pays nothing for it.
- **A value received from a channel is not copied again:** `send` copies it once, into memory of its own, and
  `receive` makes that memory the receiver's whole. The same for `parallelMap`'s results: each thread's are the
  caller's memory as the thread kept them. The compiler's cold build is 7% faster for it (its checked program comes
  out of a task), and peaks lower (1.41 GB).
- **`parallelMap`'s threads keep their copy of the program** from one call to the next, rather than loading it again.

## 0.12.1

What the next compiler builds on (it is built by this one, with this one's standard library and runtime).

- **Images** (`std.machine`, the runtime's own): `saveImage(value, path)` writes a value with all it holds as its
  memory is, at a fixed place of addresses, and `loadImage(path)` reads it back there at once (44 MB in 9 ms). The
  compiler will keep its checked programs in them.
- **A task's stack** is 64 MB on Linux and macOS (memory taken as it is touched), 8 MB on Windows: checking a large
  program in a task needs more than 8 MB on ARM64.
- **A line in machine code** is written at a fixed width, and where it is noted: kept code can have it rewritten.
- **Panics from `runtimeLine` calls** say the file of the inlined call they are in.

## 0.12.0

### Changed: the freeze review

The standard library read once more as the API 1.0 will keep ([STABILITY.md](docs/STABILITY.md#the-standard-librarys-names)).
These renames break programs that use the old names; each is the same function.

- **A name says what it is about without its module** (`use std.http` brings its names in beside a file's own):
  - `std.http`: `get`, `post`, `request`, `serve`, `handleConnection` are `httpGet`, `httpPost`, `httpRequest`,
    `serveHttp`, `serveConnection`;
  - `std.path`: `normalize`, `parent`, `extension`, `stem`, `isAbsolute` are `normalizePath`, `parentPath`,
    `fileExtension`, `fileStem`, `isAbsolutePath`;
  - `std.json`: `encode`, `decode` are `encodeJson`, `decodeJson`;
  - `std.time`: `now()` is `utcNow()`; `std.args`: `usage` is `argsUsage`;
  - the prelude: `tlsConnect(socket, serverName)` is `startTls` (it starts TLS on a socket already connected).
- **A function that gives a new value is named by what it gives:** `Queue`'s `pushBack`, `pushFront`, `popFront`,
  `popBack` are `withBack`, `withFront`, `withoutFront`, `withoutBack`; `Heap`'s `push`, `pop`, `merge` are
  `added`, `withoutTop`, `merged`; `Date`'s and `DateTime`'s `addDays`, `addMonths`, `addSeconds` are `plusDays`,
  `plusMonths`, `plusSeconds`.
- **Opaque where the parts are the type's business:** `Queue`, `Heap` (made by `emptyQueue()`, `queueOf(xs)`,
  `emptyHeap()`, `heapOf(xs)`; `Queue.Empty` and `Heap.Empty` are gone) and `Regex` (`r.pattern`, `r.groups`).
- **`parseUrl(url)`** gives a `Url` (`secure`, `host`, `port`, `path`), not a tuple.
- **`std.json`'s helpers** for the code made for each type (`intOfJson`, `fieldFromJson`...) are no longer `pub`.
- **The C API:** `QkDiagnostic` holds its texts as pointers (valid until the machine's next call), no longer cut
  at 96 and 320 bytes, and says the file the diagnostic is in (`file`). Code that reads `d->message` and
  `d->key[0]` compiles as before.

### Added

- **TLS** through the system's library (Schannel, OpenSSL loaded on first use, SecureTransport): `startTls`,
  `readTls`, `writeTls`, `closeTls`; `connectTls` in `std.net`; `https://` in `std.http`, a server by name or by IP
  address.
- **Typed JSON:** `encodeJson(value)` and `decodeJson(text)` for records, enums, arrays, maps, optionals and
  tuples, made by the checker for each type used.
- **HTTP:** bodies in chunks (`Transfer-Encoding: chunked`) read, client and server; a response with no length
  read to the connection's end.
- **Subprocesses as streams:** `startProcess`, `writeProcess`, `readProcess`, `readProcessErrors`,
  `closeProcessInput`, `waitProcess`, `killProcess`.
- **Time zones by name:** `zone("Europe/Lisbon")` in `std.time`, from the system's TZif files or `std.zones`.
- **Concurrency:** `Channel<T>` of any type; everything that waits is cooperative (a system call that waits runs
  on a thread of its own); `cancel`, `withTimeout`, `receiveWithin`; structured concurrency (`scope`, `s.start`).
- **The language:** `?` converts errors; interfaces with `Self` and default implementations; `pub` per field of an
  opaque type; `owned` parameters (the function keeps and changes the value with no copy).
- **Packages:** semantic versions (minimal version selection) and a registry (`index.toml`), `quark get` with a
  hash.
- **Tools:** the LSP across files (rename, references, implementations, types and effects on hover, incremental
  diagnostics); `quark test --coverage`; properties in `std.testing` (`forAll`, generators, shrinking); `-g` debug
  information (DWARF 4, CodeView, gdb's JIT interface); `wasm32-wasi` with the suite green.
- **Guides:** coming from Go, Rust and Python; servers; the prelude's reference made by `quark doc`.

### Faster, smaller

- `records` from 1.95x to 1.44x C (the loop collector); the quick tier's optimizing and machine code on threads;
  `quark run` of a small program in 20 ms; constants kept by content between builds.
- Executables hold only the runtime the program reaches (hello: 911 to 136 KB); the Windows runtime needs only
  kernel32 and ntdll; Floats written and read without C.
- Large arrays grow where they are (reserved addresses): no copy, no old block left behind.
- The compiler lowers, optimizes and makes machine code in batches, keeping only the code: its own rebuild peaks
  at 2.1 GB (was 3.1).

## 0.9.2

- **System V (x86-64 Linux and macOS).** 0.9.1 brought xmm8-15 back after a call into C over the call's result when
  the result's home was one of them; they now come back before the results go home.
- **Tests.** The C hosts run only where a C compiler linked them with the libquark just built (an older one hid
  this), and a host whose output differs shows how.

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
