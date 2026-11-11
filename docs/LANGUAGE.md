# The Quark language

Quark is a small language built for three things at once:

- **Elegant.** Few keywords, one way to write each thing, one canonical layout (`quark fmt`).
  Names are camelCase for values and functions and PascalCase for types and cases. The compiler
  enforces that, so every Quark program reads the same.
- **Fast.** Quark compiles to machine code itself, with no C compiler or LLVM. Loops written as
  pipelines (`map`, `filter`, `histogram`...) fuse into one machine loop. Lambdas and one-line
  functions are inlined. Records and vectors live in registers.
- **A node graph.** Values never change behind your back and effects happen in a visible order. That
  makes every program a graph of nodes and every graph a program. Text → graph → text gives the same
  text, comments included.

This page is the reference for the current version (MVP, language version 1).

## A taste

```quark
// The light of a pixel (Rec. 709).
fn luma(r: Int, g: Int, b: Int) -> Int = round(0.2126 * r + 0.7152 * g + 0.0722 * b)

type Frame { width: Int, height: Int, rgb: [Int] }
enum Mode { Waveform, Parade, Histogram }

fn lightLevels(f: Frame) -> [Int] =
    (0..f.width * f.height).histogram(256, p => luma(f.rgb[p * 3], f.rgb[p * 3 + 1], f.rgb[p * 3 + 2]))

fn main() {
    let frame = Frame(2, 1, [255, 0, 0, 0, 0, 255])
    let counts = lightLevels(frame)
    print(counts.filter(c => c > 0).len(), "levels used")
}
```

## Words

**Keywords (19):**

| | |
| --- | --- |
| Declarations | `fn` `type` `enum` |
| Bindings | `let` (never changes) · `var` (can change) |
| Choices | `if` `else` `match` |
| Loops | `for` `in` `while` `break` `continue` |
| Leaving | `return` |
| Logic | `and` `or` `not` |
| Values | `true` `false` |

**Kept for later:** `use` (modules), `pub` (exports), `extern` (functions from the host),
`effect` (declared effects) and `self`. Using them as names is an error today, so adding them later
breaks no program.

**Names:** `camelCase` for values, functions, fields and parameters, and `PascalCase` for types and
enum cases. Underscores are not allowed. The compiler suggests the right spelling:

```text
let my_value = 3     error: names are camelCase or PascalCase, without underscores: write "myValue"
```

**Literals:** `42`, `1_000_000` (underscores between digits), `3.14`, `1e-6`, `"text"` (escapes
`\n \t \" \\`), `true`, `false`, `[1, 2, 3]`.

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
| `Text` | text (literals; printing) | 2 |
| `Vec2` `Vec3` `Vec4` | vectors of Floats: `.x .y .z .w`, `+ -` between vectors, `* /` by a number | 2–4 |
| `[T]` | array of T | 2 |
| `type Name { ... }` | a record: named fields | sum of its fields |
| `enum Name { A, B }` | one of named cases | 1 |

Types are **inferred**. You write them only on function parameters and record fields; results and
`let`s may carry one (`let xs: [Int] = ...`, `fn f(...) -> Float`).

**Conversions.** An `Int` becomes a `Float` wherever a Float is wanted (`0.5 * count`). Going the
other way is explicit: `toInt` truncates, and `floor`, `ceil` and `round` return an `Int`. A loop
(`map`, `filter`, a range) becomes an array wherever an array is wanted.

**Records** are built with the type's name, by position or by field name (all named or none):

```quark
type Point { x: Float, y: Float }
let a = Point(1, 2)
let b = Point(x: 1, y: 2)
```

**Enums** are values `Mode.Parade`, matched by case name.

## Declarations

```quark
fn name(param: Type, ...) -> Result { statements; last expression is the result }
fn name(param: Type, ...) -> Result = expression        // one expression: inlined where it is called
type Name { field: Type, ... }
enum Name { CaseA, CaseB }
```

`-> Result` may be left out when the body says it, except when the function calls itself or returns
early. A program starts at `fn main()`.

## Statements

| | |
| --- | --- |
| `let x = value` | a name for a value; it never changes |
| `var x = value` | a value that can change: `x = ...`, `x += ...` (`-= *= /=`), `xs[i] = ...`, `p.x = ...` |
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
| `or` | |
| `and` | |
| `not` | |
| `== != < <= > >=` | they do not chain: `a < b < c` is an error |
| `a..b` | a range of Ints |
| `+ -` | |
| `* / %` | |
| `-x` | |
| `f(x)`, `a.f(x)`, `a.field`, `a[i]` | |

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

  A `match` on an enum names every case or ends with `_`. A `match` on an Int ends with `_`.
- **Lambdas** `x => x * 2` and `(acc, x) => acc + x` go only into `map`, `filter`, `fold`,
  `histogram`, `any` and `all`, and are always inlined. A function's name works there too:
  `xs.map(luma)`. Functions are not values. This keeps every call visible as a node and costs nothing
  at run time.
- **Blocks** `{ ...; value }` give their last expression.

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

## Values, not references

Every value behaves as a copy. Storing an array that came from a `var` (or a parameter) copies it, so
changing the `var` later never changes what was stored:

```quark
var a = [1, 2, 3]
let b = a
a[0] = 100      // b is still [1, 2, 3]
```

Only what is needed is copied. A `let` of a `let` shares the array, and arrays passed to functions
are never copied (functions cannot change their parameters).

**Memory** comes from an arena: allocating bumps a pointer, and everything is freed at once when the
program (or, in a host, a call) ends. There is no garbage collector and no pause.

## Built-in functions

| | |
| --- | --- |
| Math | `sqrt sin cos tan asin acos atan atan2(y, x) exp log pow(x, y)`, `abs`, `floor ceil round` (→ Int), `toFloat toInt`, `min(a, b) max(a, b) clamp(v, lo, hi) lerp(a, b, t)` |
| Vectors | `Vec2(x, y) Vec3(...) Vec4(...)`, `length(v) dot(a, b)` |
| Loops | `map(f)` (f may take the index too: `(x, i) => ...`), `filter(f)`, `fold(start, (acc, x) => ...)`, `sum()`, `min()`, `max()`, `any(f)`, `all(f)`, `histogram(bins, f)`, `len()`, `toArray()` |
| Arrays | `fill(n, value)`, `[a, b, c]`, `xs[i]` (checked) |
| Out | `print(a, b, ...)`, `now()` (seconds, for timing) |

## When something goes wrong

- **Before running:** every error has a place, a caret under it and, when a name is mistyped, the
  closest name (`unknown name "totl": did you mean "total"?`).
- **While running:** an index outside an array, a division by zero, a histogram bin out of range or
  `min` of nothing stops the program with the line (`index 5 is outside the array (its length is
  3)`).
- **A budget:** `quark run --fuel N` stops a program after N loop steps. That is how a host keeps a
  plugin from hanging it.

## The node graph

`quark graph file.qk` writes the program as nodes (JSON). `quark text file.json` writes the program
back.

| Text | Graph |
| --- | --- |
| `let x = ...`, `var x = ...`, a parameter, a `for` variable | a named node; every use of the name is a wire from it |
| `f(a, b)`, `a + b`, `a.field`, `xs[i]`, `[a, b]` | a node with input pins |
| `12`, `"text"`, `true`, `Mode.Parade`, a function given to `map` | a value typed into a pin, not a node |
| `x => ...` | a node holding a small graph, with its parameters as named nodes |
| the statements of a block | the order of the block's list: the execution wire |
| `if`, `match`, `for`, `while` | nodes holding the blocks they run |
| comments and blank lines | notes on the node they belong to |

Named nodes carry a `key` (`function/name`). An editor keeps node positions in a side file under
that key, so moving nodes never touches the program. Graph → text always gives the canonical text,
so a graph edited in a node editor and a program edited as text can live in one repository.

## Not yet

These are planned and their words are reserved:

- `extern` / host functions and an embedding API in C. The core is ready for it: values are flat slots
  and the backend already calls C functions (the runtime).
- `use` / modules, `pub`.
- `effect`, declared effects: capabilities such as `doc` or `g` passed in by the host. This is how a
  plugin is kept from touching what it was not given.
- Text operations, generic functions, closures as values.
- Small arrays (`[Byte]`, `[Float32]`), SIMD, threads for `map` / `histogram`.
- Backends for System V (Linux, macOS) and ARM64. Today the code generator targets x86-64 Windows.
