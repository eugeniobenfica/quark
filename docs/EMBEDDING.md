# Embedding Quark

A C program (an engine, an editor, a video app) links `build/libquark.a` and includes
[`include/quark.h`](../include/quark.h). With that it can compile Quark, call it, give it host
functions, and turn programs into node graphs and back. Nothing else is needed: no C compiler at
run time and no other library. The complete working example is
[`examples/host/host.c`](../examples/host/host.c), a small engine in the style of Artifice. Its
output is checked by the tests.

## The machine and the host's API

```c
QkVm* vm = qkVmNew();
qkVmPrelude(vm, engineQk);                    // the host's declarations, below
qkVmHost(vm, "moveBy", (void*)moveBy);        // the C function behind each extern fn
qkVmHost(vm, "position", (void*)position);
qkVmFuel(vm, 1000000);                        // at most a million loop steps per call
```

The **prelude** is Quark text that every program compiled by this machine sees. It is how a host
describes its API, in the language itself:

```quark
extern type Object                                  // a handle: the host's, never made by a script
extern fn position(o: Object) -> Vec2
extern fn moveBy(o: Object, dx: Float, dy: Float)
extern fn setColor(o: Object, r: Float, g: Float, b: Float, a: Float)
extern fn say(o: Object, message: Text)
extern fn keyDown(key: Text) -> Bool
```

A prelude can also hold records, enums and ordinary functions that the host wants to share with
every script. Its declarations are never printed or put in graphs, because they belong to the host,
but they are in the catalog (see below).

## Values cross as slots

Every value is a few 8-byte slots (`QkValue`: `.i`, `.f`, `.p`), in order:

| Quark | Slots | C, as an argument of a host function |
| --- | --- | --- |
| `Int`, `Bool`, an enum, an extern type | 1 | `int64_t` |
| `Float` | 1 | `double` |
| `Vec2`, `Vec3`, `Vec4` | 2, 3, 4 | one `double` each |
| `Text` | 2 | `const char*` (the bytes, not 0-terminated), `int64_t` (the length) |
| `[T]` | 2 | `const void*` (the elements, each T's slots), `int64_t` (the count) |
| a record | its fields' slots | one argument per slot |

A host function returns nothing, one slot (`int64_t` or `double`; a `Bool` is an `int64_t` 0 or
1), or several slots as a struct returned by value. Windows x64 passes such a struct through a
hidden pointer, which Quark provides:

```c
typedef struct V2 { double x, y; } V2;
static V2 position(int64_t o) { ... }                       // extern fn position(o: Object) -> Vec2
static void say(int64_t o, const char* s, int64_t n) { ... } // extern fn say(o: Object, message: Text)
```

There is no limit on the number of arguments: from the fifth on they go on the stack, as the
convention says.

## Compiling and calling

```c
QkModule* m = qkCompile(vm, "ship.qk", text, len);
if (!m)
    for (int i = 0; i < qkDiagnosticCount(vm); i++)
        show(qkDiagnosticAt(vm, i)->message);         // "ship.qk:12:5: unknown name ..."

int update = qkFind(m, "update");                      // fn update(self: Object, ship: Ship, dt: Float) -> Ship
QkValue args[4] = {{.i = id}, state[0], state[1], {.f = dt}};
QkValue next[2];
if (!qkCall(m, update, args, 4, next, 2))
    log(qkLastError(vm));                              // "ship.qk:14: index 8 is outside the array ..."
```

- `qkFunctionSignature` gives the readable signature, and `qkSlotKinds` gives `'I'` / `'F'` per
  slot (parameters, then results). A host can check what it is about to call.
- A program needs no `main`: a host calls whatever functions it wants.
- Compiling takes well under a millisecond for a script. After each compile, the compiler's memory
  is freed; a module keeps only its machine code and its function table.

## When a script goes wrong

A **panic** stops the call, not the host: an index outside an array, a division by zero, `min` of
nothing, or the budget running out. `qkCall` returns false, `qkLastError` says what happened and
where, and the next call works normally.

The way back is Quark's own. The trampoline that every call goes through saves the registers and the
stack, and a panic jumps back to it. No `longjmp` and no C++ exceptions cross the generated code, so
there is nothing for a C runtime to unwind.

The **budget** (`qkVmFuel`) counts loop steps and calls. A script with `while true` stops with "ran
too long", and the frame goes on.

## Memory

- Each call gets an arena. Allocating is a pointer bump, and everything is freed at once when the
  next outermost call starts. Arrays a call returns stay valid until then.
- **State that lives across frames lives in the host.** The natural shape is a record the event
  returns: `fn update(self: Object, ship: Ship, dt: Float) -> Ship`. The host keeps `Ship`'s slots
  per object.

## Hot reload

Compile the new text, then swap modules:

```c
QkModule* fresh = qkCompile(vm, "ship.qk", edited, n);
if (fresh) { qkModuleFree(m); m = fresh; }   // else keep the old one and show the errors
```

Scripts keep no state between calls, so the new module goes on exactly where the old one stopped.

## Graphs and editors

| | |
| --- | --- |
| `qkTextToGraph(vm, text, n, layout)` | the program as nodes (JSON, see [LANGUAGE.md](LANGUAGE.md#the-node-graph)); `layout` (`{"key": [x, y]}`) adds `"pos"` to those nodes |
| `qkGraphToText(vm, json, n)` | the canonical text of a graph |
| `qkCompileGraph(vm, name, json, n)` | compiles a graph; each diagnostic's `key` names the node it is about (`update/@3`) |
| `qkFormatText(vm, text, n)` | the canonical text |
| `qkCatalog(vm, text, n)` | every node an editor can offer: the built-ins with their pins, the operators, the statements, the events, the prelude's externs and types, and the program's own functions and types |

Every node carries a **key**. A named node (a `let`, a `var`, a parameter, a loop variable) is keyed
`function/name`. Any other statement is keyed by its place (`function/@2`, `function/@2/@0` inside
a loop), and its inputs by input order (`update/at.0`). Keys stay stable while the code around a
node keeps its shape, so an editor keeps node positions in a side file under them. Graphs never hold
positions themselves, so moving a node never changes the program.

## Limits today

- **One call at a time per process.** The arena, the budget and the panic path are shared. Calls may
  nest: a host function may call Quark again.
- **Windows x64 only.** System V (Linux, macOS) and ARM64 backends are next. The IR and this API stay
  as they are; only the code generator's calling convention and encoder change.
- **No text operations** beyond literals, passing text to the host, and printing. Programs cannot
  build or compare text yet.
