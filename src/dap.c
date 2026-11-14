/* Quark — a debug adapter: a debugger client (VS Code, or any other that speaks the Debug Adapter Protocol) talks
 * to it over stdin and stdout. It compiles the program for debugging (every statement says its line, every name
 * its value: lower.c), runs it in this process, and where it stops (a breakpoint, a step) answers the client's
 * questions: the calls, their names and values. The program's own output goes to the client as output events;
 * the program reads no stdin (that is the client's).
 *
 * Requests: initialize, launch {program, args, stopOnEntry}, setBreakpoints (by line), configurationDone,
 * threads, stackTrace, scopes, variables, evaluate (a name), continue, next, stepIn, stepOut, disconnect.
 * The client's requests are read while the program is stopped (or before it starts, or after it ends). */
#define _DEFAULT_SOURCE
#include "qk.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <process.h>
#define dupFd _dup
#define fdOpen _fdopen
#define processId _getpid
#define NULL_DEVICE "NUL"
#else
#include <unistd.h>
#define dupFd dup
#define fdOpen fdopen
#define processId getpid
#define NULL_DEVICE "/dev/null"
#endif

static FILE* in;  /* the client's messages */
static FILE* out; /* ours */
static int seq = 1;

/* ---- messages: "Content-Length: n\r\n\r\n" and n bytes of JSON ------------------------------------- */

static char* readMessage(void) {
    char line[1024];
    long length = -1;
    for (;;) {
        if (!fgets(line, sizeof line, in)) return NULL;
        if (!strcmp(line, "\r\n") || !strcmp(line, "\n")) {
            if (length >= 0) break;
            continue;
        }
        if (!strncmp(line, "Content-Length:", 15)) length = strtol(line + 15, NULL, 10);
    }
    char* body = malloc((size_t)length + 1);
    if (fread(body, 1, (size_t)length, in) != (size_t)length) {
        free(body);
        return NULL;
    }
    body[length] = '\0';
    return body;
}

static void sendMessage(const QkBuf* body) {
    fprintf(out, "Content-Length: %zu\r\n\r\n", body->len);
    fwrite(body->data, 1, body->len, out);
    fflush(out);
}

/* {"seq": n, "type": "response", "request_seq": s, "success": true, "command": c, "body": body} */
static void respond(int requestSeq, const char* command, const char* body) {
    QkBuf b = {0};
    qkBufPrintf(&b, "{\"seq\": %d, \"type\": \"response\", \"request_seq\": %d, \"success\": true, \"command\": ", seq++, requestSeq);
    qkJsonString(&b, command);
    if (body) qkBufPrintf(&b, ", \"body\": %s", body);
    qkBufPuts(&b, "}");
    sendMessage(&b);
    qkBufFree(&b);
}

static void refuse(int requestSeq, const char* command, const char* message) {
    QkBuf b = {0};
    qkBufPrintf(&b, "{\"seq\": %d, \"type\": \"response\", \"request_seq\": %d, \"success\": false, \"command\": ", seq++, requestSeq);
    qkJsonString(&b, command);
    qkBufPuts(&b, ", \"message\": ");
    qkJsonString(&b, message);
    qkBufPuts(&b, "}");
    sendMessage(&b);
    qkBufFree(&b);
}

static void event(const char* name, const char* body) {
    QkBuf b = {0};
    qkBufPrintf(&b, "{\"seq\": %d, \"type\": \"event\", \"event\": \"%s\"", seq++, name);
    if (body) qkBufPrintf(&b, ", \"body\": %s", body);
    qkBufPuts(&b, "}");
    sendMessage(&b);
    qkBufFree(&b);
}

static void output(const char* category, const char* text, size_t n) {
    QkBuf t = {0}, b = {0};
    qkBufAdd(&t, text, n);
    qkBufPrintf(&b, "{\"category\": \"%s\", \"output\": ", category);
    qkJsonString(&b, t.data ? t.data : "");
    qkBufPuts(&b, "}");
    event("output", b.data);
    qkBufFree(&t);
    qkBufFree(&b);
}

/* ---- the program's output: stdout goes to a file, read back and sent at each stop and at the end -------- */

static char outPath[1024];
static FILE* outReader;

static void captureOutput(void) {
    const char* dir = getenv("TEMP");
    if (!dir) dir = getenv("TMPDIR");
    if (!dir) dir = "/tmp";
    snprintf(outPath, sizeof outPath, "%s/quark-dap-%d.txt", dir, (int)processId());
    fflush(stdout);
    if (!freopen(outPath, "wb", stdout)) return;
    outReader = fopen(outPath, "rb");
}

static void flushOutput(void) {
    fflush(stdout);
    if (!outReader) return;
    clearerr(outReader);
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, outReader)) > 0) output("stdout", chunk, n);
}

static void endOutput(void) {
    flushOutput();
    if (outReader) fclose(outReader);
    outReader = NULL;
    fclose(stdout);
    remove(outPath);
}

/* ---- the program ----------------------------------------------------------------------------------- */

static Program prog;
static const char* programPath;
static bool stopOnEntry;

static void compileError(const char* path, int line, int col, const char* message) {
    QkBuf b = {0};
    qkBufPrintf(&b, "%s:%d:%d: %s\n", path, line, col, message);
    output("stderr", b.data, b.len);
    qkBufFree(&b);
}

/* the file a function comes from (its frame's): the program's own, or a module it uses */
static const char* fileOf(const char* fn) {
    for (int i = 0; fn && i < prog.count; i++) {
        const Decl* d = prog.decls[i];
        if (d->kind == D_FN && !strcmp(d->name, fn)) return d->module && d->module < prog.moduleCount ? prog.modules[d->module].path : programPath;
    }
    return programPath;
}

/* the same file, however it is written: \ or /, a relative path or the whole one, the case on Windows */
static bool samePath(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    const char* longer = la >= lb ? a : b;
    const char* shorter = la >= lb ? b : a;
    size_t ls = la >= lb ? lb : la, ll = la >= lb ? la : lb;
    if (!ls) return false;
    for (size_t i = 0; i < ls; i++) {
        char x = longer[ll - ls + i], y = shorter[i];
        if (x == '\\') x = '/';
        if (y == '\\') y = '/';
#ifdef _WIN32
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
#endif
        if (x != y) return false;
    }
    if (ls == ll) return true;
    char before = longer[ll - ls - 1];
    return before == '/' || before == '\\' || shorter[0] == '/' || shorter[0] == '\\';
}

/* breakpoints by file: the runtime knows only lines, so it stops at any of them, and here the file decides */
typedef struct Break {
    char* path;
    int line;
} Break;
static Break* breaks;
static int breakCount, breakCap;

static void runtimeBreaks(void) {
    qkDebugger.breakCount = 0;
    for (int i = 0; i < breakCount; i++) {
        if (qkDebugger.breakCount == qkDebugger.breakCap) {
            qkDebugger.breakCap = qkDebugger.breakCap ? qkDebugger.breakCap * 2 : 8;
            qkDebugger.breaks = realloc(qkDebugger.breaks, sizeof(int) * (size_t)qkDebugger.breakCap);
        }
        qkDebugger.breaks[qkDebugger.breakCount++] = breaks[i].line;
    }
}

static bool breakAt(const char* path, int line) {
    for (int i = 0; i < breakCount; i++)
        if (breaks[i].line == line && samePath(breaks[i].path, path)) return true;
    return false;
}

static void setBreakpoints(int requestSeq, const J* args) {
    const J* source = qkJsonGet(args, "source");
    const J* path = qkJsonGet(source, "path");
    const J* list = qkJsonGet(args, "breakpoints");
    const char* p = path && path->kind == J_STR ? path->str : "";
    for (int i = 0; i < breakCount; i++) /* this file's breakpoints are all given anew */
        if (samePath(breaks[i].path, p)) {
            free(breaks[i].path);
            breaks[i--] = breaks[--breakCount];
        }
    QkBuf b = {0};
    qkBufPuts(&b, "{\"breakpoints\": [");
    for (int i = 0; list && list->kind == J_ARR && i < list->count; i++) {
        const J* line = qkJsonGet(list->items[i], "line");
        int at = line ? (int)line->num : 0;
        if (breakCount == breakCap) {
            breakCap = breakCap ? breakCap * 2 : 8;
            breaks = realloc(breaks, sizeof(Break) * (size_t)breakCap);
        }
        char* copy = malloc(strlen(p) + 1);
        strcpy(copy, p);
        breaks[breakCount++] = (Break){copy, at};
        qkBufPrintf(&b, "%s{\"id\": %d, \"verified\": true, \"line\": %d}", i ? ", " : "", breakCount, at);
    }
    qkBufPuts(&b, "]}");
    runtimeBreaks();
    respond(requestSeq, "setBreakpoints", b.data);
    qkBufFree(&b);
}

/* ---- stopping and stepping ------------------------------------------------------------------------- */

enum { RUN, STEP_IN, STEP_OVER, STEP_OUT, ENTRY };
static int mode = RUN;
static int modeDepth; /* the calls the program was in when the step began */
static bool ended;

/* a frame's id: its depth + 1 (the outermost is 1), so 0 means none */
static void stackTrace(int requestSeq) {
    QkBuf b = {0};
    int depth = qkDebugDepth();
    qkBufPuts(&b, "{\"stackFrames\": [");
    for (int i = depth - 1; i >= 0; i--) {
        int line = 0;
        const char* fn = qkDebugFrame(i, &line);
        const char* file = fileOf(fn);
        const char* base = file;
        for (const char* c = file; *c; c++)
            if (*c == '/' || *c == '\\') base = c + 1;
        qkBufPrintf(&b, "%s{\"id\": %d, \"name\": ", i == depth - 1 ? "" : ", ", i + 1);
        qkJsonString(&b, fn ? fn : "?");
        qkBufPuts(&b, ", \"source\": {\"name\": ");
        qkJsonString(&b, base);
        qkBufPuts(&b, ", \"path\": ");
        qkJsonString(&b, file);
        qkBufPrintf(&b, "}, \"line\": %d, \"column\": 1}", line);
    }
    qkBufPrintf(&b, "], \"totalFrames\": %d}", depth);
    respond(requestSeq, "stackTrace", b.data);
    qkBufFree(&b);
}

static void variables(int requestSeq, int frame) {
    QkBuf b = {0};
    qkBufPuts(&b, "{\"variables\": [");
    for (int i = 0; i < qkDebugVarCount(frame - 1); i++) {
        const char* text = "";
        const char* name = qkDebugVar(frame - 1, i, &text);
        qkBufPuts(&b, i ? ", {\"name\": " : "{\"name\": ");
        qkJsonString(&b, name);
        qkBufPuts(&b, ", \"value\": ");
        qkJsonString(&b, text);
        qkBufPuts(&b, ", \"variablesReference\": 0}");
    }
    qkBufPuts(&b, "]}");
    respond(requestSeq, "variables", b.data);
    qkBufFree(&b);
}

/* a name's value, in the frame asked about (the innermost when none is) */
static void evaluate(int requestSeq, const J* args) {
    const J* expr = qkJsonGet(args, "expression");
    const J* frame = qkJsonGet(args, "frameId");
    int depth = frame ? (int)frame->num - 1 : qkDebugDepth() - 1;
    const char* want = expr && expr->kind == J_STR ? expr->str : "";
    while (*want == ' ') want++;
    for (int i = qkDebugVarCount(depth) - 1; i >= 0; i--) {
        const char* text = "";
        const char* name = qkDebugVar(depth, i, &text);
        if (!strcmp(name, want)) {
            QkBuf b = {0};
            qkBufPuts(&b, "{\"result\": ");
            qkJsonString(&b, text);
            qkBufPuts(&b, ", \"variablesReference\": 0}");
            respond(requestSeq, "evaluate", b.data);
            qkBufFree(&b);
            return;
        }
    }
    QkBuf m = {0};
    qkBufPrintf(&m, "%s has no value here yet (the debugger shows names, not expressions)", want);
    refuse(requestSeq, "evaluate", m.data);
    qkBufFree(&m);
}

static void quit(void) {
    endOutput();
    fflush(out);
    exit(0);
}

static bool launch(int requestSeq, const J* args, const char* prelude);

/* Answers the client until a request lets the program go on (true), or the client is gone (false). */
static bool serve(const char* prelude) {
    for (;;) {
        char* body = readMessage();
        if (!body) return false;
        J* m = qkJsonParse(body, strlen(body));
        free(body);
        if (!m) continue;
        const J* cmdJ = qkJsonGet(m, "command");
        const J* seqJ = qkJsonGet(m, "seq");
        const J* args = qkJsonGet(m, "arguments");
        const char* cmd = cmdJ && cmdJ->kind == J_STR ? cmdJ->str : "";
        int rs = seqJ ? (int)seqJ->num : 0;
        bool running = qkDebugDepth() > 0;
        if (!strcmp(cmd, "initialize")) {
            respond(rs, cmd, "{\"supportsConfigurationDoneRequest\": true, \"supportsEvaluateForHovers\": true, "
                             "\"supportsTerminateRequest\": true}");
            event("initialized", NULL);
        } else if (!strcmp(cmd, "launch")) {
            if (launch(rs, args, prelude)) respond(rs, cmd, NULL);
        } else if (!strcmp(cmd, "setBreakpoints")) {
            setBreakpoints(rs, args);
        } else if (!strcmp(cmd, "setExceptionBreakpoints")) {
            respond(rs, cmd, "{\"breakpoints\": []}");
        } else if (!strcmp(cmd, "configurationDone")) {
            respond(rs, cmd, NULL);
            if (!running && !ended && programPath) return true; /* the program starts */
        } else if (!strcmp(cmd, "threads")) {
            respond(rs, cmd, ended ? "{\"threads\": []}" : "{\"threads\": [{\"id\": 1, \"name\": \"main\"}]}");
        } else if (!strcmp(cmd, "stackTrace")) {
            stackTrace(rs);
        } else if (!strcmp(cmd, "scopes")) {
            const J* frame = qkJsonGet(args, "frameId");
            QkBuf b = {0};
            qkBufPrintf(&b, "{\"scopes\": [{\"name\": \"Locals\", \"variablesReference\": %d, \"expensive\": false}]}",
                        frame ? (int)frame->num : 0);
            respond(rs, cmd, b.data);
            qkBufFree(&b);
        } else if (!strcmp(cmd, "variables")) {
            const J* ref = qkJsonGet(args, "variablesReference");
            variables(rs, ref ? (int)ref->num : 0);
        } else if (!strcmp(cmd, "evaluate")) {
            evaluate(rs, args);
        } else if (!strcmp(cmd, "continue") || !strcmp(cmd, "next") || !strcmp(cmd, "stepIn") || !strcmp(cmd, "stepOut")) {
            if (!running) {
                refuse(rs, cmd, "the program is not running");
                continue;
            }
            mode = cmd[0] == 'c' ? RUN : cmd[0] == 'n' ? STEP_OVER : cmd[4] == 'I' ? STEP_IN : STEP_OUT;
            modeDepth = qkDebugDepth();
            qkDebugger.stepping = mode != RUN;
            respond(rs, cmd, cmd[0] == 'c' ? "{\"allThreadsContinued\": true}" : NULL);
            return true;
        } else if (!strcmp(cmd, "pause")) {
            respond(rs, cmd, NULL); /* the program runs in this thread: it stops only where it is told to */
        } else if (!strcmp(cmd, "disconnect") || !strcmp(cmd, "terminate")) {
            respond(rs, cmd, NULL);
            if (!ended) event("terminated", NULL);
            quit();
        } else {
            refuse(rs, cmd, "not handled");
        }
    }
}

static const char* dapPrelude;

static void stopped(int line) {
    int depth = qkDebugDepth();
    const char* reason = NULL;
    int frameLine = 0;
    const char* file = fileOf(qkDebugFrame(depth - 1, &frameLine));
    if (mode == ENTRY || mode == STEP_IN || (mode == STEP_OVER && depth <= modeDepth) || (mode == STEP_OUT && depth < modeDepth))
        reason = mode == ENTRY ? "entry" : "step";
    else if (breakAt(file, line)) reason = "breakpoint";
    if (!reason) return;
    flushOutput();
    QkBuf b = {0};
    qkBufPrintf(&b, "{\"reason\": \"%s\", \"threadId\": 1, \"allThreadsStopped\": true}", reason);
    event("stopped", b.data);
    qkBufFree(&b);
    if (!serve(dapPrelude)) quit(); /* the client is gone */
}

static bool launch(int requestSeq, const J* args, const char* prelude) {
    const J* program = qkJsonGet(args, "program");
    if (!program || program->kind != J_STR) {
        refuse(requestSeq, "launch", "launch needs \"program\": the .qk file to run");
        return false;
    }
    char* path = malloc(strlen(program->str) + 1);
    strcpy(path, program->str);
    const J* entry = qkJsonGet(args, "stopOnEntry");
    stopOnEntry = entry && entry->kind == J_BOOL && entry->b;
    const J* argv = qkJsonGet(args, "args");
    if (argv && argv->kind == J_ARR) { /* args(): the program's */
        qkProgramArgs = calloc((size_t)argv->count + 1, sizeof(char*));
        qkProgramArgCount = 0;
        for (int i = 0; i < argv->count; i++)
            if (argv->items[i]->kind == J_STR) {
                qkProgramArgs[qkProgramArgCount] = malloc(strlen(argv->items[i]->str) + 1);
                strcpy(qkProgramArgs[qkProgramArgCount++], argv->items[i]->str);
            }
    }
    size_t len;
    char* text = qkReadFile(path, &len);
    if (!text) {
        QkBuf m = {0};
        qkBufPrintf(&m, "cannot read %s", path);
        refuse(requestSeq, "launch", m.data);
        qkBufFree(&m);
        return false;
    }
    qkErrorSink = compileError;
    QkSource* src = qkNew(sizeof *src);
    *src = (QkSource){path, text, len};
    bool ok = qkParse(src, &prog);
    if (ok && prelude) {
        size_t plen;
        char* ptext = qkReadFile(prelude, &plen);
        QkSource* ps = qkNew(sizeof *ps);
        *ps = (QkSource){prelude, ptext ? ptext : "", ptext ? plen : 0};
        Program pre;
        if (ptext && qkParse(ps, &pre)) qkAddPrelude(&prog, &pre);
    }
    ok = ok && qkLoadUses(&prog, path) && qkCheck(&prog, true);
    qkErrorSink = NULL;
    if (!ok) {
        refuse(requestSeq, "launch", "the program does not compile");
        return false;
    }
    programPath = path;
    qkRunPath = path;
    return true;
}

/* runs main under the debugger: false when it does not start */
static int runProgram(void) {
    IrProgram ir;
    MachineCode mc;
    qkLower(&prog, &ir, false);
    qkOptimize(&ir);
    qkGenCode(&ir, &mc);
    if (ir.mainIndex < 0 || ir.externCount) {
        output("stderr", "quark: a program to debug starts at fn main() and calls no host function\n", 73);
        return 1;
    }
    Loaded l;
    if (!qkLoad((const uint8_t*)mc.code.data, mc.code.len, (const uint8_t*)ir.data.data, ir.data.len, mc.relocs, mc.relocCount,
                mc.enter, mc.escape, NULL, &l)) {
        output("stderr", "quark: cannot load the machine code\n", 36);
        return 1;
    }
    qkLoadTrace(&l, &mc, &ir);
    mode = stopOnEntry ? ENTRY : RUN;
    qkDebugger.stepping = stopOnEntry;
    bool ok = qkRunEntry(&l, l.code + mc.fnAt[ir.mainIndex], NULL, 0, NULL, 0);
    flushOutput();
    if (ok) return 0;
    const char* exitAt = strstr(qkPanicText, ": exit(");
    if (exitAt) return atoi(exitAt + 7); /* exit(code): an end, not a panic */
    QkBuf b = {0};
    qkBufPrintf(&b, "%s\n", qkPanicText);
    output("stderr", b.data, b.len);
    qkBufFree(&b);
    return 1;
}

int qkDap(const char* prelude) {
    fflush(stdout);
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    in = fdOpen(dupFd(0), "rb");
    out = fdOpen(dupFd(1), "wb");
    if (!in || !out) return 1;
    if (!freopen(NULL_DEVICE, "rb", stdin)) clearerr(stdin); /* the program's stdin: nothing (the client's is ours) */
    captureOutput();
    dapPrelude = prelude;
    qkDebug = true;                 /* lines, values and frames compiled in */
    qkDebugger.interactive = true;  /* one thread */
    qkDebugger.stopped = stopped;
    qkExitEnds = false;             /* exit(code) ends the run, not the adapter */
    qkUseAvx = qkCpuHasAvx();
    if (!serve(prelude)) quit();    /* until configurationDone (after launch) */
    int status = runProgram();
    ended = true;
    qkDebugger.stopped = NULL;
    QkBuf b = {0};
    qkBufPrintf(&b, "{\"exitCode\": %d}", status);
    event("exited", b.data);
    qkBufFree(&b);
    event("terminated", NULL);
    serve(prelude);                 /* until disconnect */
    quit();
    return 0;
}
