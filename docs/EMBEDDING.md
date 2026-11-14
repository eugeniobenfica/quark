# Embedding Quark

Any C program (an application, an editor, a server, a tool) links `build/libquark.a` and includes
[`include/quark.h`](../include/quark.h). With that it can compile Quark, call it, give it host
functions, and turn programs into node graphs and back. Nothing else is needed: no C compiler at
run time and no other library. The complete working example is
[`examples/host/host.c`](../examples/host/host.c), a small application whose items run scripts. Its
output is checked by the tests.

## The machine and the host's API

```c
QkVm* vm = qkVmNew();
qkVmPrelude(vm, appQk);                       // the host's declarations, below
qkVmHost(vm, "moveBy", (void*)moveBy);        // the C function behind each extern fn
qkVmHost(vm, "position", (void*)position);
qkVmFuel(vm, 1000000);                        // at most a million loop steps per call
qkVmCapabilities(vm, "Item.* note print");    // what its programs may call (NULL: everything)
```

The **prelude** is Quark text that every program compiled by this machine sees. It is how a host
describes its API, in the language itself:

```quark
extern type Item                                    // a handle: the host's, never made by a script
type Point { x: Float, y: Float }                   // a record the host and its scripts share
extern fn position(i: Item) -> Point
extern effect fn moveBy(i: Item, dx: Float, dy: Float)    // effect: it changes the world
extern effect fn setColor(i: Item, r: Float, g: Float, b: Float, a: Float)
extern effect fn note(i: Item, message: Text)
extern fn flag(name: Text) -> Bool
```

Mark `effect` every host function that changes something (writes, sends, moves, draws). The host's
functions have no body for Quark to look into, so this is the one place effects are written: a script's
own functions are worked out from what they call. A function that calls `moveBy` has effects, a lambda
given to `map` may not call it, and a node editor shows the call as an action on the execution wire.
Functions that only read (`position`, `flag`) stay pure.

A prelude can also hold records, enums and ordinary functions that the host wants to share with
every script, such as `fn Point.plus(self, b: Point)` so that scripts can write `a + b`. A host function
can belong to a type too: `extern fn Item.position(self) -> Point` is called `item.position()`, and the
host registers it as `"Item.position"`. Its declarations are never printed or put in graphs, because they belong to the host,
but they are in the catalog (see below).

## Values cross as slots

Every value is a few 8-byte slots (`QkValue`: `.i`, `.f`, `.p`), in order:

| Quark | Slots | C, as an argument of a host function |
| --- | --- | --- |
| `Int`, `Bool`, an enum, an extern type | 1 | `int64_t` |
| `Float` | 1 | `double` |
| `Text` | 2 | `const char*` (the bytes, not 0-terminated), `int64_t` (the length) |
| `[T]` | 2 | `const void*` (the elements, each T's slots), `int64_t` (the count) |
| `[K: V]` | 1 | a pointer to the runtime's map (pass it back to Quark; do not read it) |
| `T?`, `T!` | 1 + T's, 3 + T's | the case (0: `None` / `Err`, 1: `Some` / `Ok`), then each case's data |
| a record | its fields' slots | one argument per slot (`Point { x: Float, y: Float }`: two `double`s) |

A host function returns nothing, one slot (`int64_t` or `double`; a `Bool` is an `int64_t` 0 or
1), or several slots as a struct returned by value. Quark follows the platform's convention for it:
on Windows x64 such a struct comes back through a hidden pointer, which Quark provides; on System V
(Linux, macOS) two slots come back in registers (`rax`, `rdx` / `xmm0`, `xmm1`) and more through
the hidden pointer. Either way the C side is the same:

```c
typedef struct Point { double x, y; } Point;
static Point position(int64_t i) { ... }                     // extern fn position(i: Item) -> Point
static void note(int64_t i, const char* s, int64_t n) { ... } // extern effect fn note(i: Item, message: Text)
```

There is no limit on the number of arguments: past the registers they go on the stack, as the
convention says.

## Compiling and calling

```c
QkModule* m = qkCompile(vm, "item.qk", text, len);
if (!m)
    for (int i = 0; i < qkDiagnosticCount(vm); i++)
        show(qkDiagnosticAt(vm, i)->message);         // "item.qk:12:5: unknown name ..."

int tick = qkFind(m, "tick");                          // fn tick(item: Item, mover: Mover, dt: Float) -> Mover
QkValue args[4] = {{.i = id}, state[0], state[1], {.f = dt}};
QkValue next[2];
if (!qkCall(m, tick, args, 4, next, 2))
    log(qkLastError(vm));                              // "item.qk:14: index 8 is outside the array ..."
```

- `qkFunctionSignature` gives the readable signature, and `qkSlotKinds` gives `'I'` / `'F'` per
  slot (parameters, then results). A host can check what it is about to call.
- A program needs no `main`: a host calls whatever functions it wants.
- Compiling takes well under a millisecond for a script. After each compile, the compiler's memory
  is freed; a module keeps only its machine code and its function table.

## When a script goes wrong

A **panic** stops the call, not the host: an index outside an array, a division by zero, `min` of
nothing, a failed `assert`, `panic("...")`, or the budget running out. `qkCall` returns false, `qkLastError` says
what happened and where, with the calls it went through when there were any (inlined ones
included), and the next call works normally:

```text
item.qk:31: index 8 is outside the array (its length is 8)
  in nearest
  called from tick at line 12
```

`exit(code)` in a script stops the call the same way; it never ends the host's process.

The way back is Quark's own. The trampoline that every call goes through saves the registers and the
stack, and a panic jumps back to it. No `longjmp` and no C++ exceptions cross the generated code, so
there is nothing for a C runtime to unwind.

The **budget** (`qkVmFuel`) counts loop steps and calls. A script with `while true` stops with "ran
too long", and the host goes on. A script that calls itself without end stops with "too many calls
deep": every function that calls others checks the stack on its way in, against the bounds of the
thread that called Quark, so the host's process never crashes on it.

## Capabilities

`qkVmCapabilities(vm, list)` says what the programs the machine compiles from then on may reach outside
themselves: host functions by name (`"moveBy note"`), by prefix (`"Item.*"`, `"net*"`), and the
built-in groups `print`, `files`, `env`, `args`, `exit`, `stdin`, `clock`. A program that calls
anything else does not compile, and the diagnostic says what it called:

```text
plugin.qk:6:5: setColor is not given to this program (the host allows: position flag print)
```

`NULL` (the default) allows everything. Use one machine per level of trust, or set the list before
each `qkCompile`: a downloaded plugin gets less than the application's own scripts. The check is at compile
time, so a program that compiled costs nothing more to run. `quark run --allow ...` does the same on
the command line.

## Memory

- Each machine (`QkVm`) has an arena of its own. Allocating is a pointer bump, and everything is
  freed at once when the machine's next outermost call starts. Arrays and texts a call returns stay
  valid until then, whatever other machines do.
- A loop inside a call gives back what its turns made as it goes (see
  [LANGUAGE.md](LANGUAGE.md#memory)), so a long call does not grow without end.
- **State that lives across calls lives in the host.** The natural shape is a record the event
  returns: `fn tick(item: Item, mover: Mover, dt: Float) -> Mover`. The host keeps `Mover`'s slots
  per item.

## Files a program uses

A program that says `use geometry` is given `geometry.qk`, found next to the name it was compiled
under (`qkCompile(vm, "scripts/item.qk", ...)` reads `scripts/geometry.qk`). `qkModuleReader` reads
it: the file system by default; a host that keeps its scripts in a pack or in memory sets its own
`char* reader(const char* path, size_t* len)`, which gives the text (from `malloc`) or NULL.

## Tasks

`qkTaskStart(m, fn, args, n)` makes a task of a function that gives nothing, and `qkTaskResume(t)` runs
it until it calls `yield()` or `wait(seconds)`, or ends: 1 it gave its turn back, 0 it is done, -1 it
stopped (`qkLastError`). Resume it whenever the host is ready for its next step (a request answered,
a timer, an event); `qkTaskEnd` frees it. Each task has its own stack, so a script yields from wherever
it is.

```c
QkTaskHandle* job = qkTaskStart(m, qkFind(m, "importAll"), NULL, 0);
/* whenever the host is ready for the next step: */
if (job && qkTaskResume(job) != 1) { qkTaskEnd(job); job = NULL; }
```

## Programs compiled ahead of time

iOS, some consoles and many embedded systems do not let a program make code executable while it runs. `quark object
script.qk -o script.o --symbol script` compiles ahead of time into an object file that the host links like
any C object: COFF on Windows, ELF on Linux, Mach-O on macOS and iOS, for the processor quark runs on, or for
another with `--target` (`arm64-ios`, `arm64-macos`, `x86_64-linux`...: every quark has all its code
generators, so a Windows machine makes an iOS app's scripts):

```c
extern QkStaticProgram script;
QkModule* m = qkModuleStatic(vm, &script); /* its host functions from qkVmHost, as for qkCompile */
```

The code needs no relocation at load time: every address it uses (the runtime, the host's functions,
the data) is read from a table in the object's data, which `qkModuleStatic` fills. Calls, panics,
capabilities and budgets work as for a compiled module; `parallelMap` runs on one thread, since the code
cannot be loaded again for the others. `tests/object/host.c` is a host that links one.

## Overflow and memory

`qkVmChecked(vm, true)` makes Int `+ - *` past their limits stop the call (a development build, say);
`qkVmMemoryLimit(vm, bytes)` stops a call whose program holds more memory than that.

## The optimizing tier

`qkVmRelease(vm, true)` compiles with the optimizing tier (as `quark build --release`): SSA form, range
analysis, loop motion, devirtualized interface calls and SIMD loops. It takes a millisecond or so where the
default takes a fraction of one, so it suits code compiled once and run long (a shipped level's scripts, a
plugin loaded at start) more than hot reload while editing. Results are the same either way.

## Hot reload

Compile the new text, then swap modules:

```c
QkModule* fresh = qkCompile(vm, "item.qk", edited, n);
if (fresh) { qkModuleFree(m); m = fresh; }   // else keep the old one and show the errors
```

Scripts keep no state between calls, so the new module goes on exactly where the old one stopped.

## Graphs and editors

| | |
| --- | --- |
| `qkTextToGraph(vm, text, n, layout)` | the program as nodes (JSON, see [LANGUAGE.md](LANGUAGE.md#the-node-graph)); `layout` (`{"key": [x, y]}`) adds `"pos"` to those nodes |
| `qkGraphToText(vm, json, n)` | the canonical text of a graph |
| `qkCompileGraph(vm, name, json, n)` | compiles a graph; each diagnostic's `key` names the node it is about (`tick/@3`) |
| `qkFormatText(vm, text, n)` | the canonical text |
| `qkCatalog(vm, text, n)` | every node an editor can offer: the built-ins with their pins, the operators, the statements, the prelude's externs and types, and the program's own functions and types |

Every node carries a **key**. A named node (a `let`, a `var`, a parameter, a loop variable) is keyed
`function/name`. Any other statement is keyed by its place (`function/@2`, `function/@2/@0` inside
a loop), and its inputs by input order (`tick/at.0`). Keys stay stable while the code around a
node keeps its shape, so an editor keeps node positions in a side file under them. Graphs never hold
positions themselves, so moving a node never changes the program.

## Threads

`parallelMap` runs a pure function on the processor's threads: each worker loads its own copy of the
module's code, with a context of its own (budget, panic path, stack bounds, arena), and its results go
into the caller's memory. A host function that a pure function calls may therefore be called from
several threads at once: keep the pure ones (the host functions not marked `effect`) thread-safe.

## Limits today

- **One call at a time per machine.** The state a call needs (budget, panic path, stack bounds,
  arena) is a context per thread; the host's own calls share the main one, so call Quark from one
  thread at a time. Calls may nest: a host function may call Quark again.
- **x86-64 and ARM64.** Windows x64, System V (Linux, macOS on Intel) and AAPCS64 (ARM64 Linux, macOS on
  Apple Silicon) calling conventions. On ARM64 a host function's record comes back as AAPCS64 says: in
  x0-x1 (16 bytes or less), in d0-d3 (all floats, four at most), or through x8; the C side is the same
  struct returned by value on every platform. The System V code is tested on Windows through adapters
  (`quark run --abi sysv`, `tests/sysv/hosts.c`, which on ARM64 tests the AAPCS64 calls), and ARM64 in an
  emulated Linux machine (`tools/arm64/vm.py`).
