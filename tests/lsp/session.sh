#!/usr/bin/env bash
# A language server session: open a file with an error, fix it, then hover, go to definition, list symbols,
# find references, rename, show inferred types, help with a call's signature, format, complete, shut down. Prints the server's answers one per line (the JSON only).
#   tests/lsp/session.sh path/to/quark.exe
Q=${1:-build/quark.exe}
send() { printf 'Content-Length: %d\r\n\r\n%s' "${#1}" "$1"; }
uri="file:///work/demo.qk"
bad='fn main() {\n    let total = 1 + true\n    print(total)\n}\n'
good='fn square(x: Int) -> Int = x * x\n\nfn main() {\n    let total = square(4)\n    print(total)\n}\n'
messy='fn main()   {\n  print( 1+2 )\n}\n'
{
    send '{"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}}'
    send '{"jsonrpc": "2.0", "method": "initialized", "params": {}}'
    send '{"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {"uri": "'$uri'", "languageId": "quark", "version": 1, "text": "'"$bad"'"}}}'
    send '{"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {"textDocument": {"uri": "'$uri'", "version": 2}, "contentChanges": [{"text": "'"$good"'"}]}}'
    send '{"jsonrpc": "2.0", "id": 2, "method": "textDocument/hover", "params": {"textDocument": {"uri": "'$uri'"}, "position": {"line": 4, "character": 11}}}'
    send '{"jsonrpc": "2.0", "id": 3, "method": "textDocument/hover", "params": {"textDocument": {"uri": "'$uri'"}, "position": {"line": 3, "character": 18}}}'
    send '{"jsonrpc": "2.0", "id": 4, "method": "textDocument/definition", "params": {"textDocument": {"uri": "'$uri'"}, "position": {"line": 3, "character": 18}}}'
    send '{"jsonrpc": "2.0", "id": 5, "method": "textDocument/documentSymbol", "params": {"textDocument": {"uri": "'$uri'"}}}'
    send '{"jsonrpc": "2.0", "id": 8, "method": "textDocument/references", "params": {"textDocument": {"uri": "'$uri'"}, "position": {"line": 0, "character": 4}, "context": {"includeDeclaration": true}}}'
    send '{"jsonrpc": "2.0", "id": 9, "method": "textDocument/rename", "params": {"textDocument": {"uri": "'$uri'"}, "position": {"line": 3, "character": 9}, "newName": "sum"}}'
    send '{"jsonrpc": "2.0", "id": 10, "method": "textDocument/inlayHint", "params": {"textDocument": {"uri": "'$uri'"}, "range": {"start": {"line": 0, "character": 0}, "end": {"line": 9, "character": 0}}}}'
    send '{"jsonrpc": "2.0", "id": 11, "method": "textDocument/signatureHelp", "params": {"textDocument": {"uri": "'$uri'"}, "position": {"line": 3, "character": 23}}}'
    send '{"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {"textDocument": {"uri": "'$uri'", "version": 3}, "contentChanges": [{"text": "'"$messy"'"}]}}'
    send '{"jsonrpc": "2.0", "id": 6, "method": "textDocument/formatting", "params": {"textDocument": {"uri": "'$uri'"}, "options": {"tabSize": 4}}}'
    send '{"jsonrpc": "2.0", "id": 7, "method": "shutdown"}'
    send '{"jsonrpc": "2.0", "method": "exit"}'
} > /tmp/lspin.bin; "$Q" lsp < /tmp/lspin.bin | tr -d '\r' | sed 's/Content-Length: [0-9]*//' | grep -v '^$'
