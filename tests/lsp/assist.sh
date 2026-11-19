#!/usr/bin/env bash
# The language server's help while writing: after a dot, a record's fields and its type's functions, an enum's
# cases; inside a call, the names of the values not given yet; then the quick fixes for errors whose messages say
# what to write (camelCase names, not, const, did you mean, a let made a var, a name other languages use). Prints the server's answers one per
# line (the JSON only).
#   tests/lsp/assist.sh path/to/quark.exe
Q=${1:-build/quark.exe}
send() { printf 'Content-Length: %d\r\n\r\n%s' "${#1}" "$1"; }
uri="file:///work/assist.qk"
start='type Point { x: Int, y: Int }\n\nenum Color { Red, Green, Blue }\n\nfn Point.scaled(self, k: Int) -> Point = Point(self.x * k, self.y * k)\n\nfn move(p: Point, dx: Int, dy: Int) -> Point = Point(p.x + dx, p.y + dy)\n\nfn main() {\n    let p = Point(1, 2)\n'
dot="$start"'    print(p.)\n}\n'
cases="$start"'    let c = Color.Re\n}\n'
call="$start"'    let q = move(p, )\n}\n'
snake='fn main() {\n    let total_count = 1\n    print(total_count)\n}\n'
parsed='let limit = 3\n\nfn main() {\n    print(limit)\n}\n'
negated='fn main() {\n    if !true {\n        print(1)\n    }\n}\n'
renamed='fn main() {
    print(length([1, 2]))
}
'
checked='type Point { x: Int, y: Int }\n\nfn main() {\n    let p = Point(1, 2)\n    let total = p.x + p.z\n    print(totl)\n    total = 2\n}\n'
open() { send '{"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {"textDocument": {"uri": "'$uri'", "version": '$1'}, "contentChanges": [{"text": "'"$2"'"}]}}'; }
complete() { send '{"jsonrpc": "2.0", "id": '$1', "method": "textDocument/completion", "params": {"textDocument": {"uri": "'$uri'"}, "position": {"line": '$2', "character": '$3'}}}'; }
fixes() { send '{"jsonrpc": "2.0", "id": '$1', "method": "textDocument/codeAction", "params": {"textDocument": {"uri": "'$uri'"}, "range": {"start": {"line": 0, "character": 0}, "end": {"line": 9, "character": 0}}, "context": {"diagnostics": []}}}'; }
{
    send '{"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}}'
    send '{"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {"uri": "'$uri'", "languageId": "quark", "version": 1, "text": "'"$dot"'"}}}'
    complete 2 10 12
    open 2 "$cases"
    complete 3 10 20
    open 3 "$call"
    complete 4 10 20
    open 4 "$snake"
    fixes 5
    open 5 "$parsed"
    fixes 6
    open 6 "$negated"
    fixes 7
    open 7 "$checked"
    fixes 8
    open 8 "$renamed"
    fixes 9
    send '{"jsonrpc": "2.0", "id": 10, "method": "shutdown"}'
    send '{"jsonrpc": "2.0", "method": "exit"}'
} > /tmp/lspassist.bin
# the answers, without the diagnostics (session.sh has those) and the names every completion offers
"$Q" lsp < /tmp/lspassist.bin | tr -d '\r' | sed 's/Content-Length: [0-9]*//' | grep -v '^$' | grep -v publishDiagnostics |
    sed 's/, {"label": "[a-zA-Z0-9]*", "kind": \(14\|3\|22\|13\)}//g'
