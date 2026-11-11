/* Quark — the command line.
 *
 *   quark run   file.qk [--fuel N] [--time]   compile to machine code in memory and run it
 *   quark build file.qk [-o file.exe]         a standalone executable (this program + the machine code)
 *   quark check file.qk                       only the errors
 *   quark fmt   file.qk [--write]             the canonical text
 *   quark graph file.qk                       the node graph, as JSON
 *   quark text  file.json                     a node graph back to Quark text
 *   quark ir    file.qk                       the intermediate code
 *
 * An executable made by `quark build` is this same program with the machine code appended: when it starts
 * and finds code at its own end, it runs that code instead of the command line. */
#include "quark.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#endif

typedef struct Trailer {
    char magic[8];
    uint64_t codeLen, dataLen, relocCount;
    int64_t entry;
    int64_t fuel;
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
    return 0;
#endif
}

static char* selfPath(void) {
#ifdef _WIN32
    wchar_t w[32768];
    DWORD n = GetModuleFileNameW(NULL, w, 32768);
    if (!n || n >= 32768) return NULL;
    int len = WideCharToMultiByte(CP_UTF8, 0, w, (int)n, NULL, 0, NULL, NULL);
    char* s = qkNew((size_t)len + 1);
    WideCharToMultiByte(CP_UTF8, 0, w, (int)n, s, len, NULL, NULL);
    return s;
#else
    return NULL;
#endif
}

/* ---- a program appended to this executable ------------------------------------------------------- */

static int runPayload(const char* self) {
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
    void (*entry)(void) = qkLoad(code, t.codeLen, data, t.dataLen, relocs, (int)t.relocCount, (int32_t)t.entry);
    if (!entry) return 1;
    entry();
    fflush(stdout);
    return 0;
}

/* ---- compiling ----------------------------------------------------------------------------------- */

static bool loadProgram(const char* path, Program* prog) {
    size_t len;
    char* text = qkReadFile(path, &len);
    if (!text) {
        fprintf(stderr, "quark: cannot read %s\n", path);
        return false;
    }
    QkSource src = {path, text, len};
    return qkParse(&src, prog);
}

static bool compile(const char* path, int64_t fuel, MachineCode* mc, IrProgram* ir, bool* timed) {
    double t0 = nowSeconds();
    Program prog;
    if (!loadProgram(path, &prog) || !qkCheck(&prog)) return false;
    double t1 = nowSeconds();
    qkLower(&prog, ir, fuel > 0);
    qkOptimize(ir);
    qkGenX64(ir, mc);
    double t2 = nowSeconds();
    if (timed && *timed)
        fprintf(stderr, "quark: parsed and checked in %.2f ms, compiled in %.2f ms (%zu bytes of machine code)\n",
                (t1 - t0) * 1000, (t2 - t1) * 1000, mc->code.len);
    return true;
}

static int usage(void) {
    fputs("Quark — a small compiled language whose programs are also node graphs.\n\n"
          "  quark run   file.qk [--fuel N] [--time]   compile to machine code and run it\n"
          "  quark build file.qk [-o file.exe]         make a standalone executable\n"
          "  quark check file.qk                       report errors only\n"
          "  quark fmt   file.qk [--write]             print (or rewrite) the canonical text\n"
          "  quark graph file.qk                       print the node graph (JSON)\n"
          "  quark text  file.json                     turn a node graph back into Quark\n"
          "  quark ir    file.qk                       print the intermediate code\n",
          stderr);
    return 2;
}

static int realMain(int argc, char** argv) {
    if (argc < 3) return usage();
    const char* cmd = argv[1];
    const char* path = argv[2];
    int64_t fuel = 0;
    bool timed = false, write = false;
    const char* outPath = NULL;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--fuel") && i + 1 < argc) fuel = strtoll(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--time")) timed = true;
        else if (!strcmp(argv[i], "--write")) write = true;
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outPath = argv[++i];
        else {
            fprintf(stderr, "quark: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    qkRunPath = path;

    if (!strcmp(cmd, "run")) {
        MachineCode mc;
        IrProgram ir;
        if (!compile(path, fuel, &mc, &ir, &timed)) return 1;
        if (fuel > 0) qkFuel = fuel;
        void (*entry)(void) = qkLoad((const uint8_t*)mc.code.data, mc.code.len, (const uint8_t*)ir.data.data, ir.data.len,
                                     mc.relocs, mc.relocCount, mc.entry);
        if (!entry) {
            fputs("quark: cannot load the machine code\n", stderr);
            return 1;
        }
        double t0 = nowSeconds();
        entry();
        fflush(stdout);
        if (timed) fprintf(stderr, "quark: ran in %.2f ms\n", (nowSeconds() - t0) * 1000);
        return 0;
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
        t.entry = mc.entry;
        t.fuel = fuel;
        t.payloadSize = out.len - start;
        qkBufAdd(&out, &t, sizeof t);
        char defaultOut[1024];
        if (!outPath) {
            snprintf(defaultOut, sizeof defaultOut, "%s", path);
            char* dot = strrchr(defaultOut, '.');
            if (dot && !strchr(dot, '/') && !strchr(dot, '\\')) *dot = '\0';
            strncat(defaultOut, ".exe", sizeof defaultOut - strlen(defaultOut) - 1);
            outPath = defaultOut;
        }
        if (!qkWriteFile(outPath, out.data, out.len)) {
            fprintf(stderr, "quark: cannot write %s\n", outPath);
            return 1;
        }
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
    if (!strcmp(cmd, "check")) {
        Program prog;
        if (!loadProgram(path, &prog) || !qkCheck(&prog)) return 1;
        fputs("ok\n", stderr);
        return 0;
    }
    if (!strcmp(cmd, "ir")) {
        Program prog;
        IrProgram ir;
        if (!loadProgram(path, &prog) || !qkCheck(&prog)) return 1;
        qkLower(&prog, &ir, fuel > 0);
        qkOptimize(&ir);
        QkBuf out = {0};
        qkIrDump(&ir, &out);
        fwrite(out.data, 1, out.len, stdout);
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
        if (!loadProgram(path, &prog) || !qkCheck(&prog)) return 1;
        QkBuf out = {0};
        qkGraphExport(&prog, &out);
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
    return usage();
}

int main(int argc, char** argv) {
    static char outBuffer[1 << 16];
    setvbuf(stdout, outBuffer, _IOFBF, sizeof outBuffer);
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_BINARY); /* \n stays \n, the same on every system */
#endif
    int payload = runPayload(selfPath());
    if (payload >= 0) return payload;
#ifdef _WIN32
    /* arguments as UTF-8 */
    int wargc;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        char** u = qkNew(sizeof(char*) * (size_t)(wargc + 1));
        for (int i = 0; i < wargc; i++) {
            int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
            u[i] = qkNew((size_t)n);
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, u[i], n, NULL, NULL);
        }
        argc = wargc;
        argv = u;
    }
#endif
    int status = realMain(argc, argv);
    fflush(stdout);
    return status;
}
