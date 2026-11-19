#!/usr/bin/env bash
# The language server asked about every place of a file: it is opened, then at a few columns of each line (every
# STEP-th) hover, definition, references, rename, signature help and completion, and the file's symbols, inlay
# hints, quick fixes and formatting. Prints the server's answers one per line (the JSON only), for comparing two
# servers (tests/bootstrap.sh's tools).
#   tests/lsp/sweep.sh path/to/quark.exe FILE [STEP]
Q=${1:-build/quark.exe}
file=$2
step=${3:-1}
export LC_ALL=C # lengths in bytes
send() { printf 'Content-Length: %d\r\n\r\n%s' "${#1}" "$1"; }
uri="file:///work/$(basename "$file")"
# the file as a JSON text
text=$(awk 'BEGIN { ORS = "" } { gsub(/\\/, "\\\\"); gsub(/"/, "\\\""); gsub(/\t/, "\\t"); gsub(/\r/, "\\r"); print $0 "\\n" }' "$file")
lines=$(wc -l < "$file")
in=$(mktemp) # this run's requests
at() { pos='"textDocument": {"uri": "'$uri'"}, "position": {"line": '$1', "character": '$2'}'; }
{
    send '{"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {}}'
    send '{"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {"uri": "'$uri'", "languageId": "quark", "version": 1, "text": "'"$text"'"}}}'
    id=1
    for ((l = 0; l < lines; l += step)); do
        for c in 4 9 13 18 26; do
            at $l $c
            send '{"jsonrpc": "2.0", "id": '$id', "method": "textDocument/hover", "params": {'"$pos"'}}'
            send '{"jsonrpc": "2.0", "id": '$id', "method": "textDocument/definition", "params": {'"$pos"'}}'
            send '{"jsonrpc": "2.0", "id": '$id', "method": "textDocument/references", "params": {'"$pos"', "context": {"includeDeclaration": true}}}'
            send '{"jsonrpc": "2.0", "id": '$id', "method": "textDocument/rename", "params": {'"$pos"', "newName": "renamed"}}'
            send '{"jsonrpc": "2.0", "id": '$id', "method": "textDocument/signatureHelp", "params": {'"$pos"'}}'
            send '{"jsonrpc": "2.0", "id": '$id', "method": "textDocument/completion", "params": {'"$pos"'}}'
            id=$((id + 1))
        done
    done
    send '{"jsonrpc": "2.0", "id": '$id', "method": "textDocument/documentSymbol", "params": {"textDocument": {"uri": "'$uri'"}}}'
    send '{"jsonrpc": "2.0", "id": '$id', "method": "textDocument/inlayHint", "params": {"textDocument": {"uri": "'$uri'"}, "range": {"start": {"line": 0, "character": 0}, "end": {"line": '$lines', "character": 0}}}}'
    send '{"jsonrpc": "2.0", "id": '$id', "method": "textDocument/codeAction", "params": {"textDocument": {"uri": "'$uri'"}, "range": {"start": {"line": 0, "character": 0}, "end": {"line": '$lines', "character": 0}}, "context": {"diagnostics": []}}}'
    send '{"jsonrpc": "2.0", "id": '$id', "method": "textDocument/formatting", "params": {"textDocument": {"uri": "'$uri'"}, "options": {"tabSize": 4}}}'
    send '{"jsonrpc": "2.0", "id": '$((id + 1))', "method": "shutdown"}'
    send '{"jsonrpc": "2.0", "method": "exit"}'
} > "$in"
"$Q" lsp < "$in" | tr -d '\r' | sed 's/Content-Length: [0-9]*//' | grep -v '^$'
rm -f "$in"
