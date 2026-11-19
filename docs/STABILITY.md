# Stability

What Quark promises to keep working, from version 1.0 on, and how anything that is promised may still change.
`quark version` prints the compiler's version and the version of each contract below.

```
quark 0.9.0 (windows, x86_64)
language edition 1, QGN 1, C API 1
```

Before 1.0 (the 0.x versions) nothing here is a promise yet: it is the policy 1.0 will keep, followed already so
that 1.0 holds no surprise.

## Versions

The compiler's version is `major.minor.patch`:

- **patch** (1.2.3 → 1.2.4): fixes only. A program that compiled keeps compiling and does what it did, unless
  what it did was the bug (a wrong result, a crash, a check that should have stopped it).
- **minor** (1.2 → 1.3): additions. New built-ins, new `std` modules and functions, new commands and options, new
  warnings, faster code. Every program that compiled keeps compiling and doing what it did.
- **major** (1 → 2): the only place a promise below may be broken, and only with the path described in
  [Changing what is stable](#changing-what-is-stable).

## What is stable

| Contract | What it covers | Its number |
| --- | --- | --- |
| The language | syntax, types, inference, effects, the meaning of every expression and statement, built-in functions and their names, what a program prints and returns, panics and their exit status | language edition |
| The standard library | every `pub` declaration of `std/*.qk`: its name, its parameters (names and order), its result, what it does | language edition |
| Quark Graph Notation | what `quark graph` writes and `quark text`, `diff` and `merge` read: node kinds, field names, keys, the JSON export | QGN |
| The C API | `include/quark.h`: libquark's functions, `qkVm*`, `quark object`'s symbols; the C ABI of `extern fn` and `extern type` (layouts and calling conventions) | C API |
| The command line | commands, options and their exit statuses (`0` success, `1` the program's or the input's fault, `2` usage) | compiler major |
| Packages | `quark.toml` and `quark.lock` fields | compiler major |

## What is not

These may change in any version, without notice:

- The **text of messages** (errors, warnings, panics). Tools should match on the exit status and on the
  `file:line:col:` prefix, not on the words.
- The **machine code** made, and so performance, the size of executables, and the order in which a `parallelMap`
  touches its elements (its result's order is stable).
- The **intermediate code** (`quark ir`), `quark code`'s bytes, the catalog's extra fields (`quark catalog`), and
  `--profile`, `--trace` and `--time` output.
- The **cache files** and their folder (`quark-cache` in the system's temporary folder): any version may ignore or
  remove them (`quark clean` does).
- **Experimental features**, named as such in the documentation: `QUARK_COUNTED=1` (the counted heap) and
  `QUARK_VALIDATE=1`. They may change or go away in a minor version.
- **Undocumented behavior.** What the documentation does not say a program does is not promised, even when a
  version happens to do it.

What `quark fmt` writes is the canonical form of a program: a new minor version may write it differently (a
better line break), and the program it formats means the same.

## Changing what is stable

A stable thing changes only in a major version, and only this way:

1. **Deprecated first.** The minor version before the major one keeps it working and warns where it is used, with
   what to write instead. A deprecation is listed in the release notes.
2. **Rewritten by the tools.** When the new form can be written from the old one, `quark fmt` rewrites it (the
   canonical rewrites of `compiler/canon.qk`), so a program moves to the new edition with one command.
3. **Editions side by side.** A new language edition will be chosen by a program (`edition = 2` in
   `quark.toml`; edition 1 is the only one so far, and the field is not read yet); the compiler of the new major
   version still compiles edition 1 programs, and modules of both editions call each other.

A security fix may break a promise in a patch version, when keeping it would keep a program unsafe. It is said
in the release notes.

## Supported platforms

| Target | Status |
| --- | --- |
| `x86_64-windows` | supported: the test suite runs on it for every change |
| `x86_64-linux` (glibc, musl) | supported: the test suite runs on it for every release |
| `arm64-linux` | supported: the test suite runs on it for every release |
| `x86_64-macos`, `arm64-macos` | built by `quark build --target`; not yet run on every release |
| `arm64-windows` | built by `quark build --target`; not yet run on every release |
| `wasm32` | `quark build --target wasm32`: what the language has without the system (no files, processes, network) |

A target that is not "supported" works as far as it was tried, and its gaps are bugs to be fixed, not promises
broken.
