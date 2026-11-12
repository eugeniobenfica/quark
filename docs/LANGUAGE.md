# The Quark language

Quark is a small, general-purpose language built for three things at once:

- **Elegant.** Few keywords, one way to write each thing, one canonical layout (`quark fmt`).
  Names are camelCase for values and functions and PascalCase for types and cases. The compiler
  enforces that, so every Quark program reads the same.
- **Fast.** Quark compiles to machine code itself, with no C compiler and no LLVM. Loops written as
  pipelines (`map`, `filter`, `histogram`...) fuse into one machine loop. Small functions and
  lambdas are inlined. Records and vectors live in registers.
- **A node graph.** Values never change behind your back, and the code that changes the world is
  marked `effect`. That makes every program a graph of nodes, with data wires and an execution wire,
  and every graph a program. Text → graph → text gives back the same text, comments included.

It was made for video and animation tools and game engines that embed it, and it is a whole language
on its own: text, optionals, enums that carry data, records, arrays that grow, constants, named and
default arguments. This page is the reference for language version 1.

## A taste

```quark
// Shapes, their areas, and a report.
let unit = 10.0

enum Shape { Circle(radius: Float), Box(width: Float, height: Float), Empty }
type Item { name: Text, shape: Shape, count: Int }

fn area(s: Shape) -> Float = match s {
    Circle(r) => 3.14159 * r * r
    Box(w, h) => w * h
    Empty => 0
}

fn biggest(items: [Item]) -> Item? {
    var best: Item? = None
    var most = -1.0
    for item in items {
        let size = area(item.shape)
        if size > most {
            best = item
            most = size
        }
    }
    best
}

effect fn report(items: [Item], title: Text = "Stock") {
    print("{title}: {len(items)} items, {items.map(i => area(i.shape) * i.count).sum()} square units")
    match biggest(items) {
        Some(item) => print("biggest: {item.name}")
        None => print("nothing")
    }
}

fn main() {
    var items: [Item] = []
    items.push(Item("plate", Shape.Circle(radius: unit), count: 4))
    items.push(Item("tile", Shape.Box(2, 2), count: 50))
    let more = items[0] with { name: "big plate", count: 1 }
    items.push(more)
    report(items)
    report(items, title: "Today")
}
```

## Words

**Keywords (22):**

| | |
| --- | --- |
| Declarations | `fn` `type` `enum` `let` (at the top: a constant) `extern` (the host's) `effect` (changes the world) |
| Bindings | `let` (never changes) · `var` (can change) |
| Choices | `if` `else` `match` |
| Loops | `for` `in` `while` `break` `continue` |
| Leaving | `return` |
| Logic | `and` `or` `not` |
| Values | `true` `false` |
| Copies | `with` |

`Some` and `None` are the built-in optional's cases, not keywords.

**Kept for later:** `use` (modules) and `pub` (exports). Using them as names is an error today, so
adding them later breaks no program.

**Names:** `camelCase` for values, functions, fields and parameters, and `PascalCase` for types and
enum cases. Underscores are not allowed. The compiler suggests the right spelling:

```text
let my_value = 3     error: names are camelCase or PascalCase, without underscores: write "myValue"
```

**Literals:**
- numbers: `42`, `1_000_000` (underscores between digits), `3.14`, `1e-6`;
- text: `"text"`, with `{expressions}` inside (below) and the escapes `\n \t \" \\ \{ \}`;
- `true`, `false`;
- arrays: `[1, 2, 3]` and `[]` (the empty one takes the type it goes into).

**Comments:** `//` to the end of the line. A comment above a statement or declaration belongs to it,
and so does one at the end of its line. Both survive formatting and the trip through a graph.

**Lines:** a line break ends a statement. It does not end one inside `( )` or `[ ]`, after an
operator or a comma, or before a line that starts with `.name` (a chained call) or with `else`:

```quark
let total = (0..n)
    .map(x => x * x)
    .filter(x => x % 2 == 0)
    .sum()
```

## Types

| Type | What | Slots |
| --- | --- | --- |
| `Int` | 64-bit integer; `+ - *` wrap around; `/` and `%` round down (floored) and stop on division by zero | 1 |
| `Float` | 64-bit float | 1 |
| `Bool` | `true` / `false` | 1 |
| `Text` | text (UTF-8 bytes): joined with `+`, compared with `== < ...`, `len(t)` its bytes | 2 |
| `Vec2` `Vec3` `Vec4` | vectors of Floats: `.x .y .z .w`, `+ -` between vectors, `* /` by a number | 2–4 |
| `[T]` | array of T | 2 |
| `T?` | a T, or nothing: `Some(x)` / `None` | 1 + T's |
| `type Name { ... }` | a record: named fields | sum of its fields |
| `enum Name { A, B(x: T) }` | one of named cases; a case may carry data | 1 + every case's data |
| `extern type Name` | a handle only the host makes (an object, a layer) | 1 |

Types are **inferred**. You write them only on function parameters and record fields. A result, a
`let` or a constant may carry one: `let xs: [Int] = []`, `fn f(...) -> Float`.

**Conversions** that happen by themselves:
- an `Int` becomes a `Float` wherever a Float is wanted (`0.5 * count`);
- a T becomes a `T?` wherever an optional is wanted (`return i` in a function giving `Int?`);
- a loop (`map`, `filter`, a range) becomes an array wherever an array is wanted.

The other way is written: `toInt` truncates, `floor`, `ceil` and `round` return an `Int`, and `toText`
makes text.

**Records** are built with the type's name. The fields go by position, then by name:

```quark
type Point { x: Float, y: Float }
let a = Point(1, 2)
let b = Point(x: 1, y: 2)
let c = a with { y: 5 }          // a copy with some fields changed (vectors too: v with { x: 0 })
```

**Enums** are values `Mode.Parade`. A case can carry data, `Shape.Circle(radius: 2)`, which a
`match` takes apart:

```quark
fn area(s: Shape) -> Float = match s {
    Circle(r) => 3.14 * r * r
    Box(w, h) => w * h
    Empty => 0
}
```

Cases without data compare with `==`. Cases with data are compared with `match`.

**Optionals.** `T?` is a T or nothing. `Some(x)` and `None` make one, and `match` with `Some(x)` /
`None` takes one apart. `a ?? b` gives what `a` holds, or `b`, which is only worked out when needed:

```quark
fn find(names: [Text], wanted: Text) -> Int? {
    for name, i in names {
        if name == wanted {
            return i
        }
    }
    None
}

let at = find(names, "bia") ?? -1
```

**Text.**
- Join with `+`, compare with `== != < <= > >=` (byte by byte), and get its bytes with `len(t)`.
- Values go inside with braces: `"lap {ship.laps} at {ship.speed}"` takes Ints, Floats, Bools,
  texts and enum cases.
- `toText(x)` does the same for one value.

## Declarations

```quark
fn name(param: Type, other: Type = default) -> Result { statements; the last one is the result }
fn name(param: Type) -> Result = expression          // one expression
effect fn name(...) { ... }                          // changes the world (below)
let name = value                                     // a constant (at the top of a file)
type Name { field: Type, ... }
enum Name { CaseA, CaseB(data: Type) }
extern fn name(param: Type) -> Result                // the host's: a C function, no body
extern effect fn name(...)                           // the host's, and it changes the world
extern type Name                                     // the host's handle (an object, a layer...)
```

- `-> Result` may be left out when the body says it, except when the function calls itself or
  returns early.
- **Arguments** go by position, then by name: `report(items, title: "Today")`. A parameter with a
  default (`title: Text = "Stock"`) may be left out. Defaults and constants are pure values, worked
  out where they are used.
- Small functions are inlined where they are called; that is free, and shows nowhere in the program.
- A program run from the command line starts at `fn main()`. A program for a host (an engine, an
  app) has the functions the host calls, such as `start` and `update`. `fn main` is not needed there.

### Effects

A function that changes the world is an `effect fn`. That covers drawing, moving an object, printing,
reading the clock, and calling a host function marked `effect`. Everything else is **pure**: given the
same values, it gives the same result and changes nothing. The compiler keeps the line between the
two:

- an effect happens only inside an `effect fn` (or `main`);
- a lambda (in `map`, `filter`...) never has effects, so loops can be fused, reordered and, later, run
  in parallel;
- a constant or a default never has effects.

In a node graph, a pure call is a data node, and an effect call is an action node on the execution
wire. Every node says which it is (`"effect": true`), so an editor draws it right.

```quark
extern type Object
extern fn position(o: Object) -> Vec2                   // reads: a data node
extern effect fn moveBy(o: Object, dx: Float, dy: Float) // acts: an action node

effect fn update(self: Object, dt: Float) {
    if position(self).x < 300 {
        moveBy(self, 120 * dt, 0)
    }
}
```

**Host declarations.** `extern fn` is a function the host provides, and `extern type` is a value
only the host makes. A script can pass it on and compare it, but never build it. A host usually gives
these as its **prelude**, which every program it compiles sees. See [EMBEDDING.md](EMBEDDING.md).

## Statements

| | |
| --- | --- |
| `let x = value` | a name for a value; it never changes |
| `var x = value` | a value that can change: `x = ...`, `x += ...` (`-= *= /=`), `xs[i] = ...`, `p.x = ...`, `xs.push(v)` |
| `for x in source { }` | over an array, a range `a..b` (b not included) or a loop; `for x, i in ...` gives the index too |
| `while condition { }` | |
| `break`, `continue` | |
| `return value` | |
| an expression | a call, an `if` or a `match` whose value is not kept |

A value that is computed and then dropped (`x + 1` alone on a line) is an error.

## Expressions

Precedence, from loosest to tightest:

| | |
| --- | --- |
| `a ?? b` | the optional's value, or b |
| `or` | |
| `and` | |
| `not` | |
| `== != < <= > >=` | they do not chain: `a < b < c` is an error |
| `a..b` | a range of Ints |
| `+ -` | |
| `* / %` | |
| `-x` | |
| `f(x)`, `a.f(x)`, `a.field`, `a[i]`, `a with { ... }` | |

- **`a.f(b)` is `f(a, b)`.** Any function reads left to right this way: `xs.map(f).sum()`.
- **`if`** and **`match`** are expressions:

  ```quark
  let sign = if x < 0 { -1 } else { 1 }
  let name = match mode {
      Waveform => "waveform"
      Parade => "parade"
      _ => "other"
  }
  ```

  A `match` on an enum or an optional names every case or ends with `_`. A `match` on an Int ends
  with `_`.
- **Lambdas** `x => x * 2` and `(acc, x) => acc + x` go only into `map`, `filter`, `fold`,
  `histogram`, `any` and `all`, and are always inlined. A function's name works there too:
  `xs.map(luma)`. Functions are not values. This keeps every call visible as a node and costs nothing
  at run time.
- **Blocks** `{ ... }` give their last expression.

## Loops that fuse

`map` and `filter` build nothing: they add code to the loop of their source. The loop ends in
whatever consumes it: `sum`, `fold`, `histogram`, `min`/`max`, `any`/`all`, `len`, `for`, or an array
when one is needed.

```quark
let evens = (0..1_000_000).filter(x => x % 2 == 0)   // used once below: never stored
let total = evens.map(x => x * x).sum()             // one machine loop, no array
```

A `let` holding a loop that is used **once**, at the same loop depth, is fused into that use. Used
twice, it is computed once into an array. Either way the result is the same, because values never
change.

Indexes are checked. The check is dropped where it cannot fail: `for i in 0..len(xs) { xs[i] }`, with
`xs` a `let` or a parameter.

## Values, not references

Every value behaves as a copy. Storing an array that came from a `var` (or a parameter) copies it, so
changing the `var` later never changes what was stored:

```quark
var a = [1, 2, 3]
let b = a
a[0] = 100      // b is still [1, 2, 3]
```

Only what is needed is copied. A `let` of a `let` shares the array, and arrays passed to functions
are never copied (functions cannot change their parameters). `xs.push(v)` grows a `var` array in
place, with room reserved, so building a list one value at a time is linear.

**Memory** comes from an arena: allocating bumps a pointer, and everything is freed at once when the
program (or, in a host, a call) ends. There is no garbage collector and no pause.

## Built-in functions

| | |
| --- | --- |
| Math | `sqrt sin cos tan asin acos atan atan2(y, x) exp log pow(x, y)`, `abs`, `floor ceil round` (→ Int), `toFloat toInt`, `min(a, b) max(a, b) clamp(v, lo, hi) lerp(a, b, t)` |
| Vectors | `Vec2(x, y) Vec3(...) Vec4(...)`, `length(v) dot(a, b)` |
| Loops | `map(f)` (f may take the index too: `(x, i) => ...`), `filter(f)`, `fold(start, (acc, x) => ...)`, `sum()`, `min()`, `max()`, `any(f)`, `all(f)`, `histogram(bins, f)`, `len()`, `toArray()` |
| Arrays | `fill(n, value)`, `[a, b, c]`, `[]`, `xs[i]` (checked), `xs.push(v)` (a `var`) |
| Text | `"... {x} ..."`, `+`, `== <` ..., `len(t)`, `toText(x)` |
| Optionals | `Some(x)`, `None`, `a ?? b`, `match` |
| Effects | `print(a, b, ...)`, `now()` (seconds, for timing) |

## When something goes wrong

- **Before running:** every error has a place and a caret under it. When a name is mistyped, it gives
  the closest one (`unknown name "totl": did you mean "total"?`, `f has no parameter "size"`). The
  rules say what to do: `print has effects (it changes the world): write effect fn greet`.
- **While running:** an index outside an array, a division by zero, a histogram bin out of range or
  `min` of nothing stops the program with the line (`index 5 is outside the array (its length is
  3)`). In a host, it stops only that call.
- **A budget:** `quark run --fuel N` stops a program after N loop steps. That is how a host keeps a
  plugin from hanging it.

## The node graph

`quark graph file.qk` writes the program as nodes (JSON). `quark text file.json` writes the program
back.

| Text | Graph |
| --- | --- |
| `let x = ...`, `var x = ...`, a parameter, a `for` variable, a name in a match arm | a named node; every use of the name is a wire from it |
| `f(a, b)`, `a + b`, `a.field`, `xs[i]`, `[a, b]`, `a with { ... }`, `"... {x} ..."` | a node with input pins |
| `12`, `"text"`, `true`, `Mode.Parade`, a constant, `None`, a function given to `map` | a value typed into a pin, not a node |
| `x => ...` | a node holding a small graph, with its parameters as named nodes |
| the statements of a block | the order of the block's list: the execution wire |
| a call to an `effect fn` | an action node (`"effect": true`) |
| `if`, `match`, `for`, `while` | nodes holding the blocks they run |
| a constant, a default value | its own small graph |
| comments and blank lines | notes on the node they belong to |

Every node carries a `key`, a `line` in the canonical text, and its `type`:
- a named node is keyed `function/name`;
- another statement is keyed by its place (`update/@3`, and `update/@3/@0` inside it);
- an input is keyed by its order (`update/at.0`).

An editor keeps node positions in a side file under those keys (`quark graph --layout pos.json` puts
them back in as `"pos"`), so moving nodes never touches the program. Graph → text always gives the
canonical text, so a graph edited in a node editor and a program edited as text can live in one
repository. `quark catalog` lists every node an editor can offer.

## Not yet

- `use` / modules and `pub`, both reserved.
- Generic functions (`fn first<T>(xs: [T]) -> T`) and functions as parameters, both by
  specialization so they stay free.
- Maps (dictionaries), slices of text, formatting numbers (`"{x:.2}"`).
- Small arrays (`[Byte]`, `[Float32]`), SIMD, threads for `map` / `histogram`.
- Backends for System V (Linux, macOS) and ARM64. Today the code generator targets x86-64 Windows.
