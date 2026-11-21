#!/usr/bin/env bash
# A language server session over two files, open and never saved (the server reads neither from the disk): a
# function's effects on hover, the implementations of an interface and of its function, a name's references and its
# rename in both files, and an edit of part of one file that makes the other one wrong (its problems said again).
#   tests/lsp/files.sh path/to/quark.exe
Q=${1:-build/quark.exe}
send() { printf 'Content-Length: %d\r\n\r\n%s' "${#1}" "$1"; }
shapes="file:///work/proj/shapes.qk"
main="file:///work/proj/main.qk"
shapesText='pub interface Shape { fn area() -> Int }\n\npub type Square { side: Int }\n\npub type Rect { w: Int, h: Int }\n\npub fn Square.area(self) -> Int = self.side * self.side\n\npub fn Rect.area(self) -> Int = self.w * self.h\n\npub fn describe(s: Shape) -> Text = \"area {s.area()}\"\n\npub fn twice(s: Shape) -> Int = s.area() * 2\n'
mainText='use shapes\n\nfn show(s: Shape) {\n    print(describe(s))\n}\n\nfn main() {\n    show(Square(3))\n    let text = readFile(\"x.txt\") ?? \"\"\n    print(describe(Rect(2, 5)), len(text), twice(Square(2)))\n}\n'
at() { echo '"textDocument": {"uri": "'$1'"}, "position": {"line": '$2', "character": '$3'}'; }
{
    send '{"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}}'
    send '{"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {"uri": "'$shapes'", "languageId": "quark", "version": 1, "text": "'"$shapesText"'"}}}'
    send '{"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {"uri": "'$main'", "languageId": "quark", "version": 1, "text": "'"$mainText"'"}}}'
    send '{"jsonrpc": "2.0", "id": 2, "method": "textDocument/hover", "params": {'"$(at $main 2 4)"'}}'
    send '{"jsonrpc": "2.0", "id": 3, "method": "textDocument/hover", "params": {'"$(at $main 6 4)"'}}'
    send '{"jsonrpc": "2.0", "id": 4, "method": "textDocument/hover", "params": {'"$(at $main 3 12)"'}}'
    send '{"jsonrpc": "2.0", "id": 5, "method": "textDocument/implementation", "params": {'"$(at $main 2 12)"'}}'
    send '{"jsonrpc": "2.0", "id": 6, "method": "textDocument/implementation", "params": {'"$(at $shapes 12 35)"'}}'
    send '{"jsonrpc": "2.0", "id": 7, "method": "textDocument/implementation", "params": {'"$(at $shapes 0 26)"'}}'
    send '{"jsonrpc": "2.0", "id": 8, "method": "textDocument/references", "params": {'"$(at $main 3 12)"', "context": {"includeDeclaration": true}}}'
    send '{"jsonrpc": "2.0", "id": 9, "method": "textDocument/rename", "params": {'"$(at $main 3 12)"', "newName": "summary"}}'
    send '{"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {"textDocument": {"uri": "'$shapes'", "version": 2}, "contentChanges": [{"range": {"start": {"line": 10, "character": 7}, "end": {"line": 10, "character": 15}}, "text": "summary"}]}}'
    send '{"jsonrpc": "2.0", "id": 10, "method": "textDocument/hover", "params": {'"$(at $shapes 10 9)"'}}'
    send '{"jsonrpc": "2.0", "id": 11, "method": "shutdown"}'
    send '{"jsonrpc": "2.0", "method": "exit"}'
} > /tmp/lspfiles.bin; "$Q" lsp < /tmp/lspfiles.bin | tr -d '\r' | sed 's/Content-Length: [0-9]*//' | grep -v '^$'
