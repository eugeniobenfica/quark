#!/usr/bin/env bash
# A debug adapter session: launch tests/debug/demo.qk, stop at a breakpoint in a loop, step over, move the
# breakpoints, stop in a call, read the calls, a frame's names and a value, step out and in, run to the end,
# disconnect. Prints the adapter's messages one per line (the JSON only).
#   tests/dap/session.sh path/to/quark.exe
Q=${1:-build/quark.exe}
send() { printf 'Content-Length: %d\r\n\r\n%s' "${#1}" "$1"; }
prog=tests/debug/demo.qk
{
    send '{"seq": 1, "type": "request", "command": "initialize", "arguments": {"adapterID": "quark", "linesStartAt1": true}}'
    send '{"seq": 2, "type": "request", "command": "launch", "arguments": {"program": "'$prog'"}}'
    send '{"seq": 3, "type": "request", "command": "setBreakpoints", "arguments": {"source": {"path": "'$prog'"}, "breakpoints": [{"line": 13}]}}'
    send '{"seq": 4, "type": "request", "command": "configurationDone"}'
    # stopped at 13, in main's loop (i = 0)
    send '{"seq": 5, "type": "request", "command": "threads"}'
    send '{"seq": 6, "type": "request", "command": "stackTrace", "arguments": {"threadId": 1}}'
    send '{"seq": 7, "type": "request", "command": "next", "arguments": {"threadId": 1}}'
    # stopped at 13 again (i = 1)
    send '{"seq": 8, "type": "request", "command": "variables", "arguments": {"variablesReference": 1}}'
    send '{"seq": 9, "type": "request", "command": "setBreakpoints", "arguments": {"source": {"path": "'$prog'"}, "breakpoints": [{"line": 6}]}}'
    send '{"seq": 10, "type": "request", "command": "continue", "arguments": {"threadId": 1}}'
    # stopped at 6, in scale
    send '{"seq": 11, "type": "request", "command": "stackTrace", "arguments": {"threadId": 1}}'
    send '{"seq": 12, "type": "request", "command": "scopes", "arguments": {"frameId": 2}}'
    send '{"seq": 13, "type": "request", "command": "variables", "arguments": {"variablesReference": 2}}'
    send '{"seq": 14, "type": "request", "command": "evaluate", "arguments": {"expression": "total", "frameId": 1}}'
    send '{"seq": 15, "type": "request", "command": "evaluate", "arguments": {"expression": "nothing", "frameId": 2}}'
    send '{"seq": 16, "type": "request", "command": "stepOut", "arguments": {"threadId": 1}}'
    # stopped at 16, back in main
    send '{"seq": 17, "type": "request", "command": "stackTrace", "arguments": {"threadId": 1}}'
    send '{"seq": 18, "type": "request", "command": "stepIn", "arguments": {"threadId": 1}}'
    # stopped at 17
    send '{"seq": 19, "type": "request", "command": "continue", "arguments": {"threadId": 1}}'
    # the program prints and ends
    send '{"seq": 20, "type": "request", "command": "disconnect"}'
} > /tmp/dapin.bin; "$Q" dap < /tmp/dapin.bin | tr -d '\r' | sed 's/Content-Length: [0-9]*//' | grep -v '^$'
