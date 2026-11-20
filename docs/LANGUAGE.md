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
    items.push(Item("plate", .Circle(radius: unit), count: 4))
    items.push(Item("tile", .Box(2, 2), count: 50))
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
    let counts = text.lower().words().countBy(w => w)
    for (i, e) in counts.sortedBy(e => e.value, descending: true).take(5).enumerate() {
        print("{i + 1:2}. {e.key} {e.value}")
    }
}
```

## Words

**Keywords (28):**

| | |
| --- | --- |
| Declarations | `fn` `type` `interface` `enum` `const` `extern` `effect` |
| Bindings | `let` (never changes) · `var` (can change), inside functions |
| Choices | `if` `else` `match` `is` (a value against a pattern) |
| Loops | `for` `in` `while` `break` `continue` |
| Leaving | `return` · `defer` (runs when its block is left) |
| Logic | `and` `or` `not` |
| Values | `true` `false` |
| Copies | `with` |
| Files | `use` (another file's declarations) `pub` (what a file gives the files that use it) |

`Some` and `None` are the built-in optional's cases, and `Ok` and `Err` the built-in result's; they
are not keywords. `test` starts a test only at the top of a file, before a text (`test "..." { }`),
so it stays free as a name, and `self` is the first value of a type's function (`fn Vec2.length(self)`;
`var self` in one that changes it), whose type `Self` names there. `as` (after a `use` or an `extern fn`) and
`opaque` (in `pub opaque type`) are words only there.

**Names:** `camelCase` for values, functions, fields and parameters, and `PascalCase` for types,
enum cases and type parameters. Underscores are not allowed. The compiler suggests the right
spelling:

```text
let my_value = 3     error: names are camelCase or PascalCase, without underscores: write "myValue"
```

A function that changes a `var` in place is named by what it does (`push`, `pop`, `add`, `remove`); one that
gives a new value is named by what it gives (`sorted`, `reversed`, `upper`, `keys`), so `xs.sorted()` can
never be read as sorting `xs` itself.

**Literals:**
- numbers: `42`, `1_000_000`, `0xff`, `0o755`, `0b1010_0001`, `3.14`, `1e-6`;
- characters: `'a'`, `'é'`, `'\n'`, `'\u{20AC}'` are Ints, the character's code point (`'a'` is 97,
  `c - '0'` a digit's value). The escapes are a text's (below), with `\'` for the quote; `\x41` goes up to
  `\xFF`;
- text: `"text"`, with `{expressions}` inside (below) and the escapes `\n \t \r \0 \" \\ \{ \}`, `\x41` (one
  byte, `\x00` to `\x7F`, so a text stays UTF-8) and `\u{20AC}` (a code point, one to six hex digits; not
  `D800` to `DFFF`). Anything else after a backslash is an error. `quark fmt` writes a character as itself,
  and escapes only what cannot be seen: control characters (`\x1b`) and code points that show nothing
  (`\u{200B}`, `\u{FEFF}`);
- on lines of its own between `"""` and `"""`: the closing quotes' indentation is taken off every line, values
  and escapes work as in any text, and `quark fmt` puts the lines one level in from where the text starts;
- raw text: `r"C:\temp\{x}"` is the bytes written, with no escapes and no values (the braces are braces), up to
  the next `"`; `r"""` ... `"""` takes lines as `"""` does, quotes too. A raw text keeps its spelling in the
  canonical text. Patterns read best this way: `regex(r"(\d{4})-(\d{2})")`;
- `true`, `false`;
- arrays: `[1, 2, 3]` and `[]` (the empty one takes the type it goes into);
- maps: `["ana": 31, "bia": 27]` and `[:]` (the empty one takes the type it goes into).

**Comments:** `//` to the end of the line. A comment above a statement or declaration belongs to it,
and so does one at the end of its line. Both survive formatting and the trip through a graph.

**Lines:** a line break ends a statement. It does not end one inside `( )` or `[ ]`, after an
operator or a comma, or before a line that starts with `.name` (a chained call: a lowercase name),
`else`, or a binary operator: `and` `or` `is` `??` `+` `*` `/` `%` `==` `!=` `<` `<=` `>` `>=` `|` `&`
`^` `<<` `>>` (not `-`, which may start a value, nor `..`, which starts a match arm's `..60`). A line
that starts with `.Case` is a value of its own (below). So `let ready = loaded and checked` with `or forced`
on the next line is one statement; `quark fmt` joins a condition written across lines into one.

<!-- given
let text = "The fox saw the dog"
-->
```quark
let counts = text
    .words()
    .map(w => w.lower())
    .filter(w => len(w) > 2)
    .countBy(w => w)
```

**Canonical form.** `quark fmt` writes a built-in that works on a value as that value's method:
`replace(word, ",", "")` is `word.replace(",", "")`, `contains(seen, k)` is `seen.contains(k)` (`len`, `min`, `max`,
the math functions and the effects stay calls: `len(xs)`, `sqrt(x)`, `print(...)`). It drops a `toArray()` whose
values only go on to something that takes any loop (`xs.map(f).toArray().join(",")` is `xs.map(f).join(",")`: no
array is made in between). Both need the program's types, so they are made when the program checks; one that does
not is only laid out.

**Line width.** A line takes at most 120 columns where it can be broken: a call past them puts each argument on a
line of its own (a comma after each, the `)` on its own line); a function's parameters do the same; a list or map
of plain values (numbers, texts, names, cases) packs them into lines, any other one an item a line; and a chain of
`and`, `or` or `+` goes on with a line for each operand after the first, the operator first (a line that starts
with one goes on the line before). A text, a comment and a name are never broken.

**Layout.** `quark fmt` gives each program one layout. A chain of calls goes one call a line when three of
its calls take a lambda, or two do and it is long (above). An `if` with an `else` goes on one line when each
branch is one short value or one jump (`return x`, `break`, `continue`: values too, of type `Never`) and the
line fits in 70 characters: `let sign = if x < 0 { -1 } else { 1 }`, `if c { return 1 } else { return 2 }`.
Every other `if` takes its lines: one without `else` (`if done {`, `break` and `}` on three), a chain with
`else if`, and one whose branch is more than one statement, or an `if`, a `match` or a block.

## Types

| Type | What | Slots |
| --- | --- | --- |
| `Int` | 64-bit integer; `+ - *` past its limits stop the program (`+% -% *%` wrap around); `/` and `%` round down (floored) and stop on division by zero; `& \| ^ ~ << >>` work on its bits (`>>` keeps the sign; a shift count is taken modulo 64) | 1 |
| `Float` | 64-bit float | 1 |
| `Bool` | `true` / `false` | 1 |
| `Text` | text (UTF-8 bytes): joined with `+`, compared with `== < ...`, `len(t)` its bytes, sliced `t[a..b]` | 2 |
| `Bytes` | raw bytes, any values 0 to 255 (below): indexed `b[i]`, sliced, joined, compared, a loop of Ints | 2 |
| `[T]` | array of T, sliced `xs[a..b]` | 2 |
| `[K: V]` | a map from K to V, in the order the keys came; K is any value `==` compares that holds no array or map (below) | 1 |
| `T?` | a T, or nothing: `Some(x)` / `None` | 1 + T's |
| `T!` | a T, or an error with a message: `Ok(x)` / `Err("why")` | 1 + the message's 2 or T's, the larger |
| `Never` | what never gives a value: `exit(code)`, `panic(message)`; it goes wherever a value goes | 0 |
| `type Name { ... }` | a record: named fields | sum of its fields |
| `interface Name { fn f(...) -> R }` | a structural interface | one function value each |
| `Unit` | the type of the value `()` (only the value is written `()`), used for operations that return only success or failure | 0 |
| `enum Name { A, B(x: T) }` | one of named cases; a case may carry data | 1 + its largest case's data (the cases share slots) |
| `extern type Name` | a handle only the host makes (a file, a window, a connection) | 1 |
| `extern type Name { field: Int32, ... }` | a C struct: its fields of C's types, in C's layout ([C](#c)) | its fields' |
| `Pointer<T>` | an address in C's memory, of a T C lays out ([C](#c)) | 1 |
| `extern (A) -> R` | a function C calls ([C](#c)) | 1 |
| `(A, B) -> R` | a function: one a parameter takes (inlined), or one kept as a value (below) | 1 |
| `effect (A) -> R` | a function kept as a value that changes the world | 1 |
| `Pair<A, B>` | a generic record or enum, given its types (below) | its instance's |

Types are **inferred**. You write them only on function parameters and record fields. A result, a
`let`, a `var` or a constant may carry one: `let xs: [Int] = []`, `fn f(...) -> Float`.

**Conversions** that happen by themselves:
- an `Int` becomes a `Float` in arithmetic and when a Float is expected. An Int and a Float compare
  (`== != < <= > >=`) as numbers, exactly: the Int is not rounded to a Float first, so
  `9007199254740993 == 9007199254740992.0` is false and `9007199254740993 > 9007199254740992.0` is true; NaN is
  unordered (only `!=` holds). An Int pattern on a Float (`match x { 0 => ..., 1..10 => ... }`) compares the same way;
- a T becomes a `T?` wherever an optional is wanted (`return i` in a function giving `Int?`), and a
  `T!` wherever a result is wanted (`Ok(it)`);
- a `T?!` is a result whose value may be missing (a read that may find nothing, or fail): `Ok(Some(x))`,
  `Ok(None)` (the `None` takes its type from where it goes), `Err(why)`; `r?` gives the `T?`, and a match takes
  it apart (`Ok(Some(n)) => ...`, `Ok(None) => ...`, `Err(why) => ...`);
- a loop (`map`, `filter`, a range) becomes an array wherever an array is wanted.

The other way is written: `toInt` truncates, `floor`, `ceil` and `round` return an `Int`, and `toText`
makes text.

**Records** are built with the type's name. The fields go by position, then by name, and a field with a
default may be left out, as a parameter with one may:

```quark
type Point { x: Float, y: Float }
type Server { host: Text = "localhost", port: Int = 8080 }
let a = Point(1, 2)
let b = Point(x: 1, y: 2)
let c = a with { y: 5 } // a copy with some fields changed
let s = Server(port: 9000)
```

Records compare with `==` and `!=`, field by field, when every field can be compared (numbers,
Bools, texts, handles, cases, records and tuples, and arrays, maps and sets of those, boxed fields too).
`==` looks inside: two records whose arrays hold equal elements are equal, two maps or sets with the same
keys (and equal values) are equal whatever order the keys came in, and a tree compares node by node, however
deep (`Tree.Node(.Leaf(1), .Leaf(2)) == t`). Floats compare as numbers: `-0.0 == 0.0`, and NaN equals nothing,
not even itself. Cases with data compare too: the same case, and the
same data (`Shape.Circle(1) == Shape.Circle(1)`). So do optionals and results: `x == None`, `x != None`,
and `x == 3` with `x: Int?` (the 3 becomes `Some(3)`). Arrays, maps and sets compare the same way on their own
(`xs == [1, 2, 3]`). Only functions do not compare. A record has `+`, `-`, `*`, `/`,
`%` and `-x` when its type gives them, and `<`, `>`, `sorted`... when it gives `compare` (below).

**Enums** are values `Color.Red`. A case can carry data, `Shape.Circle(radius: 2)`, which a
`match` takes apart. Cases without data compare with `==`, and with `<` in the order they are
listed. `c.index()` is a case's place among its enum's cases, from 0.

**`.Case` where the enum is known.** Where a value goes into a known enum, its name may be left out:
`.Leaf`, `.Circle(radius: 1)`. That is a `let` or `var` with its type written, a function's value or
result, a `return`, an assignment, a record's field, an array element or a map's key or value of a written type, another
branch of the same `if` or `match`, the other side of `==` or `!=`, `Err(...)` in a typed result, and
the right of `??`. An optional or a result of the enum takes it too (`let g: Grade? = .A`); `Some`,
`None`, `Ok` and `Err` themselves are always written. Patterns keep their bare `Circle(r)`. A literal (a list, a
map, a tuple) that goes into a written type passes it on to what is inside it, so its `.Case`, `[]`, `[:]` and `None`
take their types there too: `tally(["a": .Red])` for a `[Text: Color]` parameter, `([], 0)` for a `([Int], Int)`
result, `rows ?? ([], None)`. Where no enum is known, the compiler asks for the full name:

<!-- given
enum Grade { A, B }
enum Tree<T> { Leaf, Node(left: Tree<T>, value: T, right: Tree<T>) }
enum Shape { Circle(radius: Float), Empty }
type Item { name: Text, shape: Shape, count: Int }
var items: [Item] = []
-->
```quark
var t: Tree<Int> = .Leaf
items.push(Item("plate", .Circle(radius: 1), 4))
fn grade(n: Int) -> Grade = match n {
    90.. => .A
    _ => .B
}
```

```text
let c = .Red     error: .Red needs an enum to go into here: write Color.Red
```

**Optionals.** `T?` is a T or nothing. `Some(x)` and `None` make one. `a ?? b` gives what `a`
holds, or `b`, which is only worked out when needed:

<!-- given
let names = ["ana", "bia"]
-->
```quark
fn find(names: [Text], wanted: Text) -> Int? {
    for (i, name) in names.enumerate() {
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

<!-- given
type Version { major: Int, minor: Int }
let input = "1.2"
-->
```quark
fn parseVersion(text: Text) -> Version! {
    let parts = text.split(".")
    if len(parts) != 2 {
        return Err("expected major.minor, found \"{text}\"")
    }
    Version(parts[0].parseInt() ?? 0, parts[1].parseInt() ?? 0)
}

match parseVersion(input) {
    Ok(v) => print("version", v.major)
    Err(why) => printErr(why)
}
```

**`Never`.** `exit(code)` and `panic(message)` never come back, so they fit wherever a value goes: a
`match` arm, an `if` branch, the right of `??`. A function whose body always ends in one gives `Never`.

<!-- given
let input = "12"
let path = "notes.txt"
-->
```quark
let n = input.parseInt() ?? panic("not a number: {input}")
let text = match readFile(path) {
    Ok(t) => t
    Err(why) => {
        printErr(why)
        exit(1)
    }
}
```

**Holes.** `_` where a value goes is a hole: a value not written yet. It is a `Never` (as `panic` is), so it fits
wherever a value goes, and a program with holes is checked, formatted and run like any other; only reaching one
stops the program, with its line: `a hole (_) was reached: this value is not written yet`. In a node graph a hole is
an empty pin (a node of kind `"hole"`), so a graph being drawn is a program at every step. A constant cannot hold one
(it is worked out when the program is compiled).

<!-- given
fn ready() -> Bool = true
-->
```quark
fn area(w: Float, h: Float) -> Float = _ // to write later
let speed: Float = if ready() { 9.5 } else { _ }
```

**`?` passes a failure on.** `a?` is what an optional or a result holds; when it holds nothing, the
function returns at once with that `None` (or that `Err`, message and all). So a function using `?`
on an optional gives an optional, and one using it on a result gives a result. In `main` and in a
test, `?` stops the program with the message and the line instead:

<!-- given
type Version { major: Int, minor: Int }
fn Version.compare(self, b: Self) -> Int = self.major - b.major
fn parseVersion(t: Text) -> Version! = Version(t.parseInt() ?? 0, 0)
-->
```quark
fn newest(texts: [Text]) -> Version! {
    var best = Version(0, 0)
    for t in texts {
        let v = parseVersion(t)? // the first Err goes back to the caller
        if v > best {
            best = v
        }
    }
    best
}
```

**`?.` reads on into what is there.** `a?.b` is `b` of what an optional or a result holds, and the rest of
the chain goes on from it; when `a` holds nothing, the whole chain is that nothing (the `None`, or the
`Err`), and the function goes on. An optional chain gives an optional (never an optional of an
optional), a result chain a result:

<!-- given
type Address { city: Text }
type User { name: Text, address: Address? }
fn find(name: Text) -> User? = None
-->
```quark
let city = find("ana")?.address?.city ?? "unknown"
let size = "42".parseInt()?.toFloat() // Ok(42.0), or the Err that parseInt gave
```

`a?` alone leaves the function; `a?.b` stays in the expression.

**Sets.** `Set<K>` holds each value once, in the order they came (K as for a map's key). A list where a
set goes makes one (`let seen: Set<Text> = ["a", "b"]`, `[]`), and `xs.toSet()` does from any loop. A `var`
set changes in place with `s.add(v)` and `s.remove(v)`; `s.contains(v)`, `len(s)`, `for v in s` and every
loop function read it; `a | b`, `a & b` and `a - b` are the union, the intersection and the difference.

**Maps.** `[K: V]` holds a V for each key, and keeps the keys in the order they came. `m[key]` is an
optional (`None` when the key is not there). A `var` map changes in place:

```quark
var ages = ["ana": 31, "bia": 27]
ages["caio"] = 40 // added
ages["ana"] = 32 // replaced
ages.remove("bia")
// in the order the keys came: ana, caio
for (name, age) in ages {
    print(name, age)
}
print(len(ages), ages["ana"] ?? 0, ages.contains("bia"), ages.keys(), ages.values())
```

A map is a loop source too: its values are `Entry` records (`e.key`, `e.value`), in the order the keys came, so every
loop function works on it: `ages.count(e => e.value > 30)`, `ages.maxBy(e => e.value)`,
`ages.sortedBy(e => e.value, descending: true)`. `countBy` makes one: `t.words().countBy(w => w)` is each
word's count. A `for` over a map whose body changes that map goes over the map as it was when the loop
began (a value, as everywhere). A map's removed keys leave room that is given back as it goes (once they are
half of what it used), so a map that keys go into and out of holds memory for the keys it has, not for all
it ever had. A map holds at most 2147483646 keys (more is a panic that says so).

**Arrays join** with `+` and **repeat** with `*`, as texts do: `xs + [x] + ys` is a new array, one after the
other, and `[0] * 256` (or `"-" * 40`) the value so many times. `xs += ys` (or `xs = xs + ys`) on a `var`, or
on a field or element of one, puts ys on xs's end in place, in the room `push` keeps, so an array built a
slice at a time costs what its length does.

**Slices.** `xs[a..b]` is the part of an array from a to b (b not included), and `t[a..b]` the part
of a text (in bytes). Either end may be left out: `xs[2..]`, `t[..3]`. A slice shares the memory it
comes from, and is checked: `xs[1..9]` of a shorter array stops the program.

**Text** is UTF-8. `s = s + t` (or `s += t`) on a `var`, or on a field or element of one, grows `s` in place:
a long text made by `+` keeps room after it, and the next piece goes there while nothing else was written
into it, so a text built a piece at a time (`out += "mov r{i % 16}, {i}\n"`) costs what its length does.
- Join with `+`, compare with `== != < <= > >=` (byte by byte), and get its length in bytes with
  `len(t)`. `len(codePoints(t))` counts its Unicode code points without building or decoding anything, and
  `codePoints(t).map(char)` gives them as texts. `codePoints(t)` is a loop of them as Ints, decoded as the loop goes (nothing
  is built: `for c in codePoints(t)`, `codePoints(t).filter(c => c > 127).count()`), and `char(n)` is the
  text of one (a panic outside Unicode: below 0, past `0x10FFFF`, or `0xD800` to `0xDFFF`).
- `t.bytes()` is its bytes (`Bytes`, below), the same memory.
- A slice `t[a..b]` is in bytes, and stops the program if an end would cut a character in two.
- `upper` and `lower` know accented Latin letters, Greek, Cyrillic and Armenian (`"ação"` becomes
  `"AÇÃO"`), and `{x:8}` pads by characters.
- Values go inside with braces: `"lap {ship.laps} at {ship.speed}"` takes any value `print` shows, written as
  `print` writes it (`"{[1, 2]}"` is `[1, 2]`, `"{p}"` is `Point(x: 1.0, y: 2.0)`), with a case's name without its enum's
  (`"{grade}"` is `A`, `"{s}"` is `Circle(radius: 2.0)`).
- A **format** after a colon: `{x:.2}` with two decimals, `{n:5}` at least five characters wide
  (spaces in front), `{x:8.3}` both.
- `x.toText()` does the same for one value.

**Bytes** are raw bytes: what a file, a network or a format holds before it is text. They are held as a text is
(an address and a length), without the UTF-8 rule, and every byte is an Int from 0 to 255.
- `t.bytes()` gives a text's bytes without copying them, and `Text(b)` gives them back as a `Text!`: `Ok`
  when they are UTF-8, else `Err` with the first byte that is not (`not UTF-8: byte 3 is not part of a
  character`). `readBytes(path)` and `writeBytes(path, b)` read and write a file's bytes, whatever they are.
- A list of Ints where Bytes go makes them: `let magic: Bytes = [0x7f, 'E', 'L', 'F']`, `var b: Bytes = []`,
  `let mixed: Bytes = b.map(x => x ^ 0x5a)`. Each one must be a byte: a constant that is not is an error,
  and one worked out while running stops the program (`256 is not a byte (0 to 255)`).
- `b[i]` (checked), `b.get(i)` (→ `Int?`), `b[a..b]` (the same memory), `len(b)`, `b + c`, and
  `== != < <= > >=` byte by byte. Bytes are a map's key and a set's value, and a record's field compares with them.
- A loop over Bytes gives Ints: `for (i, x) in b.enumerate()`, `b.map(x => x ^ 0x5a).sum()`, `b.count(x => x == '\n')`,
  `b.contains(0)`, `b.indexOf('=')`.
- `b.push(x)` on a `var` (or a field or element of one) adds a byte, `b.pop()` takes the last one off (`Int?`), and
  `b += c` joins, in place: Bytes built a
  byte at a time cost what their length does, as a text built a piece at a time does. Bytes never change
  any other way (`b[i] = x` is an error: make new ones), so `t.bytes()` and a slice share their memory safely.
- `print` shows them in hex: `Bytes[0x7f, 0x45, 0x4c, 0x46]`, `Bytes[]`.

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

**Keys.** A map's key (and a set's value) is any value `==` compares that holds no array or map: Ints, Floats,
texts, Bytes, Bools, cases with or without data, and records and tuples of those of any size, through boxes too
(`[Cell: Tile]` for a grid, `[(Text, Int): Int]`, `[Shape: Int]`, `[Float: Text]`). Keys are hashed by their
type, field by field; Ints, texts and pairs of Ints go the fastest way. A Float key follows `==`: `-0.0` and
`0.0` are one key (the first one written stays), and NaN, which equals nothing, cannot be a key: `m[x] = v`
with a NaN in `x` stops the program, and `m[x]` finds nothing.

**Results whose error is a type.** `T ! E` holds a T or an E, as in `fn parseAge(t: Text) -> Int ! ParseError`,
whose failures are `Err(.Empty)`. `?` passes an `Err` on when the function's own `Err` holds the same
type; `match` takes it apart (`Err(e) => explain(e)`). `T!` is the result whose error is a `Text` message.
`r.mapErr(e => ...)` turns one error into another: the same `Ok` value, or `Err` of what the (pure) lambda
gives, so converting is written where it happens: `let text = readFile(path).mapErr(e => .Io(e))?`.
`E` goes on the same line as `!`, with a space either side. Parentheses group a type: `(Int ! E)?` is an
optional result.

**Generic types.** A record or an enum can take type parameters: `type Pair<A, B> { first: A,
second: B }`, `enum Tree<T> { Leaf, Node(left: Tree<T>, value: T, right: Tree<T>) }`. Where it is
built, its types come from the values (`Pair(1, "one")` is a `Pair<Int, Text>`); a case without data
(`Tree.Leaf`) takes the type it goes into, as `None` does, so a `let` of one needs its type written:
`var t: Tree<Int> = .Leaf`. Each list of types gets its own instance, compiled for those types.
A generic type's functions are generic functions: `fn Pair.swap(self) -> Pair<B, A>`, with `A` and `B`
Pair's own.

## Declarations

<!-- not checked: a sketch of each declaration -->
```quark
fn name(param: Type, other: Type = default) -> R { statements; the last one is the result }
fn name(param: Type) -> R = expression // one expression
fn name<T>(xs: [T]) -> T? { ... } // generic (below)
fn name(f: (Int) -> Int, x: Int) -> Int = f(f(x)) // takes a function (below)
const name = value // a constant, worked out when compiled (below)
type Name { field: Type, ... }
interface Name { fn name(param: Type) -> R, ... }
enum Name { CaseA, CaseB(data: Type) }
extern fn name(param: Type) -> R // the host's: a C function, no body
extern effect fn name(...) // the host's, and it changes the world
extern type Name // the host's handle (a file, a window...)
extern fn name(param: Int32) -> CText in "lib" // a C library's, with C's types (below, C)
extern type Name { field: Int32, other: Float64 } // a C struct
pub extern fn name(param: Int32) -> Int32 = body // a Quark function C calls by its name
fn Type.name(self, ...) -> R // a function of a type (below)
test "what it checks" { ... } // a test, run by quark test (below)
```

- `-> R` may be left out when the body says it, except when the function returns a value early or uses
  `?`. A bare `return` in a function without `-> R` means it gives nothing, and so does a call to itself in
  a block without `-> R` (`fn walk(t: Tree) { ... walk(l) ... }`); one that calls itself and gives a
  value says its `-> R` (`fn fib(n: Int) -> Int = ...`). A block that ends with what gives nothing
  (`print(x)`) goes where `Unit` is wanted: `-> Unit`, `effect (T) -> Unit`.
- **Arguments** go by position, then by name: `report(items, title: "Today")`. A parameter with a
  default (`title: Text = "Stock"`) may be left out. Defaults are pure values, worked out where they
  are used. The built-ins take named values too, in their order: `t.split(separator: ",")`,
  `clamp(v, low: 0, high: 1)`, `xs.sorted(descending: true)`.
- **Constants** are worked out when the program is compiled, unused ones too, by running them: the compiler
  compiles the code the constants reach and runs it, so a constant gives exactly the value the same expression
  gives when the program runs. A constant is any pure expression: lambdas and every loop function, calls to the
  program's functions, sets, maps, records, cases, `with`, patterns, local `let`s and `var`s, loops, `return`,
  `break`, `continue`, `defer` and `?`. Effects, the host's functions and tasks are not (a constant never has
  effects). A failure (division by zero, an index out of bounds, overflow, `panic`), a value that holds a
  function, or more than 200 million steps (`const f30 = fib(30)` takes milliseconds; `fib(40)` is too much) is a
  compile error at the constant, with what stopped it. Use `let` for values computed at runtime. The value is put
  where the constant is used (plain numeric arrays as program data). Where nothing runs code (the language server,
  a host's `qkVm` compile) a constant is worked out where it is used instead, with the same value.

### Functions of a type, and operators

A record, an enum or an extern type can have functions of its own, named `Type.name`. The first
parameter is `self`, the value the function works on; its type is the type's, so it is not written
(for a generic type, `self` is a `Pair<A, B>`, and the function takes Pair's `A` and `B`). They are
called like any function, `a.f(b)` or `f(a, b)`, and the receiver's own function wins over a plain
function of the same name.

**`var self`** makes a function that changes the value it is called on: `fn Parser.next(var self) -> Token`.
Its body changes `self` as a `var` (`self.at += 1`, `self.items.push(x)`), and the call changes the
caller's value where it is, with no copy (a value of more than four slots goes by its address: the function
changes it there, or, when it loops over its fields, holds it in registers and stores it back as it returns; a
smaller one goes in registers both ways): `p.next()` needs `p` to be a `var`, or a field or element
of one (`st.parser.next()`, `parsers[i].next()`); on a `let` or a parameter it is an error (`next
changes its value: p must be a var`). Only `self` can be `var`, and such a function is not an effect.
In a graph its call is a statement node, as `xs.push(v)` is.

<!-- given
type Token { text: Text }
-->
```quark
type Vec2 { x: Float, y: Float }

fn Vec2.plus(self, b: Self) -> Self = Self(self.x + b.x, self.y + b.y)

fn Vec2.times(self, k: Float) -> Self = Self(self.x * k, self.y * k)

fn Vec2.length(self) -> Float = sqrt(self.x * self.x + self.y * self.y)

let speed = (Vec2(3, 4) + 2 * Vec2(1, 0)).length()

type Parser { tokens: [Token], at: Int }

fn Parser.next(var self) -> Token? {
    let t = self.tokens.get(self.at)
    self.at += 1
    t
}
```

**`Self`** names, inside a type's function (its signature and its body), the type of its `self`, with
its type parameters: `fn Keyed.compare(self, b: Self) -> Int`, `fn Queue.pushBack(self, x: T) -> Self`
(a `Queue<T>`). It builds one too, `Self(x, y)`, and names its cases, `Self.Leaf`. Outside a type's
function it is an error.

An operator on a value of such a type calls its type's function:

| Written | Calls |
| --- | --- |
| `a + b`, `a - b`, `a * b`, `a / b`, `a % b` | `T.plus(a, b)`, `T.minus`, `T.times`, `T.div`, `T.rem`, with T the type of `a` |
| `-a` | `T.negate(a)` |
| `k * a`, with `k` a number | `T.times(a, k)`: a number times a value is the value times the number |
| `a < b` (and `<= > >=`), `sorted`, `minBy`... | `T.compare(a, b)`: below 0, 0 or above 0 |
| `a += b` (and `-= *= /= %=`) | `a = a + b` |

The value's type decides: `v * 2` and `2 * v` both call `fn Vec2.times(self, k: Float)`. These
functions are inlined like any small function, so `a + b` on a `Vec2` costs two float additions. In
a graph, `a + b` stays an operator node.

<!-- given
let versions = [Version(1, 2), Version(1, 0)]
-->
```quark
type Version { major: Int, minor: Int }

fn Version.compare(self, b: Self) -> Int {
    if self.major != b.major {
        return self.major - b.major
    }
    self.minor - b.minor
}

let newest = versions.sorted()[len(versions) - 1]
```

### Generic functions

A function can take **type parameters**, written after its name: `fn largest<T>(xs: [T], ...)`. Each
use is checked with the types it is given, and gets its own copy of the function, compiled for
those types, so a generic function is as fast as one written for each type:

```quark
fn pairs<T>(xs: [T]) -> [[T]] {
    var out: [[T]] = []
    for (i, x) in xs.enumerate() {
        if i % 2 == 0 {
            out.push(xs[i..min(i + 2, len(xs))])
        }
    }
    out
}

print(pairs([1, 2, 3, 4, 5]), pairs(["a", "b"])) // [[1, 2], [3, 4], [5]] [[a, b]]
```

The types come from the values given (`pairs([1, 2])`: T is Int), as deep inside them as the parameter's type
goes: `fn heaviest<T>(xs: [(T, Int)]) -> T` called with `[("a", 2)]` has T a Text, and so do maps, sets,
optionals, results (`T ! E` tells both), generic types and functions. A type parameter is PascalCase. It may
name an interface its type must fit, `fn twice<T: Speaker>(x: T) -> Text = x.speak() + x.speak()`: a call
with a type that lacks one of the interface's functions is an error where it is called, naming what is
missing, and each instance is still compiled for its own type (no indirect calls).

### Functions that take functions

A parameter can take a function: `f: (Int) -> Int`. A call gives it a lambda or a function's name
(a built-in's too), and the parameter is called like a function. Such a function is copied into
every place that calls it, with the lambda put in, so it costs nothing and its lambdas can use the
names around them:

<!-- given
fn square(x: Int) -> Int = x * x
let words = ["a", "bb"]
-->
```quark
fn applyTwice(f: (Int) -> Int, x: Int) -> Int = f(f(x))

fn top<T>(xs: [T], n: Int, score: (T) -> Int) -> [T] =
    xs.sortedBy(x => -score(x))[..min(n, len(xs))]

let k = 10
print(applyTwice(x => x * k, 3), applyTwice(square, 2), top(words, 3, len))
```

A function given this way is passed on, called, or given to `map`, `filter`... It is never kept in
a `let` or a field: every call stays a visible node, and nothing is allocated for it.

A parameter's type says whether the function it takes may have **effects**. `f: (T) -> R` takes pure
functions only; `f: effect (T) -> R` takes any, and calling it is an effect where the lambda has one
(the lambda is put in where it is written, so its effects are its caller's, inferred as usual). The
lambda is still put in, so this costs nothing either, and it may change the `var`s around it:

<!-- given
let names = ["ana"]
let prices = [3, 4]
-->
```quark
fn each<T>(xs: [T], f: effect (T) -> Unit) {
    for x in xs {
        f(x)
    }
}

each(names, n => print(n))
var total = 0
each(prices, p => {
    total += p
})
```

The loop functions (`map`, `filter`, `fold`...) take pure functions only: their loops are fused,
reordered and run in parallel. A pure lambda changes nothing around it either: `total += x` inside a `map`
(or in a lambda given to a parameter without `effect`) is an error, `a lambda given to map is pure: it cannot
change total; use fold, or a for loop`. Only a lambda given to an `effect` parameter changes its caller's `var`s.

A function that takes functions may **call itself**, directly or through others that call it back
(`fn mapTree<T, R>(t: Tree<T>, f: (T) -> R) -> Tree<R>` calling `mapTree(l, f)`). A lambda cannot be put
into each of those calls (a copy inside each copy, without end), so such a function is given its
functions as function values (closures, below): one copy of it for each list of types, an indirect call
for each call of `f`. A generic function that calls itself with the types it was given is one copy too.
One that calls itself with new types each time (`fn deep<T>(x: T, n: Int)` calling `deep((x, x), n - 1)`)
would need a new instance for every call, `deep<Int>`, `deep<(Int, Int)>`, `deep<((Int, Int), (Int, Int))>`...:
that is an error that says so.

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
print(add2(3), compose(add2, x => x * 10)(1)) // 5 30
```

A parameter that takes a function (`f: (Int) -> Int`) is inlined with its lambda when the function
only calls it or passes it on, so it costs nothing; when the function keeps it (captures it in a
lambda, stores it, gives it back, as `compose` does) or calls itself, the compiler sees that and passes a
function value instead.

Since a kept lambda holds copies, a name it uses cannot change inside it (`count = count + 1` in it is an
error that says so): keep the state in a `var` outside and give it back, or make it a `var self` function
of a type. A lambda kept in a `let` cannot call itself by that name (the name is made by the lambda): a
function that calls itself is a `fn`.

**Interfaces.** An `interface` lists functions: any type with a function
of its own for each one goes where the interface is wanted. So one array holds values of different
types, and each call goes to its own type's function. A function that changes the world says so
(`effect fn`), since an interface has no body to infer it from; one without `->` gives nothing:

<!-- given
type Dog { name: Text }
type Cat { name: Text }
-->
```quark
interface Speaker { fn speak() -> Text }
interface Log { effect fn write(line: Text) }

fn Dog.speak(self) -> Text = "woof, I am {self.name}"

fn Cat.speak(self) -> Text = "meow"

let pets: [Speaker] = [Dog("rex"), Cat("tom")]
print(pets.map(p => p.speak()))
```

An interface value is two words, however many functions the interface lists: the address of its value's
copy, and its type's table of the interface's functions (one table for each type and interface, in the
program's data). Making one copies the value once; a call loads its function from the table and calls it
with the value; the optimizing tier (`--release`) calls the type's function directly where it knows the
type. The functions are the type's own: an interface's are never set (`s.speak = ...`, `s with { ... }`
are errors), and `print` shows an interface value as its interface's name.

A record may hold functions as data too (`type Button { label: Text, onPress: (Int) -> Int }`): that
is a record, built with its fields, never an interface.

`pub opaque type Account { balance: Int }` exports the type while keeping construction, field access,
updates and destructuring within its module. Export functions or methods to expose operations on it.

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
one name; a `pub` name that meets another in a file that sees both is an error that names the file. A
name this file does not see is an error that says where it is and what to write: `unknown type "Expr":
it is in ast.qk; add use ast`, or `square is in geometry.qk, but not pub there`.

```quark
// geometry.qk
pub type Vec2 { x: Float, y: Float }

pub fn Vec2.plus(self, b: Self) -> Self = Self(self.x + b.x, self.y + b.y)

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
edition = 1

[dependencies]
geom = { git = "https://example.org/geom.git", tag = "v1.2.0" }
util = { git = "../util", rev = "3f2a9c1e07" }
```

`edition` is the edition of the language the package is written in ([STABILITY.md](STABILITY.md)); a
compiler that does not read that edition says so. `quark get` fetches each package with the system `git` into
`.quark/pkg/<name>/`, and the packages
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

A function has **effects** when it interacts with the outside world. This includes reading changing
host state, even when the call only reads. A pure function depends on its inputs. It can still fail
or run indefinitely. The checker infers effects from bodies and uses the declared contract for
host functions and interface methods.

- a lambda given to `map`, `filter`, `fold`... never has effects, so loops can be fused, reordered and
  run in parallel (`parallelMap`, below);
- a lambda given to a function's parameter has effects only when the parameter's type says so
  (`f: effect (T) -> R`); they are its caller's (above);
- a constant or a default never has effects;
- a function value's type says whether it has effects (`effect (Text) -> Int`), and a lambda kept in
  one whose type says none has none.

`effect` is written on host functions (`extern effect fn`), interface methods and function types.
A library may also use `pub effect fn` to declare that a public function's contract permits effects.
In a node graph, a pure call is a
data node, and an effect call is an action node on the execution wire. Every node says which it is
(`"effect": true`), so an editor draws it right.

```quark
extern type Item
type Point { x: Float, y: Float }
extern effect fn position(i: Item) -> Point // reads changing state: an action node
extern effect fn moveBy(i: Item, dx: Float, dy: Float) // acts: an action node

// calls moveBy: it changes the world
fn tick(item: Item, dt: Float) {
    if position(item).x < 300 {
        moveBy(item, 120 * dt, 0)
    }
}
```

**Host declarations.** `extern fn` is a function the host provides, and `extern type` is a value
only the host makes. A script can pass it on and compare it, but never build it. A host usually gives
these as its **prelude**, which every program it compiles sees. See [EMBEDDING.md](EMBEDDING.md).
An external function can bind a different symbol with `extern effect fn readItem() -> Item as "read_item"`.
The same declarations reach any C library, with C's own types: see [C](#c).

- Small functions are inlined where they are called; that is free, and shows nowhere in the program.
  Inlining has a budget: a function goes in only when what it costs inlined (its body and what it inlines in
  turn) is small and the function it goes into has not grown too much, so code grows with the program, not
  with how deep its small functions call each other.
- A program run from the command line starts at `fn main()`. It may return `Unit`, an `Int` exit
  code, or a result of either. An `Err` is printed to standard error and ends the program with status 1.
  A program for a host (an application,
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
| `var x = value` | a value that can change: `x = ...`, `x += ...` (`-= *= /= %= &= \|= ^= <<= >>=`; `s += t` joins texts, Bytes and arrays), `xs[i] = ...`, `m[k] = ...`, `p.x = ...`, `xs.push(v)`, `xs.pop()`, `m.remove(k)`, `p.next()` (a `var self` function), and the same through any chain of fields, elements and map values: `t.syms[i].uses += 1`, `grid[i][j] = v`, `m[k].push(x)` (a key the map does not hold is a panic), `st.parser.next()` |
| `let Case(a, b) = value else { ... }` | the data of one case, or the else block, which must leave (`return`, `break`, `continue`, `exit`, `panic`) |
| `var (a, b) = value` | a pattern's names that can change (`var (lo, hi) = (0, len(xs))`), as `let (a, b) = ...` |
| `for x in source { }` | over an array, a range `a..b` (b not included) or a loop. `for (i, x) in xs.enumerate()` gives a zero-based index and value. a map's values are its `Entry` records (`for e in m`), and `for (key, value) in m` or `for Entry(key, value) in m` takes them apart. Patterns must match every element. |
| `while condition { }` | |
| `defer { ... }` | runs at block completion, `return`, failed `?` (including the implicit failure of an untyped `main` or a test), `break` and `continue`. The last one registered runs first. `panic`, runtime traps and process `exit` skip deferred blocks. |
| `break`, `continue` | |
| `return value` | |
| an expression | a call, an `if`, a `match` or an `a?` whose value is not kept |

A value that is computed and then dropped (`x + 1` alone on a line) is an error.
Use `let _ = value` for deliberate discard. A result-returning call used as a statement requires
handling, propagation with `?`, or explicit discard.

`?? return`, `?? break` and `let ... else` keep the happy path flat:

<!-- given
enum Shape { Circle(radius: Float), Empty }
-->
```quark
fn firstWord(t: Text) -> Text? {
    let w = t.words().first() ?? return None
    w[0..1].upper() + w[1..]
}

fn area(s: Shape) -> Float {
    let Circle(r) = s else {
        return 0.0
    }
    3.14159 * r * r
}
```

**`return`, `break` and `continue` are values too**, of type `Never` (as `exit` and `panic` are): they fit
wherever a value goes, and leave from there. `let n = t.parseInt() ?? return Err("not a number")`,
`let tok = next() ?? break`, `match x { 0 => 1, _ => return 5 }`. On a line of its own each is a statement.

## Expressions

Precedence, from loosest to tightest:

| | |
| --- | --- |
| `a ?? b` | the optional's (or result's) value, or b |
| `or` | |
| `and` | |
| `not` | |
| `== != < <= > >=`, `a is Pattern` | they do not chain: `a < b < c` is an error |
| `a..b` | a range of Ints |
| `+ -`, `+% -%`, and the bits' `\| ^` | |
| `* / %`, `*%`, and the bits' `& << >>` (as in Go and Swift: `a & b == c` is `(a & b) == c`) | |
| `-x`, `~x` (a number written with its sign is one value: `-5.abs()` is 5) | |
| `f(x)`, `a.f(x)`, `a.field`, `a[i]`, `a[i..j]`, `a with { ... }`, `a?`, `a?.b` | |

- **`a.f(b)` is `f(a, b)`.** Any function reads left to right this way: `xs.map(f).sum()`.
- **Int arithmetic never wraps silently.** `+ - *`, unary `-`, `abs` and `sum()` past an Int's limits stop the
  program (in a constant: an error when it is compiled). `+% -% *%` (and `+%= -%= *%=`) wrap around instead, for
  what is meant to: hashes, random generators, unsigned arithmetic.

  ```quark
  fn fnv(t: Text) -> Int {
      var h = -3750763034362895579
      for b in t.bytes() {
          h = (h ^ b) *% 1099511628211
      }
      h
  }
  ```

  The checks cost little where `--release` sees they cannot fail (`i + 1` where `i < n`, `r * 3 + 2` where
  `r < len(xs) / 3`: they are dropped). An Int loop over arrays (4 elements at a time) checks each lane, and stops
  after its vector part when one went past the limits; that costs about three times the loop's own work, so a loop
  whose speed matters and whose values cannot go past them (or may wrap) says `+% *%`.
- **`if`** and **`match`** are expressions:

  <!-- given
  let x = 3
  enum Color { Red, Green, Blue }
  let color = Color.Red
  -->
  ```quark
  let sign = if x < 0 { -1 } else { 1 }
  let name = match color {
      Red => "red"
      Green => "green"
      _ => "other"
  }
  ```

- **Patterns.** A `match` arm starts with an enum case (`Circle(r)`, `Some(x)`, `Err(why)`), an Int
  (`3`, or a character: `'q'`), a range of Ints (`80..90`, `90..`, `..0`: the end is not included; with `..=`
  it is: `'a'..='z'`, `0..=9`), a text (`"quit"`), a constant's name, a name, or `_`. A constant's name
  compares the value with the constant (`limit => ...`, `favorite => ...`: any constant `==` compares). Any
  other name takes the whole value and always matches, as `_` does (`n if n > 2 => "many: {n}"`,
  `other => ...`); inside a case it takes a field (`Pair(a, b)`). `..=` is only a pattern's: a range of values is `a..b`, as everywhere. Booleans can match
  `true` and `false`. Alternatives without bindings can share an arm, as in `Red | Green => ...`, and so can
  the patterns inside a case (`Some(' ' | '
')`, `Some('a'..='z')`): a field takes any pattern an arm does.
  Any arm may add `if condition`, which can use the arm's names:

  <!-- given
  enum Grade { A, B, C }
  -->
  ```quark
  fn describe(x: Int?) -> Text = match x {
      Some(n) if n < 0 => "negative"
      Some(n) if n == 0 => "zero"
      Some(n) => "positive {n}"
      None => "nothing"
  }

  fn grade(score: Int) -> Grade = match score {
      90.. => .A
      75..90 => .B
      _ => .C
  }

  fn size(count: Int) -> Text = match count {
      0 => "none"
      n if n > 2 => "many: {n}"
      n => "few: {n}"
  }
  ```

  A `match` on an enum, an optional or a result names every case (an arm with `if` does not count)
  or ends with `_` (or a name). A `match` on a number (Int patterns on a Float compare exactly) or a Text
  ends with `_` or a name.
- **`is`** tests a value against one pattern, a match arm's (cases, literals, ranges, tuples, a name, `_`, and
  alternatives with `|` that name nothing), and gives a Bool: `let empty = q is Empty`. The names the
  pattern takes are in reach where it held: in the block of an `if`, the body of a `while`, and to the
  right of `and` in the same condition; nowhere else.

  <!-- given
  enum Token { Ident(name: Text), Number(n: Int) }
  let tok = Token.Ident("fn")
  var stack = [1, 2]
  -->
  ```quark
  if tok is Ident(name) and name == "fn" {
      print("a function")
  }
  while stack.pop() is Some(top) {
      print(top)
  }
  ```
- **Lambdas** `x => x * 2` and `(acc, x) => acc + x` go into `map`, `filter`, `fold`, `any`,
  `all`, `first`, `count`, `sortedBy`... and into functions that take functions, and are always
  inlined. A function's name works there too: `xs.map(square)`, `words.map(len)`. Over tuples, or a map's
  entries, a lambda may take one value for each field, as `for (i, x) in` does: `xs.enumerate().map((i, x) => "{i}: {x}")`,
  `ages.filter((name, age) => age > 30)`.
- **Blocks** `{ ... }` give their last expression.

## Loops that fuse

`map` and `filter` build nothing: they add code to the loop of their source. The loop ends in
whatever consumes it: `sum`, `mean`, `fold`, `min`/`max`, `any`/`all`, `first`/`last`, `count`,
`countBy`, `contains`, `minBy`/`maxBy`, `len`, `for`, or an array when one is needed. `take(n)` and
`skip(n)` add a counter to the loop: `take` ends it once it has n, so nothing past them is worked out
(a count past the end, or below zero, is simply the end).

<!-- given
fn isPrime(n: Int) -> Bool = n > 1 and (2..n).all(d => n % d != 0)
-->
```quark
let evens = (0..1_000_000).filter(x => x % 2 == 0) // replayable pipeline
let total = evens.map(x => x * x).sum() // one machine loop, no array
let firstTen = (0..1_000_000).filter(isPrime).take(10) // stops at the tenth prime
```

A replayable pipeline over immutable inputs is evaluated by each consumer. Reusing the binding
does not force it into an array. `toArray()` explicitly materializes all elements. Sources whose
setup can run code or read mutable state are materialized at the binding so that setup keeps its
original evaluation point.

A library type can implement `fn Batch.items(self) -> [Item]` to feed its elements to `for` and
pipelines. The method supplies an array, which the pipeline can traverse and fuse normally.

Indexes are checked. The check is dropped where it cannot fail: `for i in 0..len(xs) { xs[i] }`, with
`xs` a `let` or a parameter.

## Tasks: functions that run a step at a time (coroutines)

`spawn(f)` makes a task of a function of no values (a lambda, or a function's name) and returns an
opaque `Task` handle. `step(task)` runs it until it calls `yield()` (or `sleep(seconds)`, which gives the turn back
until the time went by), and is `true` while it has more to do. Each task runs on a stack of its own,
so a task can yield from anywhere, deep inside the functions it calls. A panic in a task stops whoever
stepped it, with its message. Outside a task, `yield()` does nothing and `sleep(seconds)` sleeps.
`joinTask(task)` drives a task to completion, without a pause between its turns except while it sleeps, and
propagates its panic. A task handle can no longer
be stepped or joined after completion.

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
workflow that waits for an event. They are effects, in the capability group `tasks`. A task allocates in an
arena of its own (see [Memory](#memory)), so tasks that are alive keep no one's loops from giving memory back.

### Tasks in the background, and channels

`go(f)` makes a task of `f` that is no one's to step: the background runs it. `runTasks()` runs every background
task until all are done (those they `go` too), and a `receive` outside a task runs them until a value comes. What
a task waits for gives its turn back: `sleep`, a `receive` on an empty channel, a socket's read, write or accept
that would wait, and the calls the system makes wait: `connect` and `listen` (the name looked up, the connection
made), `runProcess`, `readLine`. Those run on a thread of their own while the task waits, so three processes run
from three tasks run at once. When every task waits, the program waits on the system (the sockets' readiness, the
soonest sleep's end), so a thousand idle connections cost no processor time; while one waits on such a call, it
looks again every few milliseconds.

A `Channel<T>` carries values of `T` from task to task, in the order they were sent: numbers, texts, records,
enums, arrays, maps, functions, other channels. Each is copied, with all it holds: a task's memory is its own.
`send` never waits; `receive` waits for a value, and gives `None` once the channel is closed and every value in
it received. A receive that could never get one (every task waits on a channel, and none is left to send) stops
the program: a deadlock. `channel()` takes no values, so its `T` is written where it goes.

```quark
type Done { worker: Int, steps: [Text] }

let results: Channel<Done> = channel()
for id in 0..4 {
    go(() => send(results, Done(id, ["read", "sum"])))
}
for _ in 0..4 {
    let done = receive(results)? // runs the workers until one sends
    print("worker {done.worker}: {done.steps}")
}
```

A channel's values wait in memory of the channels' own until they are received: emptied when every channel is,
and made again from what is waiting when it has grown to twice that.

A server runs each connection in a task of its own (`std.net`, `std.http`): each waits only on its own socket,
and the others go on meanwhile.

<!-- given
fn handle(s: Socket) = closeSocket(s)
-->

```quark
let server = listen("0.0.0.0", 8080)?
go(() => {
    while accept(server) is Ok(socket) {
        go(() => handle(socket))
    }
})
runTasks()
```

A panic in a background task stops the program with its message. Everything runs on one thread: `parallelMap` and
`parallelFold` are for the processor's other cores.

### Scopes: tasks that end before their caller goes on

`scope(body)` runs `body(s)` and then waits for every task `s.start(f)` made in it, the ones those tasks start in
it too: no task of a scope outlives it, so what a function starts is done when it returns. The tasks run at once
(and the others go on); `scope` gives `body`'s value. A panic in one stops the program, its message saying the line
that started the task. Scopes nest, inside tasks too. `go(f)`, with no scope, is for what lives as long as the
program (a server's loop).

```quark
fn fetch(name: Text) -> Text {
    sleep(0.01)
    "{name} done"
}

let results: Channel<Text> = channel()
scope(s => {
    for name in ["a", "b", "c"] {
        s.start(() => send(results, fetch(name)))
    }
})
closeChannel(results) // every task has ended: nothing more comes
```

### Cancelling, and deadlines

`go(f)` gives the task it makes, and `cancel(task)` stops it: where it next gives its turn back, as a panic stops
it (its `defer`s do not run, its memory is let go of), or at once when it is not running. A call it waits for on a
thread of its own (`runProcess`, `connect`) ends first: that call writes to the task's memory. A task that ended
already is not touched, and `step` on a cancelled task gives `false`.

`withTimeout(seconds, f)` is `f()`'s value when `f` ends within the time, and `None` when it does not: `f` runs as a
background task, cancelled then. `receiveWithin(channel, seconds)` is a `receive` that waits so long at most.

```quark
fn slow(seconds: Float) -> Text {
    sleep(seconds)
    "done"
}

print(withTimeout(0.5, () => slow(0.01))) // Some(done)
print(withTimeout(0.01, () => slow(5.0))) // None, after 0.01 s
```

A channel closed and with nothing left in it is let go of; its number from then on is a closed, empty channel's,
so `withTimeout` in a loop keeps no memory.

## Threads: parallelMap and parallelFold

`xs.parallelMap(f)` is `xs.map(f)` with the elements shared out to the processor's threads: each thread
loads its own copy of the program and calls `f` on its part. `f` is a function: a name, or a lambda
(its captured values are read by every thread; they never change). It must be pure (no effects), and give values
without functions in them: numbers, Bools, texts, Bytes, arrays, maps, cases and records of those (not closures,
not interfaces). Each result is copied out of its thread's memory as it is made, and into the caller's when every
thread is done, so a thread's memory goes when it is done. The elements are dealt out in turn (thread t takes t,
t + threads, ...), so slow elements side by side are shared. A panic in a thread stops the program (or the host's
call) with its message. With fewer elements than threads it runs on fewer.

The compiler's own optimizing tier (`--release`) runs this way: each function is a value optimized on its own, and
the functions are shared out to the threads.

```quark
fn collatz(n: Int) -> Int {
    var x = n
    var steps = 0
    while x != 1 {
        x = if x % 2 == 0 { x / 2 } else { 3 * x + 1 }
        steps += 1
    }
    steps
}

let steps = (1..200_001).parallelMap(collatz) // 5.5 times faster than map, measured on one desktop processor
```

`xs.parallelFold(start, step, combine)` is `fold` shared out the same way: each thread folds its part from
`start` with `step`, and the parts are then combined in order, from `start`, with `combine`. So `combine` must be
associative and `start` nothing to it (as `0` is to `+`), and the result is the one `fold` gives. When the
accumulator is an element (`(acc, x) => acc + x`), `combine` may be left out: `step` combines the parts. Both
functions are pure and give plain values (numbers, Bools, cases, records of those: the accumulator is copied
word by word); in a constant, `parallelFold` is a `fold`.

<!-- given
fn collatz(n: Int) -> Int = n % 7
let xs = [1, 2, 3]
-->
```quark
let longest = (1..200_001).parallelFold(0, (best, n) => max(best, collatz(n)), (a, b) => max(a, b))
let total = xs.parallelFold(0, (sum, x) => sum + x) // combine is step
```

Host functions a pure `f` calls may be called from several threads at once.

## Values, not references

Every value behaves as a copy. A `var` holds its own array (or map): storing it anywhere copies it,
and so does giving a `var` an array that came from somewhere else, so changing one never changes
another:

```quark
let a = [1, 2, 3]
var b = a
b[0] = 100 // a is still [1, 2, 3]
```

Only what is needed is copied. A `let` of a `let` shares the array, arrays passed to functions are
never copied (functions cannot change their parameters), and a freshly made array (`[0] * n`, `map`,
a literal...) goes into a `var` as it is. `xs.push(v)` and `xs += ys` grow a `var` array in place, with
room reserved, so building a list a value (or a slice) at a time is linear; `m[k] = v` changes a `var` map in place.

A `var` changed inside (`t.syms[i].uses += 1`, `m[k].push(x)`, `grid[i][j] = v`, a `var self` call)
owns every array and map in its value, all the way in, and changes them in place: what goes into it is
copied unless it was just made, and so is what comes out of it to be kept (`var b = a`, `let row =
grid[0]`, a loop's name over its rows). Changing one never changes another:

```quark
var grid = [[0] * 3] * 3
let before = grid
grid[1][1] = 5 // before is still all zeros
```

The compiler copies only when a change could be seen. A `let` that reads out of a `var` (`let ins =
self.code[k]`) shares what it reads when, for the rest of its block, nothing it holds is changed in place: no
array or map of a type it can hold is changed through that `var`, there or in a `var self` function called on it
(pushing into `self.code`, or setting one of its elements, changes nothing `ins` holds), and no function value or
`defer` takes it. And when the `var`'s place is then given a new value in the same block (`let done = self.cur`,
then `self.cur = IrFn("")`), the `let` takes what the place held: it is its own, kept anywhere with no copy.

## Memory

Memory comes from an **arena**: allocating bumps a pointer, and everything is freed at once when the
program (or, in a host, a call) ends. There is no garbage collector walking the heap and no pause.

A **loop that makes things** gets its memory back on the way. It marks the arena where it starts;
when its turns have made enough (twice what it kept and looked into the last time, at least 4 MB; what a loop
inside it gave back does not count), what the turns made that
the `var`s from outside the loop still hold is copied out, and everything else they made goes at once. A large
array or text (64 KB or more) has a block of its own and is never copied: kept where it is while the `var`s reach
it (with its room to grow), only what it holds is.
What was there before the mark stays where it is, however big: a collection costs what the loop keeps of its
own. A container from before the loop that the loop changes in place (`xs[i] = "..."`, `m[k] = v`, a push into
its room) is noted when it changes, and what it holds that the turns made is kept too (an array's elements
looked at sixteen at a time, only where it changed since the loop's mark, not all of it). Values shared by
several `var`s stay shared, and an array or text that was growing keeps its room. Loops that make nothing (the
numeric ones) have none of this, so they run as before, and neither do loops whose only making is `xs.push(v)`
into a `var` from before them that they change no other way (`xs[i] = v` aside): what such an array leaves behind
as it moves to more room is less than what it holds. A loop of three million turns that builds a text, a map
and a list each turn stays at about 10 MB, where it would need over 300 MB without it.

Collections have **generations**. What one collection kept is old to the next, which copies only what the turns
made since (and what old values were changed to hold, as above): a loop that builds something big a turn at a
time copies each piece about once, not again at every collection. When what these collections kept since the
last full one is as much as that one kept (16 MB at least), the next is full again: everything since the loop's
mark, so what the loop dropped since goes too, and a loop that runs for ever stays at a few times what its `var`s
hold. A loop's first collection comes after 4 MB, and the next ones after more (up to 16 MB) as it goes on: a loop
that runs briefly never collects, and the loop around it takes back what it left. With a memory limit (`--memory`),
these are a sixteenth of the limit at most (64 KB at least).

Each **task** allocates in an arena of its own, and what its function captured is copied there when it is
spawned (values never go from a task to anyone else). So a task that is alive, parked or stepped now and
then, does not keep anyone's loops from giving memory back, and its own loops give back theirs; its arena goes
when it ends.

## Built-in functions

| | |
| --- | --- |
| Math | `sqrt sin cos tan asin acos atan atan2(y, x) exp log pow(x, y)`, `abs`, `floor ceil round` (→ Int), `toFloat toInt`, `min(a, b) max(a, b) clamp(value, low, high) lerp(a, b, t)` |
| Loops | `map(f)`, `filter(f)`, `take(n)`, `skip(n)`, `fold(start, (acc, x) => ...)`, `sum()`, `mean()` (→ `Float`), `min()`, `max()`, `any(f)`, `all(f)`, `first()` / `first(f)` and `last()` / `last(f)` (→ `T?`), `count(f)`, `countBy(f)` (→ `[K: Int]`), `contains(v)`, `indexOf(v)` (→ `Int?`), `minBy(f)`, `maxBy(f)`, `len()`, `enumerate()` (→ `(index, value)` tuples), `toArray()`, `toSet()` |
| Arrays | `[a, b, c]`, `[value] * n` (n of the value), `xs * n`, `[]`, `xs[i]` (checked), `xs.get(i)` (→ `T?`: `None` outside, no panic), `xs[a..b]`, `xs + ys`, `xs.push(v)` and `xs.pop()` (→ `T?`: the last value, taken off; a `var`), `sorted()`, `sortedBy(f)` (both stable; `descending: true` for the other way), `reversed()`, `parallelMap(f)`, `parallelFold(start, step, combine)` |
| Maps | `[k: v, ...]`, `[:]`, `m[k]` (→ `V?`), `m[k] = v` and `m.remove(k)` (a `var`), `len(m)`, `contains(m, k)`, `keys(m)`, `values(m)`, `for (k, v) in m`, and every loop function, over `Entry(key, value)` records; a key is any value `==` compares that holds no array or map |
| Sets | `Set<K>`, `[a, b]` and `[]` where a set goes, `xs.toSet()`, `s.add(v)` and `s.remove(v)` (a `var`), `s.contains(v)`, `len(s)`, `a \| b`, `a & b`, `a - b`, every loop function |
| Bytes | `t.bytes()`, `Text(b)` (→ `Text!`), `[1, 2, 255]` where Bytes go, `b[i]`, `b.get(i)`, `b[a..b]`, `len(b)`, `b + c`, `== <` ..., `b.push(x)` and `b.pop()` (a `var`), every loop function (over Ints) |
| Text | `"... {x} ..."`, `"{x:.2}"`, `'a'` (an Int), `r"raw"`, `+`, `== <` ..., `len(t)`, `t[a..b]`, `toText(x)`, `codePoints(t)` (a loop of Ints; `len(codePoints(t))` counts them), `char(n)`, `split(t, separator)`, `words(t)`, `lines(t)`, `trim(t)`, `upper(t)`, `lower(t)`, `contains(t, part)`, `indexOf(t, part)` (→ `Int?`), `startsWith`, `endsWith`, `replace(t, old, new)`, `t * n` (the text n times), `join(texts, separator)`, `parseInt(t)` / `parseFloat(t)` (→ `Int!` / `Float!`: `Err("not a number: \"x1\"")`, a long text cut to its first 40 characters and `…`; `parseInt(t) ?? 0`) |
| Optionals, results | `Some(x)`, `None`, `Ok(x)`, `Err(message)` or `Err(value)` in a `T ! E`, `r.mapErr(e => ...)`, `a ?? b`, `a?`, `match`, `is`, `let ... else`, `==` |
| Tuples | `(a, b)`, `t.0`, `let (q, r) = divmod(7, 2)`, as a map's key |
| Random | `random()` (a Float in [0, 1)), `randomInt(lo, hi)` (hi not included), `seedRandom(n)`: effects, group `random` |
| Tasks | `spawn(f)`, `step(task)`, `joinTask(task)`, `yield()`, `sleep(seconds)`: effects, group `tasks` |
| Background, channels | `go(f)` (f as a task the background runs, → `Task`), `runTasks()` (every such task, to its end), `channel()` (→ `Channel<T>`, its `T` written where it goes), `send(ch, value)`, `receive(ch)` (→ `T?`: `None` once closed and empty), `receiveWithin(ch, seconds)`, `closeChannel(ch)`, `cancel(task)`, `withTimeout(seconds, f)` (→ `T?`), `scope(body)` and `s.start(f)` (→ `Task`): effects, group `tasks` ([below](#tasks-in-the-background-and-channels)) |
| Files a part at a time | `openFile(path, mode)` (→ `File!`; `"r"`, `"w"`, `"a"`, `"r+"`, `"w+"`, bytes as they are), `readChunk(file, most)` (→ `Bytes!`: none at the end), `writeChunk(file, bytes)` (→ `Unit!`), `seekFile(file, at, from)` (→ `Int!`, the place now; `from` 0 the start (left out), 1 where it is, 2 the end), `closeFile(file)`: effects, group `files` |
| Sockets (TCP) | `listen(host, port)` (→ `Socket!`; host `"0.0.0.0"` every address), `accept(listener)` (→ `Socket!`), `connect(host, port)` (→ `Socket!`), `readSocket(socket, most)` (→ `Bytes!`: none when the other end closed it), `writeSocket(socket, bytes)` (→ `Unit!`, every byte), `closeSocket(socket)`: effects, group `net`; a read, write or accept waits only the task that makes it |
| Local time | `localOffset(seconds)` (→ `Int`: the system's time zone's offset from UTC at that moment, in seconds east), group `clock` |
| Enums | `c.index()` |
| Tests, failures | `assert(condition)`, `assert(condition, message)`, `panic(message)` (→ `Never`) |
| Effects | `print(a, b, ...)` and `printErr(a, b, ...)` (every value is made before the line is written), `monotonicTime()` (seconds, for timing), `unixTime()` (seconds since 1970), `readLine()` (→ `Text?`, `None` at the end of the input), `readFile(path)` (→ `Text!`: `Err` too when the file is not UTF-8), `writeFile(path, text)` (→ `Unit!`: `Err` says why it could not be written), `appendFile(path, text)` (→ `Unit!`, the file made when missing), `readBytes(path)` (→ `Bytes!`), `writeBytes(path, b)` (→ `Unit!`), `pathExists(path)` (a file or a folder is there), `listDir(path)` (→ `[Text]!`), `makeDir(path)` (→ `Unit!`: the folders above it too), `removePath(path)` (→ `Unit!`: a file, or a folder that is empty), `runProcess(program, args)` (→ `Process!`, below), `env(name)` (→ `Text?`), `args()` (→ `[Text]`: `quark run file.qk -- a b`, or the arguments of a built executable), `exit(code)` (→ `Never`) |

**Running a program.** `runProcess(program, args)` runs a program to its end and gives a `Process`, a built-in
record `Process { status: Int, out: Text, err: Text }`: its exit status, and what it wrote to standard output
and to standard error (read as it writes, so neither one fills up and stops it). The program is found as the
shell finds it (`PATH`; a `.exe` on Windows), its arguments go to it as they are (spaces and quotes too: no
shell reads them), and it reads no input. `Err` says why it could not be started. On Linux and macOS, a program
stopped by a signal has the status 128 plus the signal's number.

```quark
let r = runProcess("git", ["status", "--short"])?
if r.status != 0 {
    printErr(r.err)
}
```

## When something goes wrong

- **Before running:** every error has a place and a caret under it. Every syntax error is said at once: a statement
  that cannot be read becomes a hole and the reading goes on from the next line at its column, a declaration whose
  own lines cannot be read is left out and the reading goes on from the next one. So what could be read is still a
  program: `quark graph` writes its graph (holes where statements could not be read) even when it says errors. When a name is mistyped, it gives
  the closest one (`unknown name "totl": did you mean "total"?`, `f has no parameter "size"`). The
  rules say what to do, for example by naming the call with effects that cannot run in a pure pipeline.
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
  programs a 256 MB stack: a million calls deep works, and about two million of a function as small as
  `fn depth(n: Int) -> Int = if n == 0 { 0 } else { 1 + depth(n - 1) }`.
- **Capabilities:** `quark run --allow print,clock file.qk` lets a program reach only what is listed:
  host functions by name (`moveBy`), by prefix (`Object.*`), and the built-in groups `print`, `files`
  (reading, writing, appending, folders, files a part at a time), `process` (`runProcess`), `net` (sockets),
  `env`, `args`, `exit`, `stdin`, `clock`, `random`, `tasks` (and `go`, channels). A call to anything else is an
  error before it runs
  (`setColor is not given to this program`). A host gives each program its list with
  `qkVmCapabilities`: a third-party plugin gets less than the application's own scripts.
- **Overflow:** an Int `+ - *` (unary `-`, `abs`, `sum()`) past the limits stops the program (`an Int went past
  its limits`), in every build; `+% -% *%` wrap around. `toInt` (and `floor`, `ceil`, `round`) of a Float too big
  for an Int, or not a number, stops it too.
- **A budget:** `quark run --fuel N` stops a program after N loop steps. That is how a host keeps a
  script from hanging it.

## The node graph

`quark graph file.qk` writes the program as nodes, in Quark's own notation for them (QGN, Quark Graph Notation):
a Quark value, of the types `std.graph` gives, so a program can take a graph as it is. `quark text file.qgn` writes
the program back. JSON stays as the export (`quark graph file.qk --json`), and every tool that reads a graph reads
either; `quark graph g.qgn --json` and `quark graph g.json` say a graph again in the other notation.

<!-- not checked -->
```quark
Graph(quark: 1, decls: [
    Decl(kind: "fn", name: "area", params: [Param(id: 1, name: "s", typeName: "Shape", key: "area/s")], returns: "Float", form: "expr", result: Pin(node: 2), nodes: [
        Node(id: 4, kind: "op", key: "area/=/arm0", hash: "391c50ad5b76e648", line: 8, typeName: "Float", op: "*", inputs: [Pin(node: 5), Pin(node: 3)]),
        Node(id: 2, kind: "match", key: "area/=", line: 7, typeName: "Float", inputs: [Pin(node: 1)], arms: [Arm(pattern: "Circle", binds: [Bind(id: 3, name: "r")], value: Pin(node: 4))]),
    ]),
])
```

A JSON object is a record named by what it is (`Decl`, `Node`, `Pin`, `Param`, `Field`, `Case`, `Arm`, `Bind`, `Alt`,
`Block`, `Note`), its keys the record's fields, an array a list, `null` `None`; a key that is a Quark keyword gets a
name (`type` is `typeName`, `fn` `function`, `else` `otherwise`, `var` `mutable`, `effect` `effects`, `interface`
`isInterface`, `pub` `isPub`). Every field has a value when it is left out, so a record says only what it has. Both
ways are exact: JSON → QGN → JSON is the same graph.

| Text | Graph |
| --- | --- |
| `let x = ...`, `var x = ...`, a parameter, a `for` variable, a name in a match arm | a named node; every use of the name is a wire from it |
| `f(a, b)`, `a + b`, `a.field`, `xs[i]`, `[a, b]`, `[k: v]`, `a with { ... }`, `"... {x} ..."`, `a?` | a node with input pins |
| `12`, `"text"`, `true`, `Color.Red`, `.Red`, a constant, `None`, a function given to `map` | a value typed into a pin, not a node |
| `x => ...` | a node holding a small graph, with its parameters as named nodes |
| the statements of a block | the order of the block's list: the execution wire |
| a call that changes the world (inferred) | an action node (`effects: true`) |
| `if`, `match`, `for`, `while`, `let ... else` | nodes holding the blocks they run; a match arm's pattern and its `if` ride on the arm |
| `a is Pattern` | an `is` node: the value's pin, the pattern (and its named nodes) riding on it |
| `return x`, `break`, `continue` where a value goes (`x ?? return 0`) | a block node marked `jump`, holding that one statement |
| a constant, a default value | its own small graph |
| comments and blank lines | notes on the node they belong to |

Every node carries a `key`, a `hash`, a `line` in the canonical text, and its `type`. The `hash` is its content's
(its kind and canonical text, what it holds included; 16 hex digits), whatever its place: when a statement is inserted
above a node, its key (its place) changes and its hash does not, so two versions agree on what did not change. Two
nodes with the same content have the same hash; their keys tell them apart.
- a named node is keyed `function/name` (when a function names two, as two loops' `i`, each is keyed
  by its place too: `main/@3/i`);
- another statement is keyed by its place (`update/@3`, and `update/@3/@0` inside it);
- an input is keyed by its order (`update/at.0`).

An editor keeps node positions in a side file under those keys (`quark graph --layout pos.json` puts
them back in as `"pos"`), so moving nodes never touches the program. An entry may say the node's hash too
(`{"main/@0": {"pos": [10, 20], "hash": "a97e66e52860b109"}}`): when a statement inserted above the node changes its
key, the entry follows it by its hash, and an entry whose hash is another node's is not given to the node now at its
key. Graph → text always gives the
canonical text, so a graph edited in a node editor and a program edited as text can live in one
repository. `quark catalog` lists every node an editor can offer. Each pin also has a `"shape"`, its
type as data, so an editor can check a wire without reading type text. `map`'s `f` pin, for example:
`{"kind": "fn", "params": [{"kind": "param", "name": "T"}], "returns": {"kind": "param", "name": "U"}}`.
The kinds are `int`, `float`, `bool`, `text`, `bytes`, `any`, `never`, `nothing`, `enum` (any enum's case),
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

An editor can assign a persistent `uid` to a node in graph JSON. Keep it unique within its declaration
and reuse it when the node moves or changes. Diff and merge use it for that node. A layout entry
passed to `quark graph --layout` can be either `[x, y]` or `{"pos": [x, y], "uid": "editor-node-id"}`.

## Tools

| | |
| --- | --- |
| `quark run file.qk [-- args]` | compile to machine code in memory and run it |
| `quark build file.qk [-o out]` | a standalone executable |
| `quark build file.qk --target arm64-macos` | ... for another machine: `x86_64-windows`, `arm64-windows` (PE32+), `x86_64-linux`, `arm64-linux` (ELF; `--libc musl` for Alpine's, glibc otherwise), `x86_64-macos`, `arm64-macos` (Mach-O, signed ad hoc as Apple Silicon wants) |
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
| `quark run file.qk --profile` | where the time goes, by function (one sample a millisecond; inlined functions count in their caller): the runtime's share by its own functions, by the C functions it calls and by the program's functions that called it; each function with what it calls; and which functions make the copies values make |
| `quark build file.qk --release` (also `run`, `object`, `test`, `ir`) | the optimizing tier: SSA, range analysis (bounds checks that cannot fail dropped), loop-invariant code motion, strength reduction, devirtualized interface calls, records kept in registers, SIMD for `map` / `filter` / `sum` pipelines. A millisecond or so to compile instead of a fraction; without it, `quark run` stays the quick tier. Each function's optimized code is kept by content (in the temporary folder, a file for each program): a build that lowers a function as an earlier one did takes it instead of optimizing it again, and makes the same bytes either way (for a program of 5000 IR instructions or more, where that takes less than optimizing; `QUARK_CACHE=on` for any, `QUARK_CACHE=off` for none; `--time` says how many were kept) |
| `quark run file.qk --memory MB` (also `build`, `test`) | at most so much memory, then a panic (`the program used more memory than it is given (64 MB)`); an executable built with it keeps the limit |
| `quark graph` / `quark text` / `quark catalog` | node graphs both ways (QGN, or JSON with `--json`), and what an editor can offer (pins with typed shapes) |
| `quark diff a b [--json]` | the changes between two versions, by node key: nodes added, removed, changed (which inputs), statements moved (`.qk` or `.json`; exit 1 when they differ) |
| `quark merge base ours theirs [-o out] [--json]` | a three-way merge by node key: canonical text (or a graph, for `.json`); conflicts named by key, ours kept there (exit 1) |

**Completion and quick fixes.** After a dot the language server offers what goes there: a record's fields
and its type's functions (`p.` → `x`, `y`, `scaled`), an enum's cases (`Color.` → `Red`, `Green`), a
module's `pub` declarations (`g.` after `use geometry as g`), and for other values the built-in functions
(`xs.map`). Inside a call it offers the names of the values not given yet (`move(p, ` → `dx:`, `dy:`).
An error whose message says what to write comes with that edit as a quick fix: a snake_case name made
camelCase everywhere in the file, `!x` made `not x`, a `let` at the top of a file made `const`,
`effect fn` made `fn`, a misspelled name, field, case or parameter made the one meant ("did you mean"),
a `let` that is changed later made a `var`, and `for i, x in` made `for (i, x) in`.

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
| `std.json` | `parseJson(text) -> Json!`, `toJson(j)`, `j.field(name)`, `j.integer()`, `j.number()`, `j.text()`. Parsed numbers use `Integer` where a signed 64-bit integer is exact and `NumberText` for other JSON number spellings. `Num` constructs a number from a Float. Every escape is read (`\uXXXX` with its surrogate pairs, `\b`, `\f`, `\/`); a text in the input with no escape becomes a slice of it (no copy); `toJson` escapes control characters. An error says at which byte the input stops being JSON. |
| `std.graph` | the types of a program's graph (QGN, what `quark graph` writes): `Graph`, `Decl`, `Node`, `Pin`, `Param`, `Field`, `Case`, `Arm`, `Bind`, `Alt`, `Block`, `Note`; `const g: Graph = Graph(...)` takes one as it is |
| `std.time` | UTC on the Gregorian calendar: `Date(year, month, day)`, `DateTime(year, month, day, hour, minute, second, millisecond)`, `enum Weekday`; `utc(seconds)` (`utc(unixTime())` is now), `dateOfDays(n)`, `isLeapYear`, `daysInMonth`; `d.days()`, `d.weekday()`, `d.addDays(n)`, `d.addMonths(n)`, `d.at(hour, minute, second, millisecond)`, `d.isValid()`, `dt.date()`, `dt.seconds()`, `dt.addSeconds(s)`, `dt.addDays(n)`; `<` and `sorted`; ISO 8601: `toIso()`, `parseDate(text) -> Date!`, `parseDateTime(text) -> DateTime!` (with seconds, fractions, `Z` or an offset turned to UTC); `now()` (UTC), `local(seconds)` and `localNow()` (the system's time zone, with its daylight saving time) |
| `std.regex` | `regex(pattern) -> Regex!`: literals, `.`, `[a-z]`, `[^...]`, `\d \w \s \D \W \S \b \B`, `^ $`, `* + ? {n} {n,} {n,m}` (and lazy, with `?` after), `\|`, groups `( )` and `(?: )`; `r.matches(text)` (all of it), `r.find(text) -> Match?`, `r.findAll(text)`, `r.replaceAll(text, "$1")`, `r.replaceEach(text, m => ...)`, `r.split(text)`; `Match(start, end, text, groups)`, in bytes. Linear in the text times the pattern, never exponential |
| `std.collections` | values that share what they hold: `Queue<T>` (a deque: `Queue.Empty`, `queueOf(xs)`, `pushBack`, `pushFront`, `front`, `back`, `popFront`, `popBack`, `size`, `isEmpty`, `toArray`, each end O(1)) and `Heap<T>` (a priority queue, smallest by `<` on top: `Heap.Empty`, `heapOf(xs)`, `push`, `top`, `pop`, `merge`, `size`, `isEmpty`, `toArray` sorted; O(log n)); `Keyed(key, value)` orders a heap by a key |
| `std.path` | `/` and `\` both: `joinPath(a, b)`, `normalize(path)`, `parent`, `fileName`, `extension`, `stem`, `withExtension(path, ext)`, `pathParts`, `isAbsolute`, `windowsPath` |
| `std.encoding` | `base64(bytes)`, `base64Url(bytes)` (no padding), `fromBase64(text) -> Bytes!` (either alphabet, padded or not), `hex(bytes)`, `fromHex(text) -> Bytes!`, `percentEncode(text)`, `percentDecode(text, plus: false) -> Text!` (URLs: RFC 3986) |
| `std.hash` | `crc32(bytes, from: 0)`, `fnv1a(bytes)` (64 bits), `sha256(bytes) -> Bytes`, `sha256Hex(text)`, `hmacSha256(key, message) -> Bytes` |
| `std.args` | a command line read by what it takes: `flag(name, short:, help:)`, `option(name, short:, help:, default:)`, `parseArgs(args(), spec) -> Args!` (`--name`, `--name=value`, `--name value`, `-v`, `-vq`, `-o value`, `-ovalue`, `--` then positional), `a.has(name)`, `a.value(name) -> Text?`, `a.int(name) -> Int!`, `a.positional`; `usage(program, spec, positional:)` |
| `std.files` | on `openFile` and its kin: `lineReader(file)` (`while r.next() is Some(line)`, `r.error` after; any size, a part at a time), `copyFile(from, to) -> Unit!`, `fileSize(path) -> Int!`, `walkFiles(folder) -> [Text]!` (every file under it, at any depth, sorted) |
| `std.csv` | RFC 4180: `parseCsv(text, separator: ',') -> [[Text]]!` (quotes, `""` inside, separators and line breaks inside quotes, `\r\n`), `toCsv(rows, separator: ',')`, `csvField(text)`, `csvRecords(rows) -> [[Text: Text]]` (the first row's names as keys) |
| `std.net` | on the sockets: `conn(socket) -> Conn` and `c.readLine() -> Text?`, `c.readUntil(bytes) -> Bytes?`, `c.readBytes(n)`, `c.readAll() -> Bytes!`, `c.write(text)`, `c.writeBytes(bytes)`, `c.close()`; `c.error` says why a read stopped early |
| `std.http` | HTTP/1.1 without TLS: `serve(host, port, handler)` (each connection a task; the handler `(Request) -> Response`), `handleConnection(socket, handler)`, `Request(method, path, query, headers, body)`, `Response(status, headers, body)`, `textResponse(text, status:)`, `jsonResponse(text, status:)`, `get(url) -> Response!`, `post(url, body, contentType:)`, `request(method, url, body, headers)`, `parseUrl`, `parseQuery`; one request a connection; `https://` is an error |
| `std.machine` | what only the compiler gives the runtime written in Quark: `switchStack(save, load)`, `frame()`, `cpu()` ([C](#c)) |
| `std.testing` | `assertEqual(actual, expected, what: ...)` shows both values (of any type print shows) when they differ, `assertNotEqual`, `assertNear(a, b, tolerance: ...)`, `assertErr(result, part: ...)` |

## C

Quark calls C, and C calls Quark, with no adapter on either side and no cost beyond the call itself: a C
library's functions are declared with C's own types and called as they are, and a Quark function marked
`extern` is one C calls.

### C's types

`Int8 Int16 Int32 Int64 UInt8 UInt16 UInt32 UInt64 Float32 Float64 CText Pointer<T>` are C's types. They are
written only where Quark meets C: in an `extern fn`'s parameters and result, as an `extern type`'s fields, as
`Pointer<T>`'s `T`, and in `extern (A, B) -> R` (written anywhere). Elsewhere `Int32` is an error ("Int32 is one of C's types"). In
Quark code their values are Quark's own: every integer type is an `Int`, both float types a `Float`, `CText` a
`Text`, so there is no numeric tower and no conversion to write.

| C type | In Quark | Crossing into C | Coming back |
| --- | --- | --- | --- |
| `Int8` ... `Int64`, `UInt8` ... `UInt64` | `Int` | its low bits, as C's cast (sign- or zero-extended in the register, as C's compilers expect) | its low bits, widened (signed or not) |
| `Float32`, `Float64` | `Float` | rounded to the width | widened |
| `CText` | `Text` | a zero-ended copy, in the arena (valid until the arena's next reset) | copied from the zero-ended bytes (`NULL`: `""`) |
| `Pointer<T>` | `Pointer<T>` | the address | the address |
| `Pointer<T>?`, `CText?` | themselves | `None` is `NULL` | `NULL` is `None` |
| `extern type Name { ... }` | a record of its fields | a C struct, by value (below) | a C struct |
| `Int`, `Float`, `Bool` | themselves | `int64_t`, `double`, `bool` | themselves |

A `Text`, an array, a record or an optional of Quark's own types crosses as the host's functions take them: slot
by slot ([EMBEDDING.md](EMBEDDING.md)). A number is never `None` in C: `Int32?` is an error.

### Declarations

```quark
extern fn strlen(text: CText) -> UInt64 // the C library's
extern effect fn getenv(name: CText) -> CText? // None for NULL
extern fn cosf(x: Float32) -> Float32 in "m" // in names the library
extern fn cAbs(n: Int32) -> Int32 as "abs" // as names the symbol
extern type DivT { quot: Int32, rem: Int32 } // a C struct: C's layout
extern fn div(n: Int32, d: Int32) -> DivT
```

- **Where a function comes from.** `in "m"` is the library: `libm.so` (then `libm.so.6`) on Linux, `libm.dylib`
  on macOS, `m.dll` (then `libm.dll`) on Windows; a name with a `.` or a `/` is the file itself. `"c"` and `"m"`
  are the C library the program links: on Windows `ucrtbase.dll`, then `msvcrt.dll` (which have the math too); on
  Linux and macOS the process itself (`dlopen(NULL)`). Without `in`, a function is the host's when the host
  gives it (`qkVmHost`), else the C library's. `quark run` and `quark test` load them before the program runs, a
  built executable when it starts, and a missing one stops it there: "cannot find strlen in the C library",
  "cannot load the library m (m.dll)".
- **Effects** are as for every extern function: `extern effect fn` changes or reads the world (`malloc`,
  `getenv`, `qsort`); plain `extern fn` depends only on its inputs (`strlen`, `cosf`), and pipelines may call
  it.
- **A C struct** is `extern type` with fields of C's types (or `Int`, `Float`, `Bool`, a pointer, another such
  struct). Its fields are laid out as the platform's C compiler does (each at its natural alignment, the whole
  rounded to its largest), and in Quark it is a record: made with `DivT(7, 2)`, read with `r.quot`, compared with
  `==`, printed `DivT(quot: 3, rem: 2)`. It crosses by value as the platform's C does (below).
- **A variadic C function** ends its parameters with `...`: `extern effect fn snprintf(out: Pointer<UInt8>, size:
  UInt64, format: CText, ...) -> Int32`. A call gives the parameters, then any number of values, by position, each
  as C's variadic arguments take it: an `Int` (or a `Bool`) as `int64_t` (`%lld`, or `%d` for what fits an `int`),
  a `Float` as `double`, a `Text` as a zero-ended copy (`%s`), a `Pointer` as its address (`%p`). Each platform's
  convention is followed: Windows x64 passes a variadic Float in both registers, System V says in `al` how many
  vector registers carry values, Windows on ARM passes a Float in an integer register, and Apple's ARM64 puts every
  variadic value on the stack.
- **An array of C's types** (`[Float32]`, `[Int16]`, `[UInt8]`, `[CText]`, `[DivT]`) is C's array: a parameter of a C
  function takes a Quark array (`[Float]`, `[Int]`, `[Text]`, `[DivT]`) and C gets the address of a copy in the arena
  laid out as C's (each element narrowed or rounded as a value of that type is, a `CText` zero-ended), as C's
  `float xs[]` is a `float*`. Its length goes as C wants it, in a parameter of its own: `memcmp(a, b, 3)`. C gives
  no arrays back and passes none to Quark (a function C calls takes a `Pointer<T>` and a count).

### A library's header, bound

`quark bind header.h` reads a C header and writes its declarations as Quark's, so a library is used without writing
them by hand. It is all in the compiler, with no C compiler: a preprocessor (`#include`, `#define` with and without
parameters, `#if` and its expressions, `#` and `##`), a parser of C's declarations, and the writer.

```
quark bind sqlite3.h --prefix sqlite3_ --lib libsqlite3-0 -o sqlite.qk
```

<!-- not checked: a program beside sqlite.qk -->
```quark
use sqlite as sql

fn main() {
    print(sql.libversion() ?? "?", sql.sqliteOk)
}
```

- **Functions**: each one declared is `pub extern effect fn` (C says nothing of what a function changes, so each is
  an effect), its name camelCase and without the prefix (`sqlite3_open_v2` is `openV2`, `as "sqlite3_open_v2"`),
  `in` the library `--lib` names. A name Quark has (`step`, `abs`) keeps its prefix, or takes a `c` (`cAbs`).
- **Types**: a struct C lays out is an `extern type` with its fields (an array field of n elements is n fields:
  `rgba0` ... `rgba3`); a struct only named, a union, and one with bit fields are handles, used through
  `Pointer<Name>`. An enum is `Int32`, its values constants. `const char*` is `CText`, any other pointer a
  `Pointer<T>` (`void*` and `char*`: `Pointer<UInt8>`).
- **NULL**: C does not say which pointers may be NULL, so every pointer (a parameter, a result, a struct's field)
  is optional: a value goes in as it is, `None` is NULL, and a result is read with `??`.
- **Function pointers**: a parameter, a result or a struct's field is `(extern (A, B) -> R)?` (zlib's `zalloc`,
  SQLite's `xCallback`): a Quark function goes in, and one C gives back is called; one inside another's types is
  an address, `Pointer<UInt8>`.
- **Constants**: each `#define` that is a number or a text, and each enum value.
- **What is not written** is listed at the end, in comments, with the reason: a `va_list`, a union by value, a
  variable of the library.
- `-I dir` adds a folder for `#include`, `-D NAME=value` a macro. The system's own headers are not read: what they
  give (`size_t`, `int32_t`, `FILE`, `INT_MAX`...) is known. C's `long` is 32 bits on Windows and 64 elsewhere, so
  the declarations are one target's (`--target x86_64-linux`; else this machine's).

zlib, SQLite, SDL2, libcurl and libpng's headers are read this way (88, 297, 831, 95 and 276 functions, each `quark check`s).

### Pointers

`Pointer<T>` is an address in C's memory, of a `T` C lays out: one of C's types, `Int`, `Float`, `Bool`, a
`Pointer`, or an extern type (`Pointer<Window>` of a handle type is passed on, never read). It is unsafe by
nature: every operation through it is an effect, and it never holds Quark's memory past the call that made it.

```quark
extern effect fn malloc(size: UInt64) -> Pointer<UInt8>?
extern effect fn free(p: Pointer<UInt8>)

fn main() {
    let p = malloc(16) ?? panic("out of memory")
    p[0] = 'h' // one T written: its low bits, as C's p[0] = 'h'
    p[1] = 'i'
    let next = p + 1 // the next T, as C's p + 1
    let word = Pointer<UInt16>(p) // the same address, another T
    print(Text(p, 2), Bytes(p, 2), next[0], word[0], next.address - p.address)
    free(p)
}
```

- `p[i]` reads the i-th `T` (as wide as C's, widened to an `Int` or a `Float`), and `p[i] = v` writes it
  (`+= -= *= /=` too). Of an extern type, `p[i]` is the whole struct, `p[i].field` one field, and
  `p[i].field = v` writes it in place.
- `p + n` and `p - n` move by `n` Ts. `Pointer<U>(p)` is the same address as another type, `Pointer<T>(n)` the
  address an `Int` gives, and `p.address` the address as an `Int`.
- `Text(p, n)` and `Bytes(p, n)` copy n bytes into Quark's memory; `Text(p, -1)` reads up to the zero.
- Pointers compare with `==` (by address), are map keys, and print as `Pointer(0x1f2e40)`; those three are not
  effects.

### Functions C calls

```quark
extern effect fn qsort(
    base: Pointer<Int64>,
    count: UInt64,
    size: UInt64,
    compare: extern (Pointer<Int64>, Pointer<Int64>) -> Int32,
)

fn compareInts(a: Pointer<Int64>, b: Pointer<Int64>) -> Int = a[0] - b[0]

pub extern fn add(a: Int32, b: Int32) -> Int32 = a + b // C calls it by its name

fn sort(numbers: Pointer<Int64>, count: Int) {
    qsort(numbers, count, 8, compareInts)
    qsort(numbers, count, 8, (a, b) => b[0] - a[0]) // and back: a lambda that keeps nothing
}
```

- `extern (A, B) -> R` is the type of a C function value (one machine word: its address). A function of
  the program goes there by its name, or a lambda that uses nothing around it (C gets only an address: "a C
  function value keeps nothing"). Its parameters and result are Quark's, as C's types are (`Pointer<Int64>`,
  `Int` for `Int32`). The compiler makes it a C entry for each such use: code that takes C's arguments where the
  platform puts them, makes them Quark's, calls the function in Quark's frame, and gives its result back as C's
  (narrowed, rounded, a struct laid out). It may have effects: C called it.
- `extern fn name(...) -> R = body` (or `{ body }`) is a Quark function with C's types in its signature: C calls
  it (through a C function value, or by its name), and Quark calls it as any function, with Quark's values. With
  `pub`, `quark object` exports it under its name (or the one `as "symbol"` gives), a symbol C links against. The
  C program calls `qkModuleStatic` once first (the object's table of addresses, its host functions), then calls
  the function directly. A function C calls with no Quark call around it allocates in the machine's arena (a
  `CText` it gives lives until that arena's next reset), and a panic in it ends the process with its message; one
  called back from C inside a Quark call (qsort's comparison) belongs to that call: a panic stops the call, the
  C frames between included (C's code there holds nothing of its own).

### C function values

A C function value is kept, passed on and called as any value, wherever C's code gave it: an extern fn's result,
an `extern type`'s field, a variable, a Quark function's parameter.

```quark
extern effect fn loadLibrary(name: CText) -> Pointer<UInt8>? in "kernel32" as "LoadLibraryA"
extern effect fn getProcAddress(module: Pointer<UInt8>, name: CText) -> (extern (Int32) -> Int32)? in "kernel32" as "GetProcAddress"

extern type Hooks { before: (extern (Int32) -> Int32)?, name: CText? } // a field C may leave NULL: None

// f is a value: one address, no copy of sumWith per function given
fn sumWith(f: extern (Int32) -> Int32, xs: [Int]) -> Int {
    var total = 0
    for x in xs {
        total += f(x)
    }
    total
}

fn main() {
    let crt = loadLibrary("ucrtbase.dll") ?? panic("no C library")
    let abs = getProcAddress(crt, "abs") ?? panic("no abs")
    print(abs(-7), sumWith(abs, [-1, 2, -3]), getProcAddress(crt, "missing") == None)
}
```

- `f(a, b)` calls the address the platform's C way, its values made C's types as an extern fn's are (narrowed,
  rounded, a struct laid out), its result Quark's again. It is an effect: C's code does what it does.
- `ops.apply(x)` calls a struct's field (the field comes before a function `apply` of the program's).
- `(extern (A) -> R)?` is NULL when `None`, compared with `==`; `Some(compare)` of a Quark function is its C entry.
- `-> Unit` and no result are the same: C's `void`.

### Calling conventions

A call into C, and C's call into Quark, follow the platform: Windows x64 (`rcx rdx r8 r9` / `xmm0-3` by
position, 32 bytes of shadow space), System V (`rdi rsi rdx rcx r8 r9` and `xmm0-7`, each counted on its own) and
AAPCS64 (`x0-x7`, `d0-d7`). A struct goes as C's does:

| | Argument | Result |
| --- | --- | --- |
| Windows x64 | 1, 2, 4 or 8 bytes: in an integer register; else the address of a copy | 1, 2, 4 or 8 bytes: in `rax`; else through a hidden pointer (the first argument) |
| System V | up to 16 bytes: each 8 bytes in a register of its class (`xmm` when all floats, else an integer one); more: its bytes on the stack | up to 16 bytes: `rax`, `rdx` / `xmm0`, `xmm1` by class; more: through a hidden pointer (`rdi`) |
| AAPCS64 | one to four floats (or doubles) of one width: one `d`/`s` register each; up to 16 bytes: `x` registers; more: the address of a copy | the same registers; more: through `x8` |

`quark run --abi sysv` (Windows' test of the System V code) calls the runtime through adapters but no C library:
a program that calls C runs natively there.

### std.machine

The runtime written in Quark needs five things only the compiler can give, as functions of the module
`std.machine` (`use std.machine as machine`): code of the backend's own, called as Quark functions are.

- `machine.switchStack(save: Pointer<Int64>, load: Pointer<Int64>)` saves the stack pointer and the registers a
  function keeps across calls into `save`, loads them from `load`, and returns where `load` was saved: on x86-64
  29 words, `rsp rbp rbx rsi rdi r12 r13 r14 r15` then `xmm6`-`xmm15` (two words each); on ARM64 21, `sp x29 x30
  x19`-`x28` then `d8`-`d15`. The saved stack pointer is where the call returns from (x86-64: the word holding
  the return address); a fresh stack is started by a `load` whose stack pointer points at the address to start
  at (x86-64, 16-aligned plus 8) or whose `x30` holds it (ARM64, `sp` 16-aligned).
- `machine.frame() -> Pointer<UInt8>` is the calling function's frame address (`rbp`, `x29`: the saved frame
  address, then the return address), where a panic walks the calls from; an inlined function's is its caller's.
- `machine.cpu() -> Int` is the processor's features as bits: 1 AVX (and the system saves its registers), 2 AVX2,
  4 SSE4.2; 0 on ARM64.
- `machine.call(f: Pointer<UInt8>, a, b, c, d) -> Int` calls the C function at `f` with four integer arguments, the
  platform's way, and gives its integer result: how the runtime enters loaded code (its trampoline) and leaves it.
- `machine.address(t: Text) -> Int` is where a text's bytes are, with no code: how the runtime keeps its one word of
  its own in its data (a text literal is in the data once, however many places use it).

An `extern fn` whose symbol a `pub extern fn` of the same program defines (the runtime's `osCtx`, declared in one
file and given by `windows.qk`, `linux.qk` or `macos.qk`) is called the Quark way, and inlined when it is small: no
C call between them.

`quark object --linked` compiles the runtime itself: every address its table holds is the linker's to fill (a C
function by its symbol, a runtime function by `qk_<name>`, the data by its place), so the object links into an
executable with no `qkModuleStatic`; its loops never collect (the runtime is the arena).

## Not yet

- SIMD outside `--release`.
- libquark (the C API) on macOS.
