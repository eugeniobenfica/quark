/* Quark — the command line.
 *
 *   quark run   file.qk [--fuel N] [--time] [-- args...]   compile to machine code in memory and run it
 *   quark build file.qk [-o file.exe]         a standalone executable (this program + the machine code);
 *                                             --no-avx for processors without AVX
 *   quark check file.qk                       only the errors
 *   quark fmt   file.qk [--write]             the canonical text
 *   quark graph file.qk                       the node graph, as JSON
 *   quark text  file.json                     a node graph back to Quark text
 *   quark ir    file.qk                       the intermediate code
 *   quark get   [dir]                         fetch the packages quark.toml names, write quark.lock
 *
 * An executable made by `quark build` is this same program with the machine code appended: when it starts
 * and finds code at its own end, it runs that code instead of the command line. */
#define _DEFAULT_SOURCE /* POSIX: readlink, clock_gettime, chmod */
#define _GNU_SOURCE     /* Linux: REG_RIP, for the profiler */
#define _DARWIN_C_SOURCE
#include "qk.h"
#include "../include/quark.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#include <mmsystem.h>
#else
#include <sys/stat.h>
#include <signal.h>
#include <sys/time.h>
#ifdef __linux__
#include <ucontext.h>
#endif
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

typedef struct Trailer {
    char magic[8];
    uint64_t codeLen, dataLen, relocCount;
    int64_t entry; /* main's offset */
    int64_t enter, escape;
    int64_t fuel;
    int64_t features; /* 1: AVX */
    uint64_t payloadSize;
} Trailer;

static const char kMagic[8] = {'Q', 'U', 'A', 'R', 'K', 'P', 'R', 'G'};

static double nowSeconds(void) {
#ifdef _WIN32
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
#endif
}

static char* selfPath(void) {
#ifdef _WIN32
    wchar_t w[32768];
    DWORD n = GetModuleFileNameW(NULL, w, 32768);
    if (!n || n >= 32768) return NULL;
    int len = WideCharToMultiByte(CP_UTF8, 0, w, (int)n, NULL, 0, NULL, NULL);
    char* s = calloc(1, (size_t)len + 1); /* not the compiler's memory: a compile frees that */
    WideCharToMultiByte(CP_UTF8, 0, w, (int)n, s, len, NULL, NULL);
    return s;
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size)) return NULL;
    char* s = calloc(1, strlen(buf) + 1);
    if (s) strcpy(s, buf);
    return s;
#else
    char* s = calloc(1, 4097); /* Linux */
    ssize_t n = s ? readlink("/proc/self/exe", s, 4096) : -1;
    if (n <= 0) {
        free(s);
        return NULL;
    }
    return s;
#endif
}

/* ---- a program appended to this executable ------------------------------------------------------- */

static int runPayload(const char* self, int argc, char** argv) {
    size_t len;
    char* image = self ? qkReadFile(self, &len) : NULL;
    if (!image || len < sizeof(Trailer)) return -1;
    Trailer t;
    memcpy(&t, image + len - sizeof t, sizeof t);
    if (memcmp(t.magic, kMagic, 8) || t.payloadSize > len - sizeof t) {
        free(image);
        return -1;
    }
    const uint8_t* p = (const uint8_t*)image + len - sizeof t - t.payloadSize;
    const uint8_t* code = p;
    const uint8_t* data = code + t.codeLen;
    const Reloc* relocs = (const Reloc*)(data + t.dataLen);
    if (t.fuel > 0) qkFuel = t.fuel;
    qkProgramArgs = argv + 1; /* everything after the executable's own name */
    qkProgramArgCount = argc > 1 ? argc - 1 : 0;
    if ((t.features & 1) && !qkCpuHasAvx()) {
        fputs("this program was built for processors with AVX, and this one has none\n", stderr);
        return 1;
    }
    Loaded l;
    if (!qkLoad(code, t.codeLen, data, t.dataLen, relocs, (int)t.relocCount, (int32_t)t.enter, (int32_t)t.escape, NULL, &l)) return 1;
    bool ok = qkRunEntry(&l, l.code + t.entry, NULL, 0, NULL, 0);
    fflush(stdout);
    if (!ok) {
        fprintf(stderr, "%s\n", qkPanicText);
        return 1;
    }
    return 0;
}

/* Runs main; a panic prints its message. */
/* ---- quark run --profile: where the time goes, by sampling -------------------------------------------
 * Every millisecond another thread (Windows) or a timer signal (Linux) reads where the program is; at the end,
 * each function's share of the samples. Inlined functions count in the function they were inlined into. */
static const uint8_t* profCode;
static size_t profLen;
static int32_t* profAt;      /* each function's start, as the code has them (increasing) */
static int profCount;
static volatile int64_t* profHits;  /* per function; [profCount]: outside Quark's code (the runtime, C) */
static volatile bool profOn;

static void profSample(uintptr_t pc) {
    if (pc < (uintptr_t)profCode || pc >= (uintptr_t)profCode + profLen) {
        profHits[profCount]++;
        return;
    }
    int32_t at = (int32_t)(pc - (uintptr_t)profCode);
    int f = -1;
    for (int i = 0; i < profCount; i++)
        if (profAt[i] <= at && (f < 0 || profAt[i] > profAt[f])) f = i;
    profHits[f >= 0 ? f : profCount]++;
}

#ifdef _WIN32
static HANDLE profTarget;
static DWORD WINAPI profThread(LPVOID unused) {
    (void)unused;
    timeBeginPeriod(1);
    while (profOn) {
        Sleep(1);
        if (SuspendThread(profTarget) == (DWORD)-1) continue;
        CONTEXT ctx;
        ctx.ContextFlags = CONTEXT_CONTROL;
#if defined(_M_ARM64) || defined(__aarch64__)
        if (GetThreadContext(profTarget, &ctx)) profSample((uintptr_t)ctx.Pc);
#else
        if (GetThreadContext(profTarget, &ctx)) profSample((uintptr_t)ctx.Rip);
#endif
        ResumeThread(profTarget);
    }
    timeEndPeriod(1);
    return 0;
}
static HANDLE profHandle;
static void profStart(void) {
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &profTarget, 0, FALSE, DUPLICATE_SAME_ACCESS);
    profOn = true;
    profHandle = CreateThread(NULL, 0, profThread, NULL, 0, NULL);
}
static void profStop(void) {
    profOn = false;
    WaitForSingleObject(profHandle, INFINITE);
    CloseHandle(profHandle);
    CloseHandle(profTarget);
}
#elif defined(__linux__)
static void profSignal(int sig, siginfo_t* info, void* uc) {
    (void)sig, (void)info;
    ucontext_t* u = uc;
#if defined(__x86_64__)
    profSample((uintptr_t)u->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
    profSample((uintptr_t)u->uc_mcontext.pc);
#endif
}
static void profStart(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = profSignal;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    struct itimerval t = {{0, 1000}, {0, 1000}};
    setitimer(ITIMER_PROF, &t, NULL);
    profOn = true;
}
static void profStop(void) {
    struct itimerval t = {{0, 0}, {0, 0}};
    setitimer(ITIMER_PROF, &t, NULL);
    profOn = false;
}
#else
static void profStart(void) { fputs("quark: --profile works on Windows and Linux\n", stderr); }
static void profStop(void) {}
#endif

static bool profiling;

static void profReport(const IrProgram* ir) {
    int64_t total = 0;
    for (int i = 0; i <= profCount; i++) total += profHits[i];
    if (!total) {
        fputs("quark: profile: no samples (the program ran under a millisecond)\n", stderr);
        return;
    }
    fprintf(stderr, "quark: profile, %lld samples (one a millisecond):\n", (long long)total);
    bool* shown = calloc((size_t)profCount + 1, 1);
    for (;;) {
        int best = -1;
        for (int i = 0; i <= profCount; i++)
            if (!shown[i] && profHits[i] && (best < 0 || profHits[i] > profHits[best])) best = i;
        if (best < 0) break;
        shown[best] = true;
        fprintf(stderr, "  %5.1f%%  %s\n", 100.0 * (double)profHits[best] / (double)total,
                best < profCount ? ir->fns[best].name : "(the runtime: printing, text, maps, memory)");
    }
    free(shown);
}

static int runMain(const MachineCode* mc, const IrProgram* ir, bool timed) {
    if (ir->mainIndex < 0) {
        fputs("quark: a program starts at fn main() { ... }\n", stderr);
        return 1;
    }
    if (ir->externCount) {
        fprintf(stderr, "quark: this program calls the host (extern fn %s): run it from a host that gives it\n", ir->externs[0].name);
        return 1;
    }
    Loaded l;
    if (!qkLoad((const uint8_t*)mc->code.data, mc->code.len, (const uint8_t*)ir->data.data, ir->data.len, mc->relocs,
                mc->relocCount, mc->enter, mc->escape, NULL, &l)) {
        fputs("quark: cannot load the machine code\n", stderr);
        return 1;
    }
    qkLoadTrace(&l, mc, ir);
    double t0 = nowSeconds();
    if (profiling) {
        profCode = l.code;
        profLen = l.codeLen;
        profAt = mc->fnAt;
        profCount = ir->count;
        profHits = calloc((size_t)ir->count + 1, sizeof(int64_t));
        profStart();
    }
    bool ok = qkRunEntry(&l, l.code + mc->fnAt[ir->mainIndex], NULL, 0, NULL, 0);
    if (profiling) {
        profStop();
        fflush(stdout);
        profReport(ir);
    }
    fflush(stdout);
    if (!ok) {
        fprintf(stderr, "%s\n", qkPanicText);
        return 1;
    }
    if (timed) fprintf(stderr, "quark: ran in %.2f ms\n", (nowSeconds() - t0) * 1000);
    return 0;
}

/* ---- compiling ----------------------------------------------------------------------------------- */

static const char* preludePath; /* --prelude: the host's declarations */

static bool loadProgram(const char* path, Program* prog) {
    size_t len;
    char* text = qkReadFile(path, &len);
    if (!text) {
        fprintf(stderr, "quark: cannot read %s\n", path);
        return false;
    }
    QkSource src = {path, text, len};
    if (!qkParse(&src, prog)) return false;
    if (preludePath) {
        char* ptext = qkReadFile(preludePath, &len);
        if (!ptext) {
            fprintf(stderr, "quark: cannot read %s\n", preludePath);
            return false;
        }
        QkSource* psrc = qkNew(sizeof *psrc);
        *psrc = (QkSource){preludePath, ptext, len};
        Program prelude;
        if (!qkParse(psrc, &prelude)) return false;
        qkAddPrelude(prog, &prelude);
    }
    return qkLoadUses(prog, path); /* the files its uses name */
}

static bool compile(const char* path, int64_t fuel, MachineCode* mc, IrProgram* ir, bool* timed) {
    double t0 = nowSeconds();
    Program prog;
    if (!loadProgram(path, &prog) || !qkCheck(&prog, true)) return false;
    double t1 = nowSeconds();
    qkLower(&prog, ir, fuel > 0);
    qkOptimize(ir);
    qkGenCode(ir, mc);
    double t2 = nowSeconds();
    if (timed && *timed)
        fprintf(stderr, "quark: parsed and checked in %.2f ms, compiled in %.2f ms (%zu bytes of machine code)\n",
                (t1 - t0) * 1000, (t2 - t1) * 1000, mc->code.len);
    return true;
}

/* ---- quark repl: declarations kept, lets kept, an expression's value printed ---------------------- */

static void silent(const char* path, int line, int col, const char* message) {
    (void)path, (void)line, (void)col, (void)message;
}

/* the session as a program: its declarations, then main with its lets and this line */
static int replRun(const QkBuf* decls, const QkBuf* lets, const char* line, bool quiet) {
    QkBuf text = {0};
    qkBufPrintf(&text, "%s\nfn main() {\n%s%s\n}\n", decls->data ? decls->data : "", lets->data ? lets->data : "", line);
    qkNewReset();
    qkTypesReset();
    qkErrorSink = quiet ? silent : NULL;
    int errors = qkErrorCount;
    Program prog;
    QkSource src = {"repl", qkDup(text.data, text.len), text.len};
    bool ok = qkParse(&src, &prog) && qkLoadUses(&prog, "repl") && qkCheck(&prog, true);
    qkErrorSink = NULL;
    qkBufFree(&text);
    if (!ok || qkErrorCount != errors) {
        qkErrorCount = errors;
        return -1; /* does not compile */
    }
    IrProgram ir;
    MachineCode mc;
    qkLower(&prog, &ir, false);
    qkOptimize(&ir);
    qkGenCode(&ir, &mc);
    return runMain(&mc, &ir, false);
}

static int repl(void) {
    QkBuf decls = {0}, lets = {0}, pending = {0};
    char line[4096];
    int depth = 0;
    puts("Quark: declarations, lets and expressions; :quit to leave");
    for (;;) {
        fputs(depth ? "... " : "> ", stdout);
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!depth && (!strcmp(line, ":quit") || !strcmp(line, ":q"))) break;
        for (char* c = line; *c; c++) depth += *c == '{' ? 1 : *c == '}' ? -1 : 0;
        qkBufPuts(&pending, line);
        qkBufPuts(&pending, "\n");
        if (depth > 0) continue; /* a block not closed yet */
        depth = 0;
        char* in = malloc(pending.len + 1);
        memcpy(in, pending.data, pending.len);
        in[pending.len] = 0;
        pending.len = 0;
        char* t = in;
        while (*t == ' ') t++;
        size_t len = strlen(t);
        while (len && t[len - 1] == '\n') t[--len] = 0;
        if (!*t) {
            free(in);
            continue;
        }
        bool decl = !strncmp(t, "fn ", 3) || !strncmp(t, "type ", 5) || !strncmp(t, "enum ", 5) || !strncmp(t, "const ", 6) ||
                    !strncmp(t, "use ", 4) || !strncmp(t, "pub ", 4);
        if (decl) { /* kept when the whole compiles */
            QkBuf more = {0}, none = {0};
            qkBufPrintf(&more, "%s%s\n", decls.data ? decls.data : "", t);
            if (replRun(&more, &none, "", false) != -1) {
                qkBufFree(&decls);
                decls = more;
            } else {
                qkBufFree(&more);
            }
        } else if (!strncmp(t, "let ", 4) || !strncmp(t, "var ", 4)) { /* kept, and run again before what follows */
            if (replRun(&decls, &lets, t, false) == 0) qkBufPrintf(&lets, "%s\n", t);
        } else { /* an expression: its value printed; else a statement, run once */
            QkBuf shown = {0};
            qkBufPrintf(&shown, "print(%s)", t);
            if (replRun(&decls, &lets, shown.data, true) == -1) replRun(&decls, &lets, t, false);
            qkBufFree(&shown);
        }
        free(in);
    }
    return 0;
}

/* quark test: each test "name" { ... } on its own, with its own memory; a failed assert or a panic fails only it. */
static int runTests(const char* path, int64_t fuel) {
    Program prog;
    if (!loadProgram(path, &prog) || !qkCheck(&prog, false)) return 1;
    IrProgram ir;
    MachineCode mc;
    qkLower(&prog, &ir, fuel > 0);
    qkOptimize(&ir);
    qkGenCode(&ir, &mc);
    if (ir.externCount) {
        fprintf(stderr, "quark: these tests call the host (extern fn %s): run them from a host that gives it\n", ir.externs[0].name);
        return 1;
    }
    Loaded l;
    if (!qkLoad((const uint8_t*)mc.code.data, mc.code.len, (const uint8_t*)ir.data.data, ir.data.len, mc.relocs, mc.relocCount,
                mc.enter, mc.escape, NULL, &l)) {
        fputs("quark: cannot load the machine code\n", stderr);
        return 1;
    }
    qkLoadTrace(&l, &mc, &ir);
    int passed = 0, failed = 0;
    for (int i = 0; i < prog.count; i++) {
        Decl* d = prog.decls[i];
        if (d->kind != D_FN || !d->isTest) continue;
        int fn = -1;
        for (int k = 0; k < ir.count && fn < 0; k++)
            if (!strcmp(ir.fns[k].name, d->name)) fn = k;
        if (fn < 0) continue;
        qkFuel = fuel > 0 ? fuel : INT64_MAX;
        double t0 = nowSeconds();
        bool ok = qkRunEntry(&l, l.code + mc.fnAt[fn], NULL, 0, NULL, 0);
        double ms = (nowSeconds() - t0) * 1000;
        if (ok) {
            passed++;
            printf("ok    %s (%.1f ms)\n", d->testName, ms);
        } else {
            failed++;
            printf("FAIL  %s\n      %s\n", d->testName, qkPanicText);
        }
        fflush(stdout);
    }
    printf("%d passed, %d failed\n", passed, failed);
    qkUnload(&l);
    return failed ? 1 : 0;
}

int qkGet(const char* root); /* package.c */

static int usage(void) {
    fputs("Quark — a small compiled language whose programs are also node graphs.\n\n"
          "  quark debug file.qk [--break LINE]...   stop at lines, step, see the names' values\n"
          "  quark repl                                declarations, lets and expressions, line by line\n"
          "  quark code  file.qk [-o code.bin]         the machine code, raw, for a disassembler\n"
          "  quark object file.qk -o file.o [--symbol name]   an object file a host links (QkStaticProgram name)\n"
          "  quark run   file.qk [--fuel N] [--time] [--allow print,files,...] [-- args...]   compile to machine code and run it\n"
          "  quark build file.qk [-o file.exe]         make a standalone executable\n"
          "  quark test  file.qk                       run its test \"...\" { } blocks\n"
          "  quark lsp   [--prelude host.qk]           a language server (stdio), for editors\n"
          "  quark check file.qk                       report errors only\n"
          "  quark fmt   file.qk [--write]             print (or rewrite) the canonical text\n"
          "  quark graph file.qk                       print the node graph (JSON)\n"
          "  quark text  file.json                     turn a node graph back into Quark\n"
          "  quark ir    file.qk                       print the intermediate code\n"
          "  quark catalog file.qk | -                 every node an editor can offer (JSON)\n"
          "  quark get   [dir]                         fetch the packages quark.toml names (git), write quark.lock\n\n"
          "  --prelude host.qk   the host's declarations (extern fn, extern type) for run, check, graph, catalog\n"
          "  --layout pos.json   graph: node positions by key, written into the nodes as \"pos\"\n",
          stderr);
    return 2;
}

static int realMain(int argc, char** argv) {
    if (argc >= 2 && !strcmp(argv[1], "lsp")) { /* quark lsp [--prelude host.qk]: for editors */
        const char* prelude = argc >= 4 && !strcmp(argv[2], "--prelude") ? argv[3] : NULL;
        return qkLsp(prelude);
    }
    if (argc >= 2 && !strcmp(argv[1], "repl")) { /* quark repl: line by line */
        qkExitEnds = true;
        return repl();
    }
    if (argc >= 2 && !strcmp(argv[1], "get")) /* quark get [dir]: the packages quark.toml names, into .quark/pkg */
        return argc > 3 ? usage() : qkGet(argc == 3 ? argv[2] : ".");
    if (argc < 3) return usage();
    const char* cmd = argv[1];
    const char* path = argv[2];
    int64_t fuel = 0;
    bool timed = false, write = false, noAvx = false;
    const char* outPath = NULL;
    const char* symbolName = NULL;
    const char* target = NULL;
    const char* layoutPath = NULL;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--")) { /* the rest is the program's: args() */
            qkProgramArgs = argv + i + 1;
            qkProgramArgCount = argc - i - 1;
            break;
        }
        if (!strcmp(argv[i], "--fuel") && i + 1 < argc) fuel = strtoll(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--symbol") && i + 1 < argc) symbolName = argv[++i]; /* quark object: the program's name */
        else if (!strcmp(argv[i], "--target") && i + 1 < argc) target = argv[++i]; /* quark object: arm64-macos, x86_64-linux... */
        else if (!strcmp(argv[i], "--profile")) profiling = true;
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) { /* the last value of every name, by graph key (JSON) */
            qkDebug = true;
            qkDebugger.tracePath = argv[++i];
        } else if (!strcmp(argv[i], "--break") && i + 1 < argc) { /* quark debug: stop at this line */
            if (qkDebugger.breakCount == qkDebugger.breakCap) {
                qkDebugger.breakCap = qkDebugger.breakCap ? qkDebugger.breakCap * 2 : 8;
                qkDebugger.breaks = realloc(qkDebugger.breaks, sizeof(int) * (size_t)qkDebugger.breakCap);
            }
            qkDebugger.breaks[qkDebugger.breakCount++] = atoi(argv[++i]);
        } /* where the time goes, by function */
        else if (!strcmp(argv[i], "--checked")) qkChecked = true; /* Int + - * past the limits: a panic */
        else if (!strcmp(argv[i], "--memory") && i + 1 < argc) qkArenaLimit(qkArena, strtoll(argv[++i], NULL, 10) << 20); /* MB */
        else if (!strcmp(argv[i], "--allow") && i + 1 < argc) qkCapabilities = argv[++i]; /* a sandbox: print,clock */
        else if (!strcmp(argv[i], "--time")) timed = true;
        else if (!strcmp(argv[i], "--write")) write = true;
        else if (!strcmp(argv[i], "--no-avx")) noAvx = true;
        else if (!strcmp(argv[i], "--abi") && i + 1 < argc) { /* sysv on Windows: tests the Linux / macOS calls */
            const char* abi = argv[++i];
            qkAbi = !strcmp(abi, "sysv") ? QK_ABI_SYSV : QK_ABI_WIN64;
        }
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outPath = argv[++i];
        else if (!strcmp(argv[i], "--prelude") && i + 1 < argc) preludePath = argv[++i];
        else if (!strcmp(argv[i], "--layout") && i + 1 < argc) layoutPath = argv[++i];
        else {
            fprintf(stderr, "quark: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    qkRunPath = path;
    qkUseAvx = !noAvx && qkCpuHasAvx();

    if (!strcmp(cmd, "debug")) { /* stops at the breakpoints (or the first line), then steps */
        qkDebug = true;
        qkDebugger.interactive = true;
        qkDebugger.stepping = qkDebugger.breakCount == 0;
        cmd = "run";
    }
    if (!strcmp(cmd, "run")) {
        MachineCode mc;
        IrProgram ir;
        if (!compile(path, fuel, &mc, &ir, &timed)) return 1;
        if (fuel > 0) qkFuel = fuel;
        int status = runMain(&mc, &ir, timed);
        qkDebugFinish(); /* --trace: the values, written */
        return status;
    }
    if (!strcmp(cmd, "build")) {
        MachineCode mc;
        IrProgram ir;
        if (!compile(path, fuel, &mc, &ir, &timed)) return 1;
        const char* self = selfPath();
        size_t selfLen;
        char* image = self ? qkReadFile(self, &selfLen) : NULL;
        if (!image) {
            fputs("quark: cannot read the quark executable itself\n", stderr);
            return 1;
        }
        /* an image that already carries a program: keep only the program-free part */
        if (selfLen >= sizeof(Trailer)) {
            Trailer old;
            memcpy(&old, image + selfLen - sizeof old, sizeof old);
            if (!memcmp(old.magic, kMagic, 8)) selfLen -= sizeof old + old.payloadSize;
        }
        QkBuf out = {0};
        qkBufAdd(&out, image, selfLen);
        size_t start = out.len;
        qkBufAdd(&out, mc.code.data, mc.code.len);
        qkBufAdd(&out, ir.data.data, ir.data.len);
        qkBufAdd(&out, mc.relocs, sizeof(Reloc) * (size_t)mc.relocCount);
        Trailer t;
        memcpy(t.magic, kMagic, 8);
        t.codeLen = mc.code.len;
        t.dataLen = ir.data.len;
        t.relocCount = (uint64_t)mc.relocCount;
        if (ir.mainIndex < 0 || ir.externCount) {
            fputs("quark: an executable starts at fn main() and calls no host function\n", stderr);
            return 1;
        }
        t.entry = mc.fnAt[ir.mainIndex];
        t.enter = mc.enter;
        t.escape = mc.escape;
        t.fuel = fuel;
        t.features = qkUseAvx ? 1 : 0;
        t.payloadSize = out.len - start;
        qkBufAdd(&out, &t, sizeof t);
        char defaultOut[1024];
        if (!outPath) {
            snprintf(defaultOut, sizeof defaultOut, "%s", path);
            char* dot = strrchr(defaultOut, '.');
            if (dot && !strchr(dot, '/') && !strchr(dot, '\\')) *dot = '\0';
#ifdef _WIN32
            strncat(defaultOut, ".exe", sizeof defaultOut - strlen(defaultOut) - 1);
#endif
            outPath = defaultOut;
        }
        if (!qkWriteFile(outPath, out.data, out.len)) {
            fprintf(stderr, "quark: cannot write %s\n", outPath);
            return 1;
        }
#ifndef _WIN32
        chmod(outPath, 0755); /* it runs */
#endif
        fprintf(stderr, "quark: wrote %s (%zu bytes of machine code)\n", outPath, mc.code.len);
        return 0;
    }
    if (!strcmp(cmd, "bin")) { /* the raw machine code, for a disassembler */
        MachineCode mc;
        IrProgram ir;
        if (!compile(path, fuel, &mc, &ir, &timed)) return 1;
        if (!outPath || !qkWriteFile(outPath, mc.code.data, mc.code.len)) return 1;
        return 0;
    }
    if (!strcmp(cmd, "test")) {
        qkChecked = true; /* tests check Int arithmetic */
        return runTests(path, fuel);
    }
    if (!strcmp(cmd, "check")) {
        Program prog;
        if (!loadProgram(path, &prog) || !qkCheck(&prog, false)) return 1;
        fputs("ok\n", stderr);
        return 0;
    }
    if (!strcmp(cmd, "ir")) {
        Program prog;
        IrProgram ir;
        if (!loadProgram(path, &prog) || !qkCheck(&prog, false)) return 1;
        qkLower(&prog, &ir, fuel > 0);
        qkOptimize(&ir);
        QkBuf out = {0};
        qkIrDump(&ir, &out);
        fwrite(out.data, 1, out.len, stdout);
        return 0;
    }
    if (!strcmp(cmd, "object")) { /* an object file the host links: quark object file.qk -o file.o [--symbol name] */
        qkObjectMode = true;
        if (target) { /* another machine's: x86_64 or arm64, then windows, linux or macos */
            const char* dash = strchr(target, '-');
            bool arm = !strncmp(target, "arm64", 5) || !strncmp(target, "aarch64", 7);
            bool x64 = !strncmp(target, "x86_64", 6) || !strncmp(target, "x64", 3);
            const char* os = dash ? dash + 1 : "";
            int osId = !strcmp(os, "windows") ? QK_OS_WINDOWS : !strcmp(os, "linux") ? QK_OS_LINUX
                     : !strcmp(os, "macos") || !strcmp(os, "ios") ? QK_OS_MACOS : -1;
            if ((!arm && !x64) || osId < 0 || (arm && osId == QK_OS_WINDOWS)) {
                fprintf(stderr, "quark: --target is x86_64-windows, x86_64-linux, x86_64-macos, arm64-linux, arm64-macos or arm64-ios\n");
                return 2;
            }
            qkTargetArch = arm ? QK_ARCH_ARM64 : QK_ARCH_X64;
            qkTargetOs = osId;
            qkAbi = osId == QK_OS_WINDOWS ? QK_ABI_WIN64 : QK_ABI_SYSV;
            qkUseAvx = false; /* another machine: the baseline */
        }
        Program prog;
        IrProgram ir;
        if (!loadProgram(path, &prog) || !qkCheck(&prog, false)) return 1;
        qkLower(&prog, &ir, fuel > 0);
        qkOptimize(&ir);
        MachineCode mc;
        qkGenCode(&ir, &mc);
        QkBuf obj = {0};
        if (!qkWriteObject(&mc, &ir, symbolName ? symbolName : "quarkProgram", &obj)) {
            fputs("quark: no object format for this target yet\n", stderr);
            return 1;
        }
        if (!qkWriteFile(outPath ? outPath : "program.o", obj.data, obj.len)) {
            fputs("quark: cannot write the object file\n", stderr);
            return 1;
        }
        return 0;
    }
    if (!strcmp(cmd, "code")) { /* the machine code, raw, with each function's offset: for a disassembler */
        Program prog;
        IrProgram ir;
        if (!loadProgram(path, &prog) || !qkCheck(&prog, false)) return 1;
        qkLower(&prog, &ir, fuel > 0);
        qkOptimize(&ir);
        MachineCode mc;
        qkGenCode(&ir, &mc);
        const char* codePath = outPath ? outPath : "code.bin";
        if (!qkWriteFile(codePath, mc.code.data, mc.code.len)) return 1;
        for (int i = 0; i < ir.count; i++) printf("%08x %s\n", mc.fnAt[i], ir.fns[i].name);
        return 0;
    }
    if (!strcmp(cmd, "fmt")) {
        Program prog;
        if (!loadProgram(path, &prog)) return 1;
        QkBuf out = {0};
        qkFormat(&prog, &out);
        if (write) {
            if (!qkWriteFile(path, out.data, out.len)) {
                fprintf(stderr, "quark: cannot write %s\n", path);
                return 1;
            }
            return 0;
        }
        fwrite(out.data, 1, out.len, stdout);
        return 0;
    }
    if (!strcmp(cmd, "graph")) {
        Program prog;
        if (!loadProgram(path, &prog) || !qkCheck(&prog, false)) return 1;
        char* layout = NULL;
        if (layoutPath) {
            size_t layoutLen;
            layout = qkReadFile(layoutPath, &layoutLen);
            if (!layout) {
                fprintf(stderr, "quark: cannot read %s\n", layoutPath);
                return 1;
            }
        }
        QkBuf out = {0};
        qkGraphExportWith(&prog, &out, layout);
        fwrite(out.data, 1, out.len, stdout);
        return 0;
    }
    if (!strcmp(cmd, "text")) {
        size_t len;
        char* json = qkReadFile(path, &len);
        if (!json) {
            fprintf(stderr, "quark: cannot read %s\n", path);
            return 1;
        }
        Program prog;
        if (!qkGraphImport(json, len, path, &prog)) return 1;
        QkBuf out = {0};
        qkFormat(&prog, &out);
        fwrite(out.data, 1, out.len, stdout);
        return 0;
    }
    if (!strcmp(cmd, "catalog")) { /* path: a program, or - for the built-ins and the prelude only */
        QkVm* vm = qkVmNew();
        if (preludePath) {
            size_t len;
            char* text = qkReadFile(preludePath, &len);
            if (!text || !qkVmPrelude(vm, text)) {
                fprintf(stderr, "quark: the prelude %s does not read\n", preludePath);
                return 1;
            }
        }
        size_t len = 0;
        char* text = strcmp(path, "-") ? qkReadFile(path, &len) : NULL;
        char* json = qkCatalog(vm, text, len);
        if (!json) return 1;
        fputs(json, stdout);
        qkFree(json);
        qkVmFree(vm);
        return 0;
    }
    return usage();
}

int main(int argc, char** argv) {
    qkExitEnds = true;
    static char outBuffer[1 << 16];
    setvbuf(stdout, outBuffer, _IOFBF, sizeof outBuffer);
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_BINARY); /* \n stays \n, the same on every system */
#endif
#ifdef _WIN32
    /* arguments as UTF-8 */
    int wargc;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        char** u = calloc((size_t)wargc + 1, sizeof(char*)); /* the arguments outlive compiles */
        for (int i = 0; i < wargc; i++) {
            int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
            u[i] = malloc((size_t)n);
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, u[i], n, NULL, NULL);
        }
        argc = wargc;
        argv = u;
    }
#endif
    int payload = runPayload(selfPath(), argc, argv);
    if (payload >= 0) return payload;
    int status = realMain(argc, argv);
    fflush(stdout);
    return status;
}
