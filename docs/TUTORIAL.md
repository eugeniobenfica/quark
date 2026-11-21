# A tour of Quark

This tour goes from a first program to a small server, one idea at a time. Each example runs as it is
(`quark run file.qk`). The full reference is [LANGUAGE.md](LANGUAGE.md); what stays stable is
[STABILITY.md](STABILITY.md).

## 1. A first program

```quark
fn main() {
    let name = "world"
    print("hello, {name}")
}
```

`quark run hello.qk` compiles it to machine code and runs it; `quark build hello.qk` writes an executable,
with no C compiler and nothing else to install. `{name}` puts a value into a text; `print` writes its values
separated by spaces.

## 2. Values and names

```quark
fn main() {
    let width = 3 // a name for a value: it does not change
    var total = 0 // var: it changes
    for i in 0..width {
        total += i * 10
    }
    let ratio = toFloat(total) / 4.0
    print(total, ratio, total > 20, "-" * 8)
}
```

Types are worked out: `width` is an `Int`, `ratio` a `Float`. `0..width` is a range (the end not included).
An `Int` that goes past its limits stops the program with a message, rather than wrapping silently.

## 3. Functions

```quark
fn area(width: Float, height: Float = 1.0) -> Float = width * height

fn describe(n: Int) -> Text {
    if n == 0 {
        return "none"
    }
    "{n} of them"
}

fn main() {
    print(area(3.0), area(2.0, height: 5.0), describe(0), describe(4))
    print(4.describe()) // a.f(b) is f(a, b)
}
```

Parameters are written with their types; the result type follows `->`. A function whose body is one value can
use `=`. The last value of a block is its result.

## 4. Records, enums and match

```quark
type Point { x: Float, y: Float }

enum Shape { Circle(center: Point, radius: Float), Square(corner: Point, side: Float) }

fn Shape.area(self) -> Float = match self {
    Circle(_, r) => 3.14159 * r * r
    Square(_, s) => s * s
}

fn main() {
    let shapes = [Shape.Circle(Point(0.0, 0.0), 1.0), Shape.Square(Point(1.0, 1.0), 2.0)]
    for s in shapes {
        print(s.area())
    }
    let moved = Point(1.0, 2.0) with { x: 5.0 }
    print(moved)
}
```

A `match` must cover every case: forgetting one is an error before the program runs. Values are values: `with`
makes a changed copy, and nothing another name holds changes under you.

## 5. Missing values and errors

```quark
fn half(n: Int) -> Int? = if n % 2 == 0 { n / 2 } else { None }

fn parsePort(text: Text) -> Int! {
    let n = text.parseInt()? // an Err goes back to the caller here
    if n < 1 or n > 65535 {
        return Err("{n} is not a port")
    }
    Ok(n)
}

fn main() {
    print(half(10), half(3), half(3) ?? 0)
    print(parsePort("8080"), parsePort("70000"), parsePort("http"))
    if parsePort("x") is Err(why) {
        print("no:", why)
    }
}
```

`T?` is a value or `None`; `T!` is a value or an error. `?` passes an error up, `??` gives a value in its
place. There is no null and no exception: what can fail says so in its type.

## 6. Collections and loops

```quark
fn main() {
    let words = "the quick brown fox jumps over the lazy dog the end".words()
    let counts = words.countBy(w => w)
    let long = words.filter(w => len(w) > 4).map(w => w.upper()).toArray()
    print(counts["the"] ?? 0, long, words.sorted().take(3).toArray())
    var seen: Set<Text> = []
    for w in words {
        seen.add(w)
    }
    print(len(seen), (1..6).map(n => n * n).sum())
}
```

Arrays, maps (`[Text: Int]`) and sets come with the loop functions (`map`, `filter`, `sorted`, `countBy`,
`sum`...). A chain of them runs as one loop, without the arrays in between.

## 7. Files, modules and the standard library

```quark
use std.json
use std.files

fn main() {
    let path = (env("TEMP") ?? "/tmp") + "/tour.txt"
    writeFile(path, "one\ntwo\nthree\n")?
    var lines = lineReader(openFile(path, "r")?)
    var n = 0
    while lines.next() is Some(line) {
        n += 1
        print(n, line)
    }
    lines.close()
    let data = parseJson("\{\"name\": \"Quark\", \"tags\": [\"fast\", \"small\"]\}")?
    print(data.field("name")?.text() ?? "?")
}
```

A file of your own is a module: `use shapes` reads `shapes.qk` beside it, and its `pub` names are yours.
The standard library is `std.*`: `json`, `files`, `net`, `http`, `time`, `regex`, `math`, `collections`,
`encoding`, `hash`, `args`, `csv`, `path`, `testing`.

## 8. Tasks: many things at once

```quark
fn worker(id: Int, out: Channel<Text>) {
    for step in 0..2 {
        send(out, "worker {id}, step {step}")
        sleep(0.01)
    }
}

fn main() {
    let out: Channel<Text> = channel()
    for id in 0..3 {
        go(() => worker(id, out))
    }
    var got: [Text] = []
    for _ in 0..6 {
        got.push(receive(out) ?? "?")
    }
    print(got.sorted())
}
```

`go(f)` starts a task; tasks take turns on one thread, and one that waits (a `sleep`, a `receive`, a socket)
lets the others run. A `Channel<T>` carries values of `T` between them (`T` written where the
channel goes: `let out: Channel<Text> = channel()`). For the processor's other cores,
`xs.parallelMap(f)` shares the work out to threads.

## 9. A server

```quark
use std.http

fn page(r: Request) -> Response = match r.path {
    "/" => textResponse("hello from Quark")
    "/hi" => textResponse("hi, {r.query["name"] ?? "you"}")
    _ => textResponse("no page here", status: 404)
}

fn main() {
    serve("127.0.0.1", 8080, page)?
}
```

Each connection runs in a task of its own: a slow page does not hold up the others, and when every task waits
the process sleeps until a socket is ready.

## 10. C, and the graph

Quark calls C directly. `quark bind header.h` writes the declarations for a C library's header:

```quark
extern effect fn labs(n: Int32) -> Int32 // C's own, from the C library
extern effect fn strlen(s: CText) -> UInt64

fn main() {
    print(labs(-7), strlen("quark"))
}
```

And every program is also a graph of nodes: `quark graph file.qk` writes it, `quark text file.qgn` writes the
program back, and `quark diff` and `quark merge` compare and combine versions by node, not by line.

## Where to go next

- [LANGUAGE.md](LANGUAGE.md): the whole language, the built-ins, the standard library, C, memory.
- [COMING-FROM.md](COMING-FROM.md): Quark for people who know Go, Rust or Python.
- [SERVERS.md](SERVERS.md): network services: handlers, JSON, shared state, tests, shipping.
- [PRELUDE.md](PRELUDE.md): the prelude's built-ins, made from its source by `quark doc`.
- [EMBEDDING.md](EMBEDDING.md): Quark inside a C or C++ program.
- `quark fmt --write`: the one way each thing is written; `quark check`: errors only; `quark test`: the
  `test "..." { }` blocks of a file; `quark lsp`: an editor's language server.
