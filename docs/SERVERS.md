# Servers in Quark

How to write a network service: a handler, routes, JSON, state that requests share, calls to other services,
tests, and shipping it. Everything here is in the standard library (`std.http`, `std.net`, `std.json`); nothing
needs a framework.

## A handler is a function

`serveHttp(host, port, handler)` listens, and runs each connection in a task of its own: a slow request waits only
itself, and when every task waits the process sleeps until a socket is ready, so idle connections cost nothing. The
handler takes a `Request` (`method`, `path`, `query`, `headers` with lower-case names, `body`) and gives a
`Response`.

```quark
use std.http

fn page(r: Request) -> Response = match r.path {
    "/" => textResponse("hello")
    "/hi" => textResponse("hi, {r.query["name"] ?? "you"}")
    _ => textResponse("no page here", status: 404)
}

fn main() {
    serveHttp("0.0.0.0", 8080, page)?
}
```

Routes are a `match` on the path, with a guard for the method (`"/todos" if r.method == "POST" => ...`); a path
with a part that varies is `r.path.split("/")`, matched as an array, or tested with `startsWith`.

## JSON in and out

`std.json`'s `encodeJson(value)` writes any record, enum, array, map or optional as JSON, and `decodeJson(text)` reads one
back by the type it goes into, a field's default standing for one that is missing. An error says where it is
(`title: JSON: a text, not a number`), so it can go back to the client as it is.

```quark
use std.http
use std.json

type Todo { id: Int, title: Text, done: Bool = false }

fn added(r: Request) -> Response {
    let todo: Todo! = decodeJson(r.text())
    match todo {
        Ok(t) => jsonResponse(encodeJson(t), status: 201)
        Err(why) => textResponse("not a todo: {why}", status: 400)
    }
}

fn handle(r: Request) -> Response = match r.path {
    "/todos" if r.method == "POST" => added(r)
    "/todos" => jsonResponse(encodeJson([Todo(1, "write the guide")]))
    _ => textResponse("no such page", status: 404)
}
```

## State that requests share

Each task's memory is its own, and values are never shared, so two requests cannot race on a variable: there is
nothing to lock. State that outlives a request lives in one task, and the handlers send it what to do over a
channel, with a channel of their own for the answer. The state's task changes its `var`s one message at a time.

```quark
use std.http

enum Command { Add(title: Text, reply: Channel<Int>), Count(reply: Channel<Int>) }

// the one task that holds the titles
fn store(commands: Channel<Command>) {
    var titles: [Text] = []
    while receive(commands) is Some(c) {
        match c {
            Add(title, reply) => {
                titles.push(title)
                send(reply, len(titles))
            }
            Count(reply) => send(reply, len(titles))
        }
    }
}

fn handle(r: Request, commands: Channel<Command>) -> Response {
    let reply: Channel<Int> = channel()
    if r.method == "POST" {
        send(commands, .Add(r.text(), reply))
    } else {
        send(commands, .Count(reply))
    }
    textResponse("{receive(reply) ?? 0} titles")
}

fn main() {
    let commands: Channel<Command> = channel()
    go(() => store(commands))
    serveHttp("0.0.0.0", 8080, r => handle(r, commands))?
}
```

A database is the same shape: one task (or a few) holds the connections, the handlers ask it.

## Calling other services

`httpGet(url)`, `httpPost(url, body, contentType:)` and `httpRequest(method, url, body, headers)` are the client, for `http://`
and `https://` (TLS by the system's own library: Schannel, OpenSSL or SecureTransport, the server's certificate
checked against the system's trust). A call waits only its task. `withTimeout(seconds, f)` gives up on a slow one,
and `scope` runs several at once and waits for them all:

<!-- not checked: it calls the network -->
```quark
use std.http

fn status(url: Text) -> Text = match httpGet(url) {
    Ok(r) => "{url}: {r.status}"
    Err(why) => "{url}: {why}"
}

fn main() {
    let out: Channel<Text> = channel()
    scope(s => {
        for url in ["https://example.com/", "https://example.org/"] {
            s.start(() => send(out, withTimeout(5.0, () => status(url)) ?? "{url}: too slow"))
        }
    })
    closeChannel(out)
    while receive(out) is Some(line) {
        print(line)
    }
}
```

Below HTTP, `std.net` has the same connection over TCP (`conn(connect(host, port)?)`) and over TLS
(`connectTls(host, port)`): `readLine`, `readUntil`, `readBytes`, `write`.

## Tests

A handler is a function from a `Request` to a `Response`, so a test calls it, with no socket and no port:

<!-- given
use std.http
fn page(r: Request) -> Response = match r.path {
    "/" => textResponse("hello")
    _ => textResponse("no page here", status: 404)
}
-->
```quark
use std.testing

test "an unknown page is a 404" {
    assertEqual(page(Request("GET", "/nowhere")).status, 404)
}

test "every path gives a page" {
    forAll(texts(), p => page(Request("GET", "/" + p)).status is 200 | 404)
}
```

`quark test server.qk --coverage` says which lines of the file the tests ran.

## Shipping it

- `quark build server.qk --release -o server` makes one executable with nothing to install beside it;
  `--target x86_64-linux` (or `arm64-linux`, `x86_64-macos`...) makes it for another machine, from any machine.
- `quark run --allow net,print server.qk` runs it with only the network and printing: a call to anything else
  (`files`, `process`, `env`...) is an error before it runs.
- The server speaks plain HTTP/1.1, one request a connection: put TLS, HTTP/2 and keep-alive in the proxy in front
  of it (nginx, Caddy, a cloud's load balancer), which is where certificates are renewed anyway.
- A panic in a handler stops the program with its message and the calls it went through: let the system's
  supervisor (systemd, a container's restart policy) start it again, and fix the line it names.
