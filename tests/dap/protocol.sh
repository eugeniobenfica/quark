#!/usr/bin/env bash
# The debug adapter's protocol where no program runs: initialize, breakpoints set and given anew, a launch with no
# program, of a file that is not there, of a program that does not compile (its errors as output), the requests
# that need a running program refused or answered empty, an unknown request, disconnect. Prints the adapter's
# messages one per line (the JSON only).
#   tests/dap/protocol.sh path/to/quark.exe
Q=${1:-build/quark.exe}
send() { printf 'Content-Length: %d\r\n\r\n%s' "${#1}" "$1"; }
work=$(mktemp -d -p . dap.XXXXXX) # beside the tests: a path every quark can read
bad="$work/bad.qk"
printf 'fn main() {\n    let total = 1 + true\n    print(totl)\n}\n' > "$bad"
{
    send '{"seq": 1, "type": "request", "command": "initialize", "arguments": {"adapterID": "quark", "linesStartAt1": true}}'
    send '{"seq": 2, "type": "request", "command": "setBreakpoints", "arguments": {"source": {"path": "tests/debug/demo.qk"}, "breakpoints": [{"line": 13}, {"line": 15}]}}'
    send '{"seq": 3, "type": "request", "command": "setBreakpoints", "arguments": {"source": {"path": "tests\\debug\\demo.qk"}, "breakpoints": [{"line": 6}]}}'
    send '{"seq": 4, "type": "request", "command": "setBreakpoints", "arguments": {"source": {"path": "other.qk"}, "breakpoints": []}}'
    send '{"seq": 5, "type": "request", "command": "setExceptionBreakpoints", "arguments": {"filters": []}}'
    send '{"seq": 6, "type": "request", "command": "launch", "arguments": {}}'
    send '{"seq": 7, "type": "request", "command": "launch", "arguments": {"program": "tests/debug/missing.qk"}}'
    send '{"seq": 8, "type": "request", "command": "launch", "arguments": {"program": "'"$bad"'", "args": ["a", "b"]}}'
    send '{"seq": 9, "type": "request", "command": "configurationDone"}'
    send '{"seq": 10, "type": "request", "command": "threads"}'
    send '{"seq": 11, "type": "request", "command": "stackTrace", "arguments": {"threadId": 1}}'
    send '{"seq": 12, "type": "request", "command": "scopes", "arguments": {"frameId": 3}}'
    send '{"seq": 13, "type": "request", "command": "variables", "arguments": {"variablesReference": 1}}'
    send '{"seq": 14, "type": "request", "command": "evaluate", "arguments": {"expression": "  total"}}'
    send '{"seq": 15, "type": "request", "command": "continue", "arguments": {"threadId": 1}}'
    send '{"seq": 16, "type": "request", "command": "next", "arguments": {"threadId": 1}}'
    send '{"seq": 17, "type": "request", "command": "pause", "arguments": {"threadId": 1}}'
    send '{"seq": 18, "type": "request", "command": "restart"}'
    send '{"seq": 19, "type": "request", "command": "disconnect"}'
    send '{"seq": 20, "type": "request", "command": "threads"}'
} > "$work/in.bin"
"$Q" dap < "$work/in.bin" | tr -d '\r' | sed 's/Content-Length: [0-9]*//' | grep -v '^$' | sed "s|$work/||g"
echo "exit ${PIPESTATUS[0]}"
rm -rf "$work"
