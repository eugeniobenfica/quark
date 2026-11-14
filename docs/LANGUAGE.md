# The Quark language

Quark is a small, general-purpose language built for three things at once:

- **Elegant.** Few keywords, one way to write each thing, one canonical layout (`quark fmt`).
  Names are camelCase for values and functions and PascalCase for types and cases. The compiler
  enforces that, so every Quark program reads the same.
- **Fast.** Quark compiles to machine code itself, with no C compiler and no LLVM. Loops written as
  pipelines (`map`, `filter`, `fold`...) fuse into one machine loop. Small functions and
  lambdas are inlined. Records live in registers. Generic functions and functions that take
  functions are specialized, so they cost what hand-written code would.
- **A node graph.** Values never change behind your back, and the compiler knows which code changes
  the world: it works it out, nothing is written for it. That makes every program a graph of nodes,
  with data wires and an execution wire, and every graph a program. Text → graph → text gives back the
  same text, comments included.

Quark programs run from the command line or as standalone executables, and any C program (an
application, an editor, a server, a tool) can embed Quark as its scripting language. It is a whole
language: text, optionals and results, enums that carry data, records with their own functions and
operators, interfaces, arrays that grow, maps, slices, generic functions, functions that take
functions, patterns with ranges and guards, constants, named and default arguments, tests, files,
standard input and the command line. Nothing in it is about one kind of program. This page is the
reference for language version 1.

## A taste

```quark
// Shapes, their areas, and a report.
const unit = 10.0

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

fn report(items: [Item], title: Text = "Stock") {
    print("{title}: {len(items)} items, {items.map(i => area(i.shape) * i.count).sum():.1} square units")
    let Some(item) = biggest(items) else {
        print("nothing")
        return
    }
    print("biggest: {item.name}")
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

And a program that reads a file, counts its words and shows the most used ones
([examples/words.qk](../examples/words.qk) is a fuller one):

```quark
fn main() {
    let text = readFile("notes.txt")? // stops with "cannot read notes.txt: ..." if it cannot
    let counts = words(lower(text)).countBy(w => w)
    for e, i in counts.sortBy(e => e.value, descending: true).take(5) {
        print("{i + 1:2}. {e.key} {e.value}")
    }
}
```

## Words

**Keywords (26):**

| | |
| --- | --- |
| Declarations | `fn` `type` `enum` `const` (a constant, at the top) `extern` (the host's) `effect` (what has no body to infer it from changes the world: `extern effect fn`, `effect (A) -> R`) |
| Bindings | `let` (never changes) · `var` (can change), inside functions |
| Choices | `if` `else` `match` |
| Loops | `for` `in` `while` `break` `continue` |
| Leaving | `return` · `defer` (runs when its block is left) |
| Logic | `and` `or` `not` |
| Values | `true` `false` |
| Copies | `with` |
| Files | `use` (another file's declarations) `pub` (what a file gives the files that use it) |

`Some` and `None` are the built-in optional's cases, and `Ok` and `Err` the built-in result's; they
are not keywords. `test` starts a test only at the top of a file, before a text (`test "..." { }`),
so it stays free as a name, and `self` is the first value of a type's function (`fn Vec2.length(self)`).

**Names:** `camelCase` for values, functions, fields and parameters, and `PascalCase` for types,
enum cases and type parameters. Underscores are not allowed. The compiler suggests the right
spelling:

```text
let my_value = 3     error: names are camelCase or PascalCase, without underscores: write "myValue"
```

**Literals:**
- numbers: `42`, `1_000_000` (underscores between digits), `3.14`, `1e-6`;
- text: `"text"`, with `{expressions}` inside (below) and the escapes `\n \t \r \" \\ \{ \}`; and on lines of its
  own between `"""` and `"""`: the closing quotes' indentation is taken off every line, values and escapes
  work as in any text, and `quark fmt` puts the lines one level in from where the text starts;
- `true`, `false`;
- arrays: `[1, 2, 3]` and `[]` (the empty one takes the type it goes into);
- maps: `["ana": 31, "bia": 27]` and `[:]` (the empty one takes the type it goes into).

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
| `Int` | 64-bit integer; `+ - *` wrap around; `/` and `%` round down (floored) and stop on division by zero; `& \| ^ ~ << >>` work on its bits (`>>` keeps the sign; a shift count is taken modulo 64) | 1 |
| `Float` | 64-bit float | 1 |
| `Bool` | `true` / `false` | 1 |
| `Text` | text (UTF-8 bytes): joined with `+`, compared with `== < ...`, `len(t)` its bytes, sliced `t[a..b]` | 2 |
| `[T]` | array of T, sliced `xs[a..b]` | 2 |
| `[K: V]` | map from keys K (Int, Text, Bool or an enum's case) to values V, in the order the keys came | 1 |
| `T?` | a T, or nothing: `Some(x)` / `None` | 1 + T's |
| `T!` | a T, or an error with a message: `Ok(x)` / `Err("why")` | 3 + T's |
| `Never` | what never gives a value: `exit(code)`, `panic(message)`; it goes wherever a value goes | 0 |
| `type Name { ... }` | a record: named fields | sum of its fields |
| `type Name { fn f(...) -> R }` | an interface: functions any type with them can stand for (below) | one function value each |
| `enum Name { A, B(x: T) }` | one of named cases; a case may carry data | 1 + every case's data |
| `extern type Name` | a handle only the host makes (a file, a window, a connection) | 1 |
| `(A, B) -> R` | a function: one a parameter takes (inlined), or one kept as a value (below) | 1 |
| `effect (A) -> R` | a function kept as a value that changes the world | 1 |
| `Pair<A, B>` | a generic record or enum, given its types (below) | its instance's |

Types are **inferred**. You write them only on function parameters and record fields. A result, a
`let` or a constant may carry one: `let xs: [Int] = []`, `fn f(...) -> Float`.

**Conversions** that happen by themselves:
- an `Int` becomes a `Float` wherever a Float is wanted (`0.5 * count`);
- a T becomes a `T?` wherever an optional is wanted (`return i` in a function giving `Int?`), and a
  `T!` wherever a result is wanted (`Ok(it)`);
- a loop (`map`, `filter`, a range) becomes an array wherever an array is wanted.

The other way is written: `toInt` truncates, `floor`, `ceil` and `round` return an `Int`, and `toText`
makes text.

**Records** are built with the type's name. The fields go by position, then by name:

```quark
type Point { x: Float, y: Float }
let a = Point(1, 2)
let b = Point(x: 1, y: 2)
let c = a with { y: 5 }          // a copy with some fields changed
```

Records compare with `==` and `!=`, field by field, when every field can be compared (numbers,
Bools, texts, handles, cases, and records of those). Cases with data compare too: the same case, and the
same data (`Shape.Circle(1) == Shape.Circle(1)`). So do optionals and results: `x == None`, `x != None`,
and `x == 3` with `x: Int?` (the 3 becomes `Some(3)`). A record has `+`, `-`, `*`, `/`,
`%` and `-x` when its type gives them, and `<`, `>`, `sort`... when it gives `compare` (below).

**Enums** are values `Color.Red`. A case can carry data, `Shape.Circle(radius: 2)`, which a
`match` takes apart. Cases without data compare with `==`, and with `<` in the order they are
listed. `c.index()` is a case's place among its enum's cases, from 0.

**Optionals.** `T?` is a T or nothing. `Some(x)` and `None` make one. `a ?? b` gives what `a`
holds, or `b`, which is only worked out when needed:

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

**Results.** `T!` is a T, or an error that says why there is none. `Ok(x)` and `Err("message")`
make one; a plain T becomes `Ok(T)` where a result is wanted. `??` gives the value or a fallback,
and `match` takes one apart:

```quark
fn parseVersion(text: Text) -> Version! {
    let parts = split(text, ".")
    if len(parts) != 2 {
        return Err("expected major.minor, found \"{text}\"")
    }
    Version(parseInt(parts[0]) ?? 0, parseInt(parts[1]) ?? 0)
}

match parseVersion(input) {
    Ok(v) => print("version", v.major)
    Err(why) => printErr(why)
}
```

**`Never`.** `exit(code)` and `panic(message)` never come back, so they fit wherever a value goes: a
`match` arm, an `if` branch, the right of `??`. A function whose body always ends in one gives `Never`.

```quark
let n = parseInt(input) ?? panic("not a number: {input}")
let text = match readFile(path) {
    Ok(t) => t
    Err(why) => {
        printErr(why)
        exit(1)
    }
}
```

**`?` passes a failure on.** `a?` is what an optional or a result holds; when it holds nothing, the
function returns at once with that `None` (or that `Err`, message and all). So a function using `?`
on an optional gives an optional, and one using it on a result gives a result. In `main` and in a
test, `?` stops the program with the message and the line instead:

```quark
fn newest(texts: [Text]) -> Version! {
    var best = Version(0, 0)
    for t in texts {
        let v = parseVersion(t)?         // the first Err goes back to the caller
        if v > best {
            best = v
        }
    }
    best
}
```

**Sets.** `Set<K>` holds each value once, in the order they came (K as for a map's key). A list where a
set goes makes one (`let seen: Set<Text> = ["a", "b"]`, `[]`), and `xs.toSet()` does from any loop. A `var`
set changes in place with `s.add(v)` and `s.remove(v)`; `contains(s, v)`, `len(s)`, `for v in s` and every
loop function read it; `a | b`, `a & b` and `a - b` are the union, the intersection and the difference.

**Maps.** `[K: V]` holds a V for each key, and keeps the keys in the order they came. `m[key]` is an
optional (`None` when the key is not there). A `var` map changes in place:

```quark
var ages = ["ana": 31, "bia": 27]
ages["caio"] = 40                  // added
ages["ana"] = 32                   // replaced
ages.remove("bia")
for name, age in ages {            // in the order the keys came: ana, caio
    print(name, age)
}
print(len(ages), ages["ana"] ?? 0, contains(ages, "bia"), keys(ages), values(ages))
```

A map is a loop source too: its values are `Entry` records (`e.key`, `e.value`), in the order the keys came, so every
loop function works on it: `ages.count(e => e.value > 30)`, `ages.maxBy(e => e.value)`,
`ages.sortBy(e => e.value, descending: true)`. `countBy` makes one: `words(t).countBy(w => w)` is each
word's count.

**Arrays join** with `+`: `xs + [x] + ys` is a new array, one after the other.

**Slices.** `xs[a..b]` is the part of an array from a to b (b not included), and `t[a..b]` the part
of a text (in bytes). Either end may be left out: `xs[2..]`, `t[..3]`. A slice shares the memory it
comes from, and is checked: `xs[1..9]` of a shorter array stops the program.

**Text** is UTF-8. `s = s + t` on a `var` grows `s` in place when nothing else holds what comes after
it, so a text built a piece at a time costs what its length does.
- Join with `+`, compare with `== != < <= > >=` (byte by byte), and get its bytes with `len(t)`;
  `chars(t)` gives its characters, `len(chars(t))` how many.
- A slice `t[a..b]` is in bytes, and stops the program if an end would cut a character in two.
- `upper` and `lower` know accented Latin letters, Greek, Cyrillic and Armenian (`"ação"` becomes
  `"AÇÃO"`), and `{x:8}` pads by characters.
- Values go inside with braces: `"lap {ship.laps} at {ship.speed}"` takes Ints, Floats, Bools,
  texts and enum cases.
- A **format** after a colon: `{x:.2}` with two decimals, `{n:5}` at least five characters wide
  (spaces in front), `{x:8.3}` both.
- `toText(x)` does the same for one value.

**Types that hold themselves.** An enum's case, or a record's field, may hold a value of its own type,
directly or through an optional: trees, lists, expressions. Such a field is kept in the arena, and the
value holds its address (one slot), so building and taking apart costs one allocation per field:

```quark
enum Expr { Num(v: Int), Add(a: Expr, b: Expr), Neg(e: Expr) }

fn eval(e: Expr) -> Int = match e {
    Num(v) => v
    Add(a, b) => eval(a) + eval(b)
    Neg(x) => -eval(x)
}

type Node { value: Text, next: Node? }
```

A field that holds its own type is set whole (`n.next = Node("z", None)`), never through
(`n.next.value = ...` is an error: build a new one with `with`). A record whose field is its own type
directly (not optional) could never be built, so it is an error.

**Patterns inside patterns.** An arm takes apart what a case holds, a record or a tuple, as deep as it
goes: `Some(Circle(r)) if r > 10`, `Some(Box(w, 1))` (an Int pattern on a Float too), `Point(0, y)`,
`(q, r)`. A `match` must cover every value (checked case by case, inside the cases too), or end with `_`.
`let Point(x, y) = p` and `let (q, r) = divmod(17, 5)` need no `else`: they always match; one that may
not match needs it (`this pattern may not match`).

**Tuples.** `(Int, Text)` is a record of its own, its fields `t.0` and `t.1`: `(a, b)` makes one, and a
function gives back several values in one (`fn divmod(a: Int, b: Int) -> (Int, Int) = (a / b, a % b)`).

**Records as keys.** A record of one or two Ints, Bools or cases (and a tuple of them) is a map's key:
`[Cell: Tile]` for a grid, `[(Int, Int): Int]`.

**Results whose error is a type.** `T!E` (`T ! E` too; `quark fmt` writes `T!E`) holds a T or an E: `fn parseAge(t: Text) -> Int!ParseError`,
`Err(ParseError.Empty)`. `?` passes an `Err` on when the function's own `Err` holds the same type;
`match` takes it apart (`Err(e) => explain(e)`). `T!` alone is `T!Text`, the message.

**Generic types.** A record or an enum can take type parameters: `type Pair<A, B> { first: A,
second: B }`, `enum Tree<T> { Leaf, Node(left: Tree<T>, value: T, right: Tree<T>) }`. Where it is
built, its types come from the values (`Pair(1, "one")` is a `Pair<Int, Text>`); a case without data
(`Tree.Leaf`) takes the type it goes into, as `None` does, so a `let` of one needs its type written:
`var t: Tree<Int> = Tree.Leaf`. Each list of types gets its own instance, compiled for those types.
A generic type's functions are generic functions: `fn Pair.swap(self) -> Pair<B, A>`, with `A` and `B`
Pair's own.

## Declarations

```quark
fn name(param: Type, other: Type = default) -> Result { statements; the last one is the result }
fn name(param: Type) -> Result = expression          // one expression
fn name<T>(xs: [T]) -> T? { ... }                    // generic (below)
fn name(f: (Int) -> Int, x: Int) -> Int = f(f(x))    // takes a function (below)
const name = value                                   // a constant, worked out when compiled (below)
type Name { field: Type, ... }
type Name { fn name(param: Type) -> Result, ... }    // an interface (below)
enum Name { CaseA, CaseB(data: Type) }
extern fn name(param: Type) -> Result                // the host's: a C function, no body
extern effect fn name(...)                           // the host's, and it changes the world
extern type Name                                     // the host's handle (a file, a window...)
fn Type.name(self, ...) -> Result                    // a function of a type (below)
test "what it checks" { ... }                        // a test, run by quark test (below)
```

- `-> Result` may be left out when the body says it, except when the function calls itself, returns
  a value early or uses `?`. A bare `return` in a function without `-> Result` means it gives nothing.
- **Arguments** go by position, then by name: `report(items, title: "Today")`. A parameter with a
  default (`title: Text = "Stock"`) may be left out. Defaults are pure values, worked out where they
  are used. The built-ins take named values too, in their order: `split(t, separator: ",")`,
  `clamp(v, low: 0, high: 1)`, `xs.sort(descending: true)`.
- **Constants** (`const name = value`, at the top of a file) are pure, and worked out once, when the
  program is compiled: numbers, Bools and texts (joined, with values put in), arrays of them, `if`s on
  what is known and other constants become the value itself; an array of numbers is kept in the
  program's data and never allocated again. A mistake found there (`const z = 1 / 0`) is an error
  before the program runs. A constant the compiler cannot work out ahead (a call to a function) is
  worked out where it is used. Inside a function, `let` names a value; at the top, `const` does.

### Functions of a type, and operators

A record, an enum or an extern type can have functions of its own, named `Type.name`. The first
parameter is `self`, the value the function works on; its type is the type's, so it is not written
(for a generic type, `self` is a `Pair<A, B>`, and the function takes Pair's `A` and `B`). They are
called like any function, `a.f(b)` or `f(a, b)`, and the receiver's own function wins over a plain
function of the same name:

```quark
type Vec2 { x: Float, y: Float }

fn Vec2.plus(self, b: Vec2) -> Vec2 = Vec2(self.x + b.x, self.y + b.y)

fn Vec2.times(self, k: Float) -> Vec2 = Vec2(self.x * k, self.y * k)

fn Vec2.length(self) -> Float = sqrt(self.x * self.x + self.y * self.y)

let speed = (Vec2(3, 4) + 2 * Vec2(1, 0)).length()
```

An operator on a value of such a type calls its type's function:

| Written | Calls |
| --- | --- |
| `a + b`, `a - b`, `a * b`, `a / b`, `a % b` | `T.plus(a, b)`, `T.minus`, `T.times`, `T.div`, `T.rem`, with T the type of `a` |
| `-a` | `T.negate(a)` |
| `k * a`, with `k` a number | `T.times(a, k)`: a number times a value is the value times the number |
| `a < b` (and `<= > >=`), `sort`, `minBy`... | `T.compare(a, b)`: below 0, 0 or above 0 |
| `a += b` (and `-= *= /=`) | `a = a + b` |

The value's type decides: `v * 2` and `2 * v` both call `fn Vec2.times(self, k: Float)`. These
functions are inlined like any small function, so `a + b` on a `Vec2` costs two float additions. In
a graph, `a + b` stays an operator node.

```quark
type Version { major: Int, minor: Int }

fn Version.compare(self, b: Version) -> Int =
    if self.major != b.major { self.major - b.major } else { self.minor - b.minor }

let newest = sort(versions)[len(versions) - 1]
```

### Generic functions

A function can take **type parameters**, written after its name: `fn largest<T>(xs: [T], ...)`. Each
use is checked with the types it is given, and gets its own copy of the function, compiled for
those types, so a generic function is as fast as one written for each type:

```quark
fn pairs<T>(xs: [T]) -> [[T]] {
    var out: [[T]] = []
    for x, i in xs {
        if i % 2 == 0 {
            out.push(xs[i..min(i + 2, len(xs))])
        }
    }
    out
}

print(pairs([1, 2, 3, 4, 5]), pairs(["a", "b"]))   // [[1, 2], [3, 4], [5]] [[a, b]]
```

The types come from the values given (`pairs([1, 2])`: T is Int). A type parameter is PascalCase. It may
name an interface its type must fit, `fn twice<T: Speaker>(x: T) -> Text = x.speak() + x.speak()`: a call
with a type that lacks one of the interface's functions is an error where it is called, naming what is
missing, and each instance is still compiled for its own type (no indirect calls).

### Functions that take functions

A parameter can take a function: `f: (Int) -> Int`. A call gives it a lambda or a function's name
(a built-in's too), and the parameter is called like a function. Such a function is copied into
every place that calls it, with the lambda put in, so it costs nothing and its lambdas can use the
names around them:

```quark
fn applyTwice(f: (Int) -> Int, x: Int) -> Int = f(f(x))

fn top<T>(xs: [T], n: Int, score: (T) -> Int) -> [T] = sortBy(xs, x => -score(x))[..min(n, len(xs))]

let k = 10
print(applyTwice(x => x * k, 3), applyTwice(square, 2), top(words, 3, len))
```

A function given this way is passed on, called, or given to `map`, `filter`... It is never kept in
a `let` or a field: every call stays a visible node, and nothing is allocated for it. A function
that takes a function cannot call itself.

### Functions kept as values

A function is a value too: a function's name, or a lambda whose type is written, can be kept in a
`let`, a `var`, a field, an array or a map, given back from a function, and called through:

```quark
type Button { label: Text, onPress: (Int) -> Int }

fn double(x: Int) -> Int = x * 2

let f = double
let g: (Int) -> Int = x => x + 100
let b = Button("ok", double)
print(f(4), g(1), b.onPress(9), [double, g].map(op => op(6)))
```

A call through a value is an indirect call (it is not inlined). A function that changes the world has
an effect type, `effect (Text) -> Int`, and calling it is an effect. In a graph, a kept function is a
value typed into a pin, as a function given to `map` is. A lambda whose parameters say their types
needs no type written: `let f = (x: Int) => x * k`, and its effects are its body's
(`let log = (t: Text) => print(t)` is an `effect (Text) -> nothing`). A function a value gives is called directly:
`adder(2)(3)`.

**Closures.** A lambda kept as a value takes the values of the names around it that it uses, as they
are where it is made. Values never change behind your back, so this is exactly what it means: a later
`k = 1000` does not change what the lambda took.

```quark
fn adder(k: Int) -> (Int) -> Int = x => x + k

fn compose(f: (Int) -> Int, g: (Int) -> Int) -> (Int) -> Int = x => g(f(x))

let add2 = adder(2)
print(add2(3), compose(add2, x => x * 10)(1))   // 5 30
```

A parameter that takes a function (`f: (Int) -> Int`) is inlined with its lambda when the function
only calls it or passes it on, so it costs nothing; when the function keeps it (captures it in a
lambda, stores it, gives it back, as `compose` does), the compiler sees that and passes a function
value instead.

**Interfaces.** A type that lists functions instead of fields is an interface: any type with a function
of its own for each one goes where the interface is wanted. So one array holds values of different
types, and each call goes to its own type's function. A function that changes the world says so
(`effect fn`), since an interface has no body to infer it from; one without `->` gives nothing:

```quark
type Speaker { fn speak() -> Text }
type Log { effect fn write(line: Text) }

fn Dog.speak(self) -> Text = "woof, I am {self.name}"

fn Cat.speak(self) -> Text = "meow"

let pets: [Speaker] = [Dog("rex"), Cat("tom")]
print(pets.map(p => p.speak()))
```

A record may hold functions as data too (`type Button { label: Text, onPress: (Int) -> Int }`): that
is a record, built with its fields, never an interface.

A generic function calls what it needs on its T (`fn talk<T>(x: T) -> Text = x.speak()`); each
instance is checked with its own type, and a type without the function is an error that names it:
`Rock has no function speak: talk<Rock> needs fn Rock.speak(...)`.

### Files: use and pub

`use geometry` at the top of a file brings in the `pub` declarations of `geometry.qk`, the file next
to it; `use shapes.round` is `shapes/round.qk`. `use geometry as geo` brings them in by the alias only:
`geo.area(s)`, `geo.Vec2(1, 2)`, `[geo.Shape]`, `geo.Shape.Circle(1)`, and a type's own functions and
operators work on its values as before. `use std.math`, `use std.json`, `use std.time`... are the standard library,
written in Quark and kept in the compiler (below). A file sees its own declarations, the host's, and the
`pub` ones of the files it uses, not the files those use. Two files may each have a private function of
one name; a `pub` name that meets another in a file that sees both is an error that names the file.

```quark
// geometry.qk
pub type Vec2 { x: Float, y: Float }

pub fn Vec2.plus(self, b: Vec2) -> Vec2 = Vec2(self.x + b.x, self.y + b.y)

fn square(x: Float) -> Float = x * x // only geometry.qk sees it
```

```quark
// main.qk
use geometry

fn main() {
    print(Vec2(1, 2) + Vec2(3, 4))
}
```

A file's graph holds its own declarations and its `use`s, as nodes of their own. A host compiles a
program that uses files through `qkModuleReader` (the file system by default).

### Packages

A project that uses other people's code names it in `quark.toml`, at its root: its own name and
version, and each package by git URL (or a path to a repository) and a tag or a commit:

```toml
[package]
name = "app"
version = "0.1.0"

[dependencies]
geom = { git = "https://example.org/geom.git", tag = "v1.2.0" }
util = { git = "../util", rev = "3f2a9c1e07" }
```

`quark get` fetches each package with the system `git` into `.quark/pkg/<name>/`, and the packages
those name in their own `quark.toml` (one folder per name for the whole project), and writes
`quark.lock`: each package's exact commit and a SHA-256 of its files. Commit `quark.toml` and
`quark.lock`; `.quark/` keeps itself out of git. Then `use geom.shapes` is `.quark/pkg/geom/shapes.qk`
and `use geom` is `.quark/pkg/geom/geom.qk`, from any file of the project (the nearest `quark.toml`
above it), with the same `pub` rules as a file's. `quark run`, `build`, `check` and the rest need no
network: they check each package a program uses against `quark.lock` and stop at a package that is
not fetched, or whose files are not the locked ones (`hash mismatch`: `quark get` puts them back).

The lock wins: while `quark.toml` asks for what was locked, `quark get` checks out the locked commit,
even if the tag has moved since, and fails if its files hash to anything else. To update a package,
change its tag or commit in `quark.toml` and run `quark get`. A package's name is what `use` says
(lowercase, digits, `_`) and its own `quark.toml`'s name; one asked for at two different pins, or a
package that comes back to itself (`a cycle of packages: app -> util -> app`), is an error, found
before any package's files change. A file next to yours with a package's name is one too.

The manifest is TOML, not Quark: it is data, read before any code is, by `quark get` and by tools
that know nothing of Quark, and a lock file wants the same format.

### Effects

A function **changes the world** when it prints, reads or writes files, reads the clock or the
input, draws a random number, runs tasks, or calls a host function marked `effect`, or a function that
does any of that. Everything else is **pure**: given the same values, it gives the same result and
changes nothing. Nothing is written for it: the compiler works it out from each function's body, as it
works out types, so it costs nothing when the program runs. It keeps the line between the two:

- a lambda given to `map`, `filter`, `fold`... never has effects, so loops can be fused, reordered and
  run in parallel (`parallelMap`, below);
- a constant or a default never has effects;
- a function value's type says whether it has effects (`effect (Text) -> Int`), and a lambda kept in
  one whose type says none has none.

`effect` is written only where there is no body to look into: the host's functions
(`extern effect fn`), an interface's functions, and function types. In a node graph, a pure call is a
data node, and an effect call is an action node on the execution wire. Every node says which it is
(`"effect": true`), so an editor draws it right.

```quark
extern type Item
type Point { x: Float, y: Float }
extern fn position(i: Item) -> Point                 // reads: a data node
extern effect fn moveBy(i: Item, dx: Float, dy: Float) // acts: an action node

fn tick(item: Item, dt: Float) { // calls moveBy: it changes the world
    if position(item).x < 300 {
        moveBy(item, 120 * dt, 0)
    }
}
```

**Host declarations.** `extern fn` is a function the host provides, and `extern type` is a value
only the host makes. A script can pass it on and compare it, but never build it. A host usually gives
these as its **prelude**, which every program it compiles sees. See [EMBEDDING.md](EMBEDDING.md).

- Small functions are inlined where they are called; that is free, and shows nowhere in the program.
- A program run from the command line starts at `fn main()`. A program for a host (an application,
  a server, an editor) has the functions the host calls, such as `start` and `tick`. `fn main` is not
  needed there.

### Tests

`test "name" { ... }` at the top of a file is a test: `quark test file.qk` runs each one on its own,
with its own memory, and says which passed. `assert(condition)` (or `assert(condition, "why")`)
stops a test that fails, and so does a panic or a `?` that finds nothing; the other tests still run.
Tests can print, and are never called by the program.

```quark
fn gcd(a: Int, b: Int) -> Int = if b == 0 { a } else { gcd(b, a % b) }

test "gcd of two numbers" {
    assert(gcd(12, 18) == 6)
    assert(gcd(7, 3) == 1, "seven and three share nothing")
}
```

```text
$ quark test math.qk
ok    gcd of two numbers (0.0 ms)
1 passed, 0 failed
```

## Statements

| | |
| --- | --- |
| `let x = value` | a name for a value; it never changes |
| `var x = value` | a value that can change: `x = ...`, `x += ...` (`-= *= /=`), `xs[i] = ...`, `m[k] = ...`, `p.x = ...`, `xs.push(v)`, `m.remove(k)` |
| `let Case(a, b) = value else { ... }` | the data of one case, or the else block, which must leave (`return`, `break`, `continue`, `exit`, `panic`) |
| `for x in source { }` | over an array, a range `a..b` (b not included) or a loop; `for x, i in ...` gives the index too; over a map, `for key, value in m` |
| `while condition { }` | |
| `defer { ... }` | runs when the block it is in is left: at its end, at a `return` or a `?` (all of the function's), at a `break` or `continue` (the loop's); the last one written runs first. It is placed where the program is compiled, so it costs what its code does. Its block cannot leave (`return`, `?`); a panic runs none |
| `break`, `continue` | |
| `return value` | |
| an expression | a call, an `if`, a `match` or an `a?` whose value is not kept |

A value that is computed and then dropped (`x + 1` alone on a line) is an error.

`let ... else` keeps the happy path flat:

```quark
fn firstWord(t: Text) -> Text? {
    let Some(w) = first(words(t)) else {
        return None
    }
    upper(w[0..1]) + w[1..]
}
```

## Expressions

Precedence, from loosest to tightest:

| | |
| --- | --- |
| `a ?? b` | the optional's (or result's) value, or b |
| `or` | |
| `and` | |
| `not` | |
| `== != < <= > >=` | they do not chain: `a < b < c` is an error |
| `a..b` | a range of Ints |
| `+ -`, and the bits' `\| ^` | |
| `* / %`, and the bits' `& << >>` (as in Go and Swift: `a & b == c` is `(a & b) == c`) | |
| `-x`, `~x` | |
| `f(x)`, `a.f(x)`, `a.field`, `a[i]`, `a[i..j]`, `a with { ... }`, `a?` | |

- **`a.f(b)` is `f(a, b)`.** Any function reads left to right this way: `xs.map(f).sum()`.
- **`if`** and **`match`** are expressions:

  ```quark
  let sign = if x < 0 { -1 } else { 1 }
  let name = match color {
      Red => "red"
      Green => "green"
      _ => "other"
  }
  ```

- **Patterns.** A `match` arm starts with an enum case (`Circle(r)`, `Some(x)`, `Err(why)`), an Int
  (`3`), a range of Ints (`80..90`, `90..`, `..0`: the end is not included), a text (`"quit"`), or
  `_`. Any arm may add `if condition`, which can use the arm's names:

  ```quark
  fn describe(x: Int?) -> Text = match x {
      Some(n) if n < 0 => "negative"
      Some(n) if n == 0 => "zero"
      Some(n) => "positive {n}"
      None => "nothing"
  }

  fn grade(score: Int) -> Grade = match score {
      90.. => Grade.A
      75..90 => Grade.B
      _ => Grade.C
  }
  ```

  A `match` on an enum, an optional or a result names every case (an arm with `if` does not count)
  or ends with `_`. A `match` on an Int or a Text ends with `_`.
- **Lambdas** `x => x * 2` and `(acc, x) => acc + x` go into `map`, `filter`, `fold`, `any`,
  `all`, `first`, `count`, `sortBy`... and into functions that take functions, and are always
  inlined. A function's name works there too: `xs.map(square)`, `words.map(len)`.
- **Blocks** `{ ... }` give their last expression.

## Loops that fuse

`map` and `filter` build nothing: they add code to the loop of their source. The loop ends in
whatever consumes it: `sum`, `mean`, `fold`, `min`/`max`, `any`/`all`, `first`/`last`, `count`,
`countBy`, `contains`, `minBy`/`maxBy`, `len`, `for`, or an array when one is needed. `take(n)` and
`skip(n)` add a counter to the loop: `take` ends it once it has n, so nothing past them is worked out
(a count past the end, or below zero, is simply the end).

```quark
let evens = (0..1_000_000).filter(x => x % 2 == 0)   // used once below: never stored
let total = evens.map(x => x * x).sum()             // one machine loop, no array
let firstTen = (0..1_000_000).filter(isPrime).take(10) // stops at the tenth prime
```

A `let` holding a loop that is used **once**, at the same loop depth, is fused into that use. Used
twice, it is computed once into an array. Either way the result is the same, because values never
change.

Indexes are checked. The check is dropped where it cannot fail: `for i in 0..len(xs) { xs[i] }`, with
`xs` a `let` or a parameter.

## Tasks: functions that run a step at a time (coroutines)

`spawn(f)` makes a task of a function of no values (a lambda, or a function's name) and gives its
number. `step(task)` runs it until it calls `yield()` (or `wait(seconds)`, which gives the turn back
until the time went by), and is `true` while it has more to do. Each task runs on a stack of its own,
so a task can yield from anywhere, deep inside the functions it calls. A panic in a task stops whoever
stepped it, with its message. Outside a task, `yield()` does nothing and `wait(seconds)` sleeps.

```quark
fn countdown(name: Text, from: Int) {
    for i in 0..from {
        print(name, from - i)
        yield()
    }
}

let a = spawn(() => countdown("a", 3))
while step(a) {
    print("the caller goes on")
}
```

A host runs a script's function as a task with `qkTaskStart` and `qkTaskResume`, whenever it is ready
for the next step: a long job done a piece at a time, a conversation that waits for each answer, a
workflow that waits for an event. They are effects, in the capability group `tasks`. While a task is alive, loops keep the memory they make until the call
ends (tasks share the arena).

## Threads: parallelMap

`xs.parallelMap(f)` is `xs.map(f)` with the elements shared out to the processor's threads: each thread
loads its own copy of the program and calls `f` on its part. `f` is a function: a name, or a lambda
(its captured values are read by every thread; they never change). It must be pure (no effects), and give plain values (numbers, Bools,
cases, records of those), so each thread's memory goes when it is done. A panic in a thread stops the
program (or the host's call) with its message. With fewer elements than threads it runs on fewer.

```quark
fn collatz(n: Int) -> Int { ... }

let steps = (1..200_001).parallelMap(collatz) // 5.5 times faster than map, measured on one desktop processor
```

Host functions a pure `f` calls may be called from several threads at once.

## Values, not references

Every value behaves as a copy. A `var` holds its own array (or map): storing it anywhere copies it,
and so does giving a `var` an array that came from somewhere else, so changing one never changes
another:

```quark
let a = [1, 2, 3]
var b = a
b[0] = 100      // a is still [1, 2, 3]
```

Only what is needed is copied. A `let` of a `let` shares the array, arrays passed to functions are
never copied (functions cannot change their parameters), and a freshly made array (`fill`, `map`,
a literal...) goes into a `var` as it is. `xs.push(v)` grows a `var` array in place, with room
reserved, so building a list one value at a time is linear; `m[k] = v` changes a `var` map in place.

## Memory

Memory comes from an **arena**: allocating bumps a pointer, and everything is freed at once when the
program (or, in a host, a call) ends. There is no garbage collector walking the heap and no pause.

A **loop that makes things** gets its memory back on the way. It marks the arena where it starts;
when its turns have made enough (twice what it keeps alive, at least 4 MB), the values that the
`var`s from outside the loop hold are copied out, and everything else the turns made goes at once.
Values shared by several `var`s stay shared. Loops that make nothing (the numeric ones) have none of
this, so they run as before. A loop of three million turns that builds a text, a map and a list each
turn stays at about 10 MB, where it would need over 300 MB without it.

## Built-in functions

| | |
| --- | --- |
| Math | `sqrt sin cos tan asin acos atan atan2(y, x) exp log pow(x, y)`, `abs`, `floor ceil round` (→ Int), `toFloat toInt`, `min(a, b) max(a, b) clamp(value, low, high) lerp(a, b, t)` |
| Loops | `map(f)` (f may take the index too: `(x, i) => ...`), `filter(f)`, `take(n)`, `skip(n)`, `fold(start, (acc, x) => ...)`, `sum()`, `mean()` (→ `Float`), `min()`, `max()`, `any(f)`, `all(f)`, `first()` / `first(f)` and `last()` / `last(f)` (→ `T?`), `count()` / `count(f)`, `countBy(f)` (→ `[K: Int]`), `contains(v)`, `indexOf(v)` (→ `Int?`), `minBy(f)`, `maxBy(f)`, `len()`, `toArray()` |
| Arrays | `fill(n, value)`, `[a, b, c]`, `[]`, `xs[i]` (checked), `xs.get(i)` (→ `T?`: `None` outside, no panic), `xs[a..b]`, `xs + ys`, `xs.push(v)` (a `var`), `sort()`, `sortBy(f)` (both stable; `descending: true` for the other way), `reverse()`, `parallelMap(f)` |
| Maps | `[k: v, ...]`, `[:]`, `m[k]` (→ `V?`), `m[k] = v` and `m.remove(k)` (a `var`), `len(m)`, `contains(m, k)`, `keys(m)`, `values(m)`, `for k, v in m`, and every loop function, over `Entry(key, value)` records |
| Text | `"... {x} ..."`, `"{x:.2}"`, `+`, `== <` ..., `len(t)`, `t[a..b]`, `toText(x)`, `split(t, separator)`, `words(t)`, `lines(t)`, `chars(t)`, `trim(t)`, `upper(t)`, `lower(t)`, `contains(t, part)`, `indexOf(t, part)` (→ `Int?`), `startsWith`, `endsWith`, `replace(t, old, new)`, `repeat(t, n)`, `join(texts, separator)`, `parseInt(t)` / `parseFloat(t)` (→ `Int?` / `Float?`) |
| Optionals, results | `Some(x)`, `None`, `Ok(x)`, `Err(message)` or `Err(value)` in a `T!E`, `a ?? b`, `a?`, `match`, `let ... else`, `==` |
| Tuples | `(a, b)`, `t.0`, `let (q, r) = divmod(7, 2)`, as a map's key |
| Random | `random()` (a Float in [0, 1)), `randomInt(lo, hi)` (hi not included), `seedRandom(n)`: effects, group `random` |
| Tasks | `spawn(f)`, `step(task)`, `yield()`, `wait(seconds)`: effects, group `tasks` |
| Enums | `c.index()` |
| Tests, failures | `assert(condition)`, `assert(condition, message)`, `panic(message)` (→ `Never`) |
| Effects | `print(a, b, ...)`, `printErr(a, b, ...)`, `now()` (seconds, for timing), `time()` (seconds since 1970), `readLine()` (→ `Text?`, `None` at the end of the input), `readFile(path)` (→ `Text!`), `writeFile(path, text)` (→ `Bool`), `fileExists(path)`, `listDir(path)` (→ `[Text]!`), `env(name)` (→ `Text?`), `args()` (→ `[Text]`: `quark run file.qk -- a b`, or the arguments of a built executable), `exit(code)` (→ `Never`) |

## When something goes wrong

- **Before running:** every error has a place and a caret under it. When a name is mistyped, it gives
  the closest one (`unknown name "totl": did you mean "total"?`, `f has no parameter "size"`). The
  rules say what to do: `print has effects (it changes the world): write effect fn greet`.
- **While running:** an index outside an array, a slice past its end, a division by zero, `min` or
  `mean` of nothing, a failed `assert`, `panic(message)` or a `?` in `main` that finds nothing stops
  the program with the line,
  and with the calls it went through, inlined ones included:

  ```text
  tests/panics/trace.qk:5: division by zero
    in ratio
    called from down at line 3
    called from main at line 8
  ```

  In a host, it stops only that call. Calls that go too deep (a function that calls itself without
  end) stop the same way, with `too many calls deep`, never crashing the process; the same call
  repeated is said once (`... called from down at line 2, again and again`). The command line gives
  programs a 256 MB stack, so recursion as deep as millions of calls works.
- **Capabilities:** `quark run --allow print,clock file.qk` lets a program reach only what is listed:
  host functions by name (`moveBy`), by prefix (`Object.*`), and the built-in groups `print`, `files`,
  `env`, `args`, `exit`, `stdin`, `clock`. A call to anything else is an error before it runs
  (`setColor is not given to this program`). A host gives each program its list with
  `qkVmCapabilities`: a third-party plugin gets less than the application's own scripts.
- **Overflow:** with `--checked` (and in `quark test`), an Int `+ - *` past the limits stops the program
  instead of wrapping around; `toInt` (and `floor`, `ceil`, `round`) of a Float too big for an Int, or
  not a number, always does.
- **A budget:** `quark run --fuel N` stops a program after N loop steps. That is how a host keeps a
  script from hanging it.

## The node graph

`quark graph file.qk` writes the program as nodes (JSON). `quark text file.json` writes the program
back.

| Text | Graph |
| --- | --- |
| `let x = ...`, `var x = ...`, a parameter, a `for` variable, a name in a match arm | a named node; every use of the name is a wire from it |
| `f(a, b)`, `a + b`, `a.field`, `xs[i]`, `[a, b]`, `[k: v]`, `a with { ... }`, `"... {x} ..."`, `a?` | a node with input pins |
| `12`, `"text"`, `true`, `Color.Red`, a constant, `None`, a function given to `map` | a value typed into a pin, not a node |
| `x => ...` | a node holding a small graph, with its parameters as named nodes |
| the statements of a block | the order of the block's list: the execution wire |
| a call that changes the world (inferred) | an action node (`"effect": true`) |
| `if`, `match`, `for`, `while`, `let ... else` | nodes holding the blocks they run; a match arm's pattern and its `if` ride on the arm |
| a constant, a default value | its own small graph |
| comments and blank lines | notes on the node they belong to |

Every node carries a `key`, a `line` in the canonical text, and its `type`:
- a named node is keyed `function/name` (when a function names two, as two loops' `i`, each is keyed
  by its place too: `main/@3/i`);
- another statement is keyed by its place (`update/@3`, and `update/@3/@0` inside it);
- an input is keyed by its order (`update/at.0`).

An editor keeps node positions in a side file under those keys (`quark graph --layout pos.json` puts
them back in as `"pos"`), so moving nodes never touches the program. Graph → text always gives the
canonical text, so a graph edited in a node editor and a program edited as text can live in one
repository. `quark catalog` lists every node an editor can offer. Each pin also has a `"shape"`, its
type as data, so an editor can check a wire without reading type text. `map`'s `f` pin, for example:
`{"kind": "fn", "params": [{"kind": "param", "name": "T"}, {"kind": "int"}], "returns": {"kind": "param", "name": "U"}}`.
The kinds are `int`, `float`, `bool`, `text`, `any`, `never`, `nothing`, `enum` (any enum's case),
`array` / `loop` / `range` / `optional` / `result` (with an `element`), `map` (`key`, `element`),
`tuple` (`items`), `fn` (`params`, `returns`, `effect`), `named` (a record, an enum or an extern type,
with its `args`), `param` (a type parameter: pins with the same name take the same type; `Number` is
one with `"oneOf"` Int, Float) and `oneOf` (`options`). A built-in's `"pins"` also say when a pin
takes a `var` (`"mutable"`), may be left out (`"optional"`) or takes any number of values
(`"variadic"`).

### Versions side by side

Keys make changes readable and mergeable by node rather than by line:

```
$ quark diff base.qk ours.qk
changed  main/n: input 0: 10 -> 12
added    main/@0: call print
$ quark merge base.qk ours.qk theirs.qk -o merged.qk
conflict main/n: changed on both sides
```

`quark diff a b` reports each node **added**, **removed** or **changed** (which inputs or values, as
`input 0: 10 -> 12`) and each statement **moved** (in its block, or to another one). Pass `--json` for
`{"changes": [{"change", "key", "kind", "in", "line", "fields": [{"field", "was", "now"}]}]}`, which
lists every node, including those inside an added one. The line is in the first version's text, or in
the second's for an added node. Exit 0 means the same program, 1 means changes.

`quark merge base ours theirs [-o out]` is a three-way merge. What only one side changed is taken
from that side, each block's statements merge, and the result is canonical text (a graph when
`out` or the inputs are `.json`). Where both sides changed one node, added statements at one place,
reordered one block, or one removed a node the other still uses, the merge keeps ours (and the
node), names the conflict by key and exits 1. `--json` gives the conflicts as
`[{"key", "what"}]`. A merged program with errors is reported too (exit 1).

Both commands take `.qk` files (through their graphs) or `.json` graphs. A statement keyed by its
place (`main/@3`) gets a new place when one is inserted before it, so before comparing, the second
version's statements are lined up with the first's block by block: named statements by name, then
equal statements in order, then statements of the same kind between those. A new statement whose
place-key is taken shows with its own key in a diff (`main/@0`). Statements that moved are found by
their content.

## Tools

| | |
| --- | --- |
| `quark run file.qk [-- args]` | compile to machine code in memory and run it |
| `quark build file.qk [-o out]` | a standalone executable |
| `quark build file.qk --target wasm32 [-o file.wasm]` | a WebAssembly module, and `file.js` beside it: its loader and runtime. `node file.js [args]` runs it; in a page, `Quark.run(bytes, {write(fd, bytes), args, files, host})` gives a promise of its exit status (`files`: `{path: text}` it can read, in memory; `host`: its `extern fn`s). Panics say what the native ones say. Not yet: tasks and the debugger; `parallelMap` runs on one thread, and memory is given back only when the program ends |
| `quark test file.qk` | run its tests |
| `quark fmt file.qk [--write]` | the canonical text |
| `quark check file.qk` | only the errors |
| `quark get [dir]` | fetch the packages `quark.toml` names (git) into `.quark/pkg`, and write `quark.lock` (above) |
| `quark lsp [--prelude host.qk]` | a language server for editors (stdio): errors as you type, formatting, the type under the cursor, go to definition, references, rename, the types worked out (inlay hints), a call's signature, the file's declarations, completion, and quick fixes (below) |
| `quark dap [--prelude host.qk]` | a debug adapter for editors' debuggers (the Debug Adapter Protocol, stdio; below) |
| `quark doc file.qk [-o out.md\|out.html]` | documentation: the `pub` declarations of the file and of the files it uses, each with its signature and the comment above it; Markdown on stdout or in a `.md`, one page in a `.html`. A type's own functions are listed under it; a result type left to be worked out is shown worked out |
| `quark code file.qk [-o code.bin]` | the machine code (x86-64 or ARM64, as built), raw, and each function's offset: for a disassembler |
| `quark object file.qk -o file.o [--symbol name] [--target arm64-macos]` | an object file a host links: the program compiled ahead of time, for platforms that allow no code made while running (see EMBEDDING.md). `--target` makes it for another machine: `x86_64-windows` (COFF), `x86_64-linux`, `arm64-linux` (ELF), `x86_64-macos`, `arm64-macos`, `arm64-ios` (Mach-O) |
| `quark debug file.qk [--break LINE]...` | runs it stopping at those lines (or at the first): `c` go on, `s` (or Enter) the next line, `p name`, `l` the function's names and values, `bt` the calls, `b LINE`, `d LINE`, `q` |
| `quark run file.qk --trace values.json` | writes the last value of every named node, by the keys of its graph (`{"main/total": {"value": "3", "hits": 4}}`): what a node editor shows on its wires |
| `quark repl` | declarations, `let`s and expressions line by line: an expression's value is printed |
| `quark run file.qk --profile` | where the time goes, by function (one sample a millisecond; inlined functions count in their caller) |
| `quark run file.qk --checked` | Int `+ - *` past their limits stop the program (`quark test` always checks) |
| `quark run file.qk --memory MB` | at most so much memory, then a panic |
| `quark graph` / `quark text` / `quark catalog` | node graphs both ways, and what an editor can offer (pins with typed shapes) |
| `quark diff a b [--json]` | the changes between two versions, by node key: nodes added, removed, changed (which inputs), statements moved (`.qk` or `.json`; exit 1 when they differ) |
| `quark merge base ours theirs [-o out] [--json]` | a three-way merge by node key: canonical text (or a graph, for `.json`); conflicts named by key, ours kept there (exit 1) |

**Completion and quick fixes.** After a dot the language server offers what goes there: a record's fields
and its type's functions (`p.` → `x`, `y`, `scaled`), an enum's cases (`Color.` → `Red`, `Green`), a
module's `pub` declarations (`g.` after `use geometry as g`), and for other values the built-in functions
(`xs.map`). Inside a call it offers the names of the values not given yet (`move(p, ` → `dx:`, `dy:`).
An error whose message says what to write comes with that edit as a quick fix: a snake_case name made
camelCase everywhere in the file, `!x` made `not x`, a `let` at the top of a file made `const`,
`effect fn` made `fn`, a misspelled name, field, case or parameter made the one meant ("did you mean"),
and a `let` that is changed later made a `var`.

**Debugging in an editor.** `quark dap` speaks the Debug Adapter Protocol, so VS Code and other clients
debug Quark: `launch` with `"program"` (the `.qk` file), `"args"` and `"stopOnEntry"`; breakpoints by
line in any of the program's files; continue, step over (`next`), step in and step out; the calls
(stack trace), each one's names and their values (as `print` writes them), and `evaluate` for a name.
The program's output comes to the client as output events. For VS Code, a launch configuration of a
debugger type whose adapter is `quark dap`:

```json
{ "type": "quark", "request": "launch", "name": "Debug", "program": "${file}", "stopOnEntry": false }
```

The program runs in the adapter's own thread, so it stops only at breakpoints and steps (pause does
nothing), and it reads no standard input (that is the client's).

## The standard library

Written in Quark, in `std/`, kept inside the compiler (`use std.name`, no file needed):

| | |
| --- | --- |
| `std.math` | `pi`, `tau`; `Vec2`, `Vec3` with `+ - * /` and unary `-`, `dot`, `cross`, `length`, `distance`, `normalized`, `lerp`, `angle`, `rotated` |
| `std.json` | `enum Json { Null, Bool, Num, Str, List, Object }`, `parseJson(text) -> Json!`, `toJson(j)`, `j.field(name)`, `j.number()`, `j.text()` |
| `std.time` | UTC on the Gregorian calendar: `Date(year, month, day)`, `DateTime(year, month, day, hour, minute, second, millisecond)`, `enum Weekday`; `utc(seconds)` (`utc(time())` is now), `dateOfDays(n)`, `isLeapYear`, `daysInMonth`; `d.days()`, `d.weekday()`, `d.addDays(n)`, `d.addMonths(n)`, `d.at(hour, minute, second, millisecond)`, `d.isValid()`, `dt.date()`, `dt.seconds()`, `dt.addSeconds(s)`, `dt.addDays(n)`; `<` and `sort`; ISO 8601: `toIso()`, `parseDate(text) -> Date!`, `parseDateTime(text) -> DateTime!` (with seconds, fractions, `Z` or an offset turned to UTC) |
| `std.regex` | `regex(pattern) -> Regex!`: literals, `.`, `[a-z]`, `[^...]`, `\d \w \s \D \W \S \b \B`, `^ $`, `* + ? {n} {n,} {n,m}` (and lazy, with `?` after), `\|`, groups `( )` and `(?: )`; `r.matches(text)` (all of it), `r.find(text) -> Match?`, `r.findAll(text)`, `r.replaceAll(text, "$1")`, `r.replaceEach(text, m => ...)`, `r.split(text)`; `Match(start, end, text, groups)`, in bytes. Linear in the text times the pattern, never exponential |
| `std.collections` | values that share what they hold: `Queue<T>` (a deque: `Queue.Empty`, `queueOf(xs)`, `pushBack`, `pushFront`, `front`, `back`, `popFront`, `popBack`, `size`, `isEmpty`, `toArray`, each end O(1)) and `Heap<T>` (a priority queue, smallest by `<` on top: `Heap.Empty`, `heapOf(xs)`, `push`, `top`, `pop`, `merge`, `size`, `isEmpty`, `toArray` sorted; O(log n)); `Keyed(key, value)` orders a heap by a key |
| `std.path` | `/` and `\` both: `joinPath(a, b)`, `normalize(path)`, `parent`, `fileName`, `extension`, `stem`, `withExtension(path, ext)`, `pathParts`, `isAbsolute`, `windowsPath` |
| `std.testing` | `assertEqual(actual, expected, what: ...)` shows both values (of any type print shows) when they differ, `assertNotEqual`, `assertNear(a, b, tolerance: ...)`, `assertErr(result, part: ...)` |

## Not yet

- Small arrays (`[Byte]`, `[Float32]`), SIMD for `map`, `fold` across threads.
- Tasks on WebAssembly (`spawn`, `step`, `yield`, `wait`), and loops that give memory back there.
- Windows on ARM (`quark object` and the backend's Windows ARM64 calls).
