# Quark for people who know Go, Rust or Python

What carries over, what does not, and the Quark way of the things you do every day. Each part stands alone: read
the one for the language you know. [LANGUAGE.md](LANGUAGE.md) says everything exactly; [TUTORIAL.md](TUTORIAL.md)
starts from nothing.

The short version, whichever you come from: every value is a value (no pointers, no references, no aliasing),
errors are results (`T!`) and missing values are optionals (`T?`), interfaces are structural, functions that touch
the world are known to (effects), tasks are cheap and nothing is coloured `async`, memory is arenas with no
garbage collector and no borrow checker, and there is one canonical way to write each program (`quark fmt`).

## Coming from Go

Much will feel familiar: a small language, a compiler that is fast, structural interfaces, `defer`, tasks and
channels, one formatter, and packages pinned in a lock with minimal version selection, as Go modules do.

**Values, not pointers.** There is no `*T` and no `&x`. A record, an array or a map is a value: storing it or
passing it never shares it, so a function cannot change what it was given and two variables never see each
other's changes. What changes is a `var`, in place, and the compiler copies only when a change could be seen. A
method that changes its receiver says so: `fn Counter.add(var self, n: Int)`.

**Errors are results.** `T!` is a `T` or an `Err` with a message; `?` returns the error at once, where Go writes
`if err != nil { return err }`. `??` gives a value instead of the error. There is no `nil`: a value that may be
missing is `T?`, and the compiler makes you look before you use it.

```quark
type Account { owner: Text, balance: Int }

fn withdraw(a: Account, amount: Int) -> Account! {
    if amount > a.balance {
        return Err("{a.owner} has {a.balance}, not {amount}")
    }
    a with { balance: a.balance - amount }
}

fn main() -> Unit! {
    let after = withdraw(Account("ana", 100), 30)?
    print(after.balance) // 70
}
```

**Goroutines are tasks.** `go(f)` starts one, `Channel<T>` carries values between them, and `runTasks()` runs them
all. A blocked read, `sleep` or `receive` gives the turn back, so a thousand idle connections cost nothing. There is
no `sync.WaitGroup`: `scope(s => ...)` waits for every task started in it, as `errgroup` does, so nothing a
function starts outlives it. `withTimeout(seconds, f)` and `receiveWithin` stand for `select` with a timer.

```quark
fn main() {
    let squares: Channel<Int> = channel()
    scope(s => {
        for n in [1, 2, 3] {
            s.start(() => send(squares, n * n))
        }
    })
    closeChannel(squares)
    var total = 0
    while receive(squares) is Some(v) {
        total += v
    }
    print(total) // 14
}
```

Tasks share no memory: what goes into a channel is copied, so there are no data races to find. For the
processor's other cores, `xs.parallelMap(f)` and `parallelFold` run pure functions on threads.

**Interfaces are Go's, plus defaults and bounds.** A type fits an interface when it has its functions; nothing
says `implements`. An interface's function may have a default body, and an interface can bound a generic function
(`fn earliest<T: Ordered>(a: T, b: T) -> T`).

**What is not there:** `nil`, pointers, `panic`/`recover` as control flow (a panic stops the program, or in a host
the call), `init()` functions, package-level mutable state (a `const` is computed when compiling, once), struct
tags (typed JSON is made from the type itself: `encode(v)`, `let p: Person = decode(text)?`).

## Coming from Rust

You keep sum types, `match`, `?`, generics with bounds, no null, no exceptions, and a compiler that finds the
mistakes. You lose the borrow checker and lifetimes, because there are no references to check.

**Ownership without borrows.** Every value behaves as a copy, so there is nothing to borrow and nothing to
outlive. The compiler shares where it can and copies where a change could be seen, which is what you would have
written by hand with `Rc` and `clone()`, without writing it. A `var` is `let mut`; a function that changes its
receiver takes `var self` (`&mut self`); nothing else can change what it is given.

**`Result` and `Option`.** `T!` is `Result<T, String>`: the error is a message, which is what most programs do
with errors in the end. `T?` is `Option<T>`. `?` works on both; `??` is `unwrap_or`; `let Some(x) = v else {
return }` is `let ... else`. A failed `?` in `main` prints the error and exits with status 1.

```quark
enum Token { Number(value: Int), Plus, Minus }

fn evaluate(tokens: [Token]) -> Int {
    var total = 0
    var sign = 1
    for t in tokens {
        match t {
            Number(v) => {
                total += sign * v
            }
            Plus => {
                sign = 1
            }
            Minus => {
                sign = -1
            }
        }
    }
    total
}

fn main() {
    print(evaluate([.Number(5), .Minus, .Number(2), .Plus, .Number(10)])) // 13
}
```

**Traits are interfaces.** A type fits an interface by having its functions (no `impl Trait for`); defaults and
bounds work as you expect. An interface value is a `dyn Trait`: two words, its value and its type's table. A
generic function is made again for each type it is used with, as Rust monomorphises.

**Effects instead of `unsafe` and `async`.** The checker knows which functions touch the world (files, the
network, the clock, C) and which are pure; a lambda given to `map` or `filter` must be pure, so loops can be fused
and run in parallel. There is no `async`: any function can wait, inside a task, and the others go on.

**Memory is arenas.** Allocation is a pointer bump; a loop that makes things gives back what it no longer holds as
it goes, and a task's arena goes when it ends. There is no `Drop` to write: files and sockets are closed by their
functions (`defer closeFile(f)`).

**Cargo.** `quark.toml` names the packages, `quark get` fetches them (git, semantic versions, minimal version
selection, a SHA-256 of each in `quark.lock`); `quark test` runs `test "name" { }` blocks, with `--coverage`, and
`std.testing`'s `forAll` is `proptest`. `quark fmt` is `rustfmt` with no options. C is called directly:
`extern fn cosf(x: Float32) -> Float32 in "m"`, or a whole header with `quark bind`.

## Coming from Python

You keep short programs, a large standard library and a quick edit-run loop (`quark run` compiles to machine code
in milliseconds). You gain types the compiler checks, speed near C's, and one executable to ship.

**Types, mostly inferred.** `let` names a value that never changes, `var` one that does; a parameter says its
type, and the rest is worked out. A record is a class with fields (`type Point { x: Float, y: Float }`), and its
methods are functions of the type (`fn Point.length(self) -> Float`). There is no inheritance: an interface is
duck typing the compiler checks.

**Comprehensions are pipelines.** `xs.filter(...).map(...)` is one loop, with no lists in between. Texts are
f-strings by default: `"{name} has {count:3} items, {share:.1}%"`.

```quark
fn main() {
    let words = ["apple", "fig", "banana", "kiwi", "fig"]
    let long = words.filter(w => len(w) > 3).map(w => w.upper())
    var counts: [Text: Int] = [:]
    for w in words {
        counts[w] = (counts[w] ?? 0) + 1
    }
    print(long, counts["fig"] ?? 0) // [APPLE, BANANA, KIWI] 2
}
```

**No exceptions, no `None` surprises.** A function that can fail says so in its type (`-> Int!`), and the caller
writes `?` to pass the error on or `??` to give a value instead; `parseInt("x1") ?? 0`. A value that may be missing
is `T?`, and you cannot use it without saying what happens when it is missing.

**Async without `async`.** `go(f)` starts a task; a socket read, `sleep` or a channel's `receive` waits only that
task. There is no event loop to start and no function to mark: the same code runs in a task or outside one.

**Where things are:** `readFile`, `writeFile`, `listDir` (files and folders); `std.json` (`parseJson`, and typed
`encode`/`decode`); `std.http` (a server and a client, `https://` too); `std.time` (dates, time zones by name);
`std.regex`, `std.csv`, `std.args`, `std.path`; `runProcess` and `startProcess` for other programs. `pip` is `quark
get`; `pytest` is `quark test`, and `hypothesis` is `std.testing`'s `forAll`.
