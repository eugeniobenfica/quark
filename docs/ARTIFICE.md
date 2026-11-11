# Quark in Artifice's node system

How Artifice's nodes, events and scripts map onto Quark, and what the engine does to switch from
Lua. Everything Quark needs for this is in place: `extern fn` / `extern type`, the embedding API,
graphs with stable keys, errors that point at nodes, and the catalog. The work left is on Artifice's
side, listed at the end.

## The shape of it

| Artifice today | With Quark |
| --- | --- |
| A Lua script per object (`start(self)`, `update(self, dt)`, `hit(self, other)`, `click(self)`) | A Quark program with the same functions: the engine calls them through `qkCall` |
| A node graph compiled to Lua by `nodeCompiler.c` (one way) | The graph **is** a Quark program: `qkCompileGraph` compiles it, `qkGraphToText` shows its text, `qkTextToGraph` turns text back into nodes |
| The *Lua Code* node (opaque text) | Any Quark text: a function in the program, which the graph shows as nodes too |
| `self.x`, `self:find(name)`... (a table with methods) | `extern type Object` and `extern fn`s in the engine's prelude (`position(self)`, `find("Player")`) |
| `vars` (a table per object) | A record the events return: `update(self, ship, dt) -> Ship`, kept by the engine per object |
| `luaRuntime.c` | `libquark.a`: `qkVmNew`, `qkVmPrelude`, `qkVmHost`, `qkCompile`, `qkCall` |

## Events

| Artifice node | Quark function | The engine calls |
| --- | --- | --- |
| On Start | `fn start(self: Object) -> State` | once; keeps the State it returns |
| On Update (gives `dt`) | `fn update(self: Object, state: State, dt: Float) -> State` | every frame, with the last State |
| On Hit (`with`) | `fn hit(self: Object, other: Object)` | when hitboxes start to touch |
| On Click | `fn click(self: Object)` | when its button is clicked |
| On Key Pressed / Released | `fn keyPressed(self: Object, key: Text)` ... | on the key |

`State` is any record the program declares, such as `type Ship { speed: Float, laps: Int }`. A
program without variables leaves it out (`fn update(self: Object, dt: Float)`). The engine reads
the signatures from `qkFunctionSignature` and `qkSlotKinds`, so it knows which form it got.

## Nodes

| Artifice node | Quark |
| --- | --- |
| Number, Text (values typed in) | a pin value: `{"value": "3", "of": "Int"}` |
| Add, Subtract, Multiply, Divide, Greater, Less, Equal, And, Or, Not | operator nodes (`"op": "+"`), the catalog's `"operators"` |
| Sine, Random... | built-in calls (`sin`); Random would be a host function (`extern fn random() -> Float`) |
| Branch | `if` (a node holding its two blocks) |
| Sequence | the order of a block's statements: the execution wire |
| Set Variable / Get Variable | a field of the State record (`ship.speed`), or a `var` for one event's own use |
| Move By, Set Position, Rotate By, Set Visible, Set Color, Camera Follow, Print, Go To Scene, Quit Game, Burst Particles, Set Emitting | `extern fn`s in the engine's prelude: one C function each |
| Get Position, Get Rotation, Is Key Down, Input Axis, Delta Time, Time, Is Touching | `extern fn`s that return a value (`position(self) -> Vec2`), or a parameter (`dt`) |
| Move By 3D... (the 3D category) | the same, with `Vec3` |

A node library becomes a list of `extern fn` lines plus one C function each. A new node is one line
of prelude and one function in the engine, the same size as a new entry in `nodeLibrary.c` today.

## What the node editor uses

- **The palette:** `qkCatalog(vm, program, n)` lists the built-ins (by category, with their pins as
  `"name: Type"`), the operators, the statements, the events, the engine's externs and types, and
  the program's own functions and types. Wires get their colors from the types (`"type"` on every
  node).
- **Opening a program:** `qkTextToGraph(vm, text, n, layout)`. The layout file
  (`{"update/speed": [120, 40], ...}`) puts nodes where the user left them. A node without a
  position gets an automatic one from the editor.
- **Saving:** keep the program as text (`.qk`, canonical through `qkGraphToText`) and the
  positions as JSON beside it. Diffs stay readable, and text and nodes are one program.
- **Errors:** `qkCompileGraph` returns diagnostics with `key`, so the editor marks those nodes in
  red. A failure while running (`qkLastError`) gives a line of the canonical text, and every node in
  the graph carries its `"line"`, so the editor finds the node from there.
- **Play:** compile, call `start` and then `update` every frame. On every edit, compile again and
  swap the module (hot reload, no restart).

## What is left on Artifice's side

1. **Write the engine's prelude.** One `extern fn` per node in `nodeLibrary.c`, plus its C function.
   The C functions mostly exist already behind the Lua bindings.
2. **Swap `luaRuntime.c`** for calls to `libquark.a`: one `QkVm` for the game, and one module per
   script or graph.
3. **Point the node editor (`editor/ui/nodes.c`) at Quark's graph JSON** instead of
   `nodeCompiler.c`'s own format. Positions move to the layout file.
4. **Keep Lua while Quark settles:** an object can have either a `.lua` or a `.qk`, and both
   runtimes live side by side.
5. **Exports:** Windows x64 works today. Linux, macOS (System V) and Android (ARM64) wait for
   Quark's next backends. Until then, the player on those platforms keeps Lua.
