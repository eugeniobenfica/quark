# prelude

## compiler/prelude.qk

The built-ins written in Quark: declarations every program sees (after its own: a program's name hides one of
these), kept in the compiler (compiler/embed.sh). A built-in here is a function like any other: its values have
names, defaults and types, its errors are a call's, a.f(b) calls it. What only the runtime can do is one call,
runtime(group, name, values...): the runtime's function `name` (compiler/ir.qk's runtimeNames, qk_<name> in
compiler/runtime/) with the values' slots in order, giving what the function says it gives. group is the
capability a caller needs (quark run --allow, qkVmCapabilities), "" none. runtimeLine gives the runtime the line
of the program's call too, last, for its messages.

### files whole, and folders (runtime/files.qk)

#### fn readFile

```quark
pub effect fn readFile(path: Text) -> Text!
```

the file's text; Err says why it could not be read (not UTF-8 too)

#### fn writeFile

```quark
pub effect fn writeFile(path: Text, text: Text) -> Unit!
```

the file made, or written over; Err says why it could not be written

#### fn appendFile

```quark
pub effect fn appendFile(path: Text, text: Text) -> Unit!
```

the text at the end of the file (made when missing)

#### fn readBytes

```quark
pub effect fn readBytes(path: Text) -> Bytes!
```

#### fn writeBytes

```quark
pub effect fn writeBytes(path: Text, bytes: Bytes) -> Unit!
```

#### fn pathExists

```quark
pub effect fn pathExists(path: Text) -> Bool
```

a file or a folder is there

#### fn listDir

```quark
pub effect fn listDir(path: Text) -> [Text]!
```

the names in a folder

#### fn makeDir

```quark
pub effect fn makeDir(path: Text) -> Unit!
```

a folder made, and the folders above it that are missing

#### fn removePath

```quark
pub effect fn removePath(path: Text) -> Unit!
```

a file removed, or a folder that is empty

### the process and what is around it

#### type Process

```quark
pub type Process { status: Int, out: Text, err: Text }
```

what a program that ran gave: its exit status, what it wrote to its output and to its errors

#### fn runProcess

```quark
pub effect fn runProcess(program: Text, args: [Text]) -> Process!
```

program run to its end with args; Err says why it could not be started

#### type Subprocess

```quark
pub type Subprocess { handle: Int }
```

a program running beside this one, its input, output and errors in pipes (startProcess)

#### fn startProcess

```quark
pub effect fn startProcess(program: Text, args: [Text]) -> Subprocess!
```

program started with args: what it reads, writes and leaves are this program's to give and take as it runs
(writeProcess, readProcess, readProcessErrors, closeProcessInput, waitProcess, killProcess). A read, a write or the
wait that would wait gives a task's turn back.

#### fn writeProcess

```quark
pub effect fn writeProcess(p: Subprocess, bytes: Bytes) -> Unit!
```

bytes to the process's input (every one: the task waits while the pipe is full)

#### fn closeProcessInput

```quark
pub effect fn closeProcessInput(p: Subprocess)
```

the process's input ended: it reads its end (a program that reads to the end of its input goes on)

#### fn readProcess

```quark
pub effect fn readProcess(p: Subprocess, most: Int) -> Bytes!
```

up to most bytes of what the process wrote to its output, as they come; none: it ended its output

#### fn readProcessErrors

```quark
pub effect fn readProcessErrors(p: Subprocess, most: Int) -> Bytes!
```

the same of its errors

#### fn waitProcess

```quark
pub effect fn waitProcess(p: Subprocess) -> Int!
```

its end waited for: its exit status (its input closed first; what it still writes is not read)

#### fn killProcess

```quark
pub effect fn killProcess(p: Subprocess)
```

stopped now (waitProcess then gives the status it stopped with)

#### fn env

```quark
pub effect fn env(name: Text) -> Text?
```

an environment variable's value

#### fn args

```quark
pub effect fn args() -> [Text]
```

the program's arguments (quark run file.qk -- a b; a built executable's)

#### fn exit

```quark
pub effect fn exit(code: Int) -> Never
```

the program ends, with this status

#### fn readLine

```quark
pub effect fn readLine() -> Text?
```

a line of the input; None at its end

### files a part at a time (runtime/io.qk)

#### type File

```quark
pub type File { handle: Int }
```

an open file: C's stream

#### fn openFile

```quark
pub effect fn openFile(path: Text, mode: Text) -> File!
```

path opened: "r" read, "w" written from nothing, "a" added to, "r+" read and written, "w+" both from nothing (its
bytes as they are)

#### fn readChunk

```quark
pub effect fn readChunk(file: File, most: Int) -> Bytes!
```

up to most bytes from where the file is; none at its end

#### fn writeChunk

```quark
pub effect fn writeChunk(file: File, bytes: Bytes) -> Unit!
```

every byte, where the file is

#### fn seekFile

```quark
pub effect fn seekFile(file: File, at: Int, from: Int = 0) -> Int!
```

the file's place moved to `at` bytes from the start (from: 0), from where it is (1) or from its end (2): the
place it is now

#### fn closeFile

```quark
pub effect fn closeFile(file: File)
```

written out and closed

### sockets: TCP (runtime/io.qk); a read, write or accept that would wait gives the task's turn back

#### type Socket

```quark
pub type Socket { handle: Int }
```

#### fn listen

```quark
pub effect fn listen(host: Text, port: Int) -> Socket!
```

a socket that listens on host:port ("0.0.0.0": every address of this machine)

#### fn connect

```quark
pub effect fn connect(host: Text, port: Int) -> Socket!
```

a connection to host:port

#### fn accept

```quark
pub effect fn accept(listener: Socket) -> Socket!
```

the next connection to a listening socket

#### fn readSocket

```quark
pub effect fn readSocket(socket: Socket, most: Int) -> Bytes!
```

what came, up to most bytes; none when the other end closed it

#### fn writeSocket

```quark
pub effect fn writeSocket(socket: Socket, bytes: Bytes) -> Unit!
```

every byte sent

#### fn closeSocket

```quark
pub effect fn closeSocket(socket: Socket)
```

#### type Tls

```quark
pub type Tls { handle: Int }
```

TLS over a connected socket, by the system's library (Schannel, OpenSSL, SecureTransport): the server's certificate
checked for serverName by the system's trust (std.net's connectTls and std.http's https:// use it)

#### fn tlsConnect

```quark
pub effect fn tlsConnect(socket: Socket, serverName: Text) -> Tls!
```

the handshake with the server at the other end of the socket

#### fn readTls

```quark
pub effect fn readTls(t: Tls, most: Int) -> Bytes!
```

what the server said, decrypted, up to most bytes; none when it ended the connection

#### fn writeTls

```quark
pub effect fn writeTls(t: Tls, bytes: Bytes) -> Unit!
```

every byte sent, encrypted

#### fn closeTls

```quark
pub effect fn closeTls(t: Tls)
```

the end sent, and the socket closed

### tasks: functions run a step at a time (runtime/tasks.qk)

#### fn spawn

```quark
pub effect fn spawn(f: effect () -> Unit) -> Task
```

a task of f: step runs it until it gives its turn back

#### fn step

```quark
pub effect fn step(task: Task) -> Bool
```

the task run until it yields or sleeps: true while it has more to do

#### fn joinTask

```quark
pub effect fn joinTask(task: Task)
```

the task run to its end

#### fn yield

```quark
pub effect fn yield()
```

the turn back to whoever runs this task (nothing, outside one)

#### fn sleep

```quark
pub effect fn sleep(seconds: Float)
```

the turn back until so many seconds went by (outside a task: a sleep)

### tasks in the background, and channels (runtime/tasks.qk)

#### fn go

```quark
pub effect fn go(f: effect () -> Unit) -> Task
```

f as a task the background runs (runTasks, or a receive outside a task)

#### fn runTasks

```quark
pub effect fn runTasks()
```

every background task, run until all are done

#### type Channel

```quark
pub type Channel<T> { id: Int }
```

values from task to task, in the order they were sent: each a copy of its own (it goes from one task's memory
to another's). Its T is told where it goes: let results: Channel<Int> = channel()

#### fn channel

```quark
pub effect fn channel<T>() -> Channel<T>
```

#### fn send

```quark
pub effect fn send<T>(channel: Channel<T>, value: T)
```

the value at the end of the channel's queue (it never waits)

#### fn receive

```quark
pub effect fn receive<T>(channel: Channel<T>) -> T?
```

the first value sent and not yet received (it waits for one); None once the channel is closed and empty

#### fn receiveWithin

```quark
pub effect fn receiveWithin<T>(channel: Channel<T>, seconds: Float) -> T?
```

receive, waiting so many seconds at most: None when none came by then too

#### fn closeChannel

```quark
pub effect fn closeChannel<T>(channel: Channel<T>)
```

no more values go in: receive gives those left, then None (closed and empty, its memory is let go of)

#### fn cancel

```quark
pub effect fn cancel(task: Task)
```

the task stops, as a panic stops it (its defers do not run), where it next gives its turn back (now, when it is
not running); a call it waits for on a thread (runProcess, connect) ends first. Ended already: nothing.

#### type Scope

```quark
pub type Scope { id: Int }
```

A scope: the tasks started in it end before scope(...) gives its value

#### fn scope

```quark
pub effect fn scope<T>(body: effect (Scope) -> T) -> T
```

body(scope)'s value, once every task scope.start(f) made has ended (the others go on meanwhile): no task of it
outlives it. A task's panic stops the program, saying the line that started it.

#### fn start

```quark
pub effect fn start(scope: Scope, f: effect () -> Unit) -> Task
```

f as a task of the scope, in the background: scope(...) waits for it

#### fn withTimeout

```quark
pub effect fn withTimeout<T>(seconds: Float, f: effect () -> T) -> T?
```

f's value, when f ends within so many seconds; None when it does not (it is cancelled then). f runs as a task in
the background: what it waits for lets the others go on.

### the clock, and chance

#### fn monotonicTime

```quark
pub effect fn monotonicTime() -> Float
```

seconds, for timing: only differences mean something

#### fn unixTime

```quark
pub effect fn unixTime() -> Float
```

seconds since 1970-01-01T00:00:00Z

#### fn random

```quark
pub effect fn random() -> Float
```

a Float in [0, 1)

#### fn randomInt

```quark
pub effect fn randomInt(low: Int, high: Int) -> Int
```

an Int from low up to high, high not included

#### fn seedRandom

```quark
pub effect fn seedRandom(seed: Int)
```

the same numbers from here on, for the same seed

#### fn localOffset

```quark
pub effect fn localOffset(seconds: Float) -> Int
```

the local time's offset from UTC at that moment (seconds since 1970), in seconds east

