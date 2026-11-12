/* Quark — the runtime compiled code calls: printing, memory, time, the math library, and stopping with a
 * message. Memory comes from an arena: a program allocates by bumping a pointer and frees everything when it
 * ends (a host would reset the arena after each call). Also the loader that puts machine code in memory. */
#include "qk.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <time.h>
#endif

int64_t qkFuel = INT64_MAX;
bool qkUseAvx;

#if defined(__GNUC__) && (defined(__x86_64__) || defined(_M_X64))
#include <cpuid.h>
bool qkCpuHasAvx(void) {
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return false;
    if (!(c & (1u << 27)) || !(c & (1u << 28))) return false; /* OSXSAVE and AVX */
    unsigned lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (lo & 6) == 6; /* the system saves the AVX registers */
}
#else
bool qkCpuHasAvx(void) { return false; }
#endif
const char* qkRunPath = "program";

char qkPanicText[512];
void* qkEscapeRsp;
void (*qkEscapeStub)(void);

/* The message, then: back to the host's call (the trampoline returns 1), or the end of the process. */
static void rtPanic(int64_t code, int64_t line, int64_t a, int64_t b) {
    char* m = qkPanicText;
    size_t n = sizeof qkPanicText;
    long long L = (long long)line, A = (long long)a, B = (long long)b;
    switch (code) {
    case PANIC_INDEX: snprintf(m, n, "%s:%lld: index %lld is outside the array (its length is %lld)", qkRunPath, L, A, B); break;
    case PANIC_DIVIDE: snprintf(m, n, "%s:%lld: division by zero", qkRunPath, L); break;
    case PANIC_FUEL: snprintf(m, n, "%s:%lld: the program ran too long (its budget ran out)", qkRunPath, L); break;
    case PANIC_HISTOGRAM: snprintf(m, n, "%s:%lld: histogram: bin %lld is outside 0..%lld", qkRunPath, L, A, B); break;
    case PANIC_EMPTY: snprintf(m, n, "%s:%lld: min / max of nothing (the list is empty)", qkRunPath, L); break;
    case PANIC_SIZE: snprintf(m, n, "%s:%lld: an array of %lld elements cannot be made", qkRunPath, L, A); break;
    default: snprintf(m, n, "%s:%lld: stopped (%lld)", qkRunPath, L, (long long)code);
    }
    if (qkEscapeRsp && qkEscapeStub) qkEscapeStub(); /* never returns */
    fflush(stdout);
    fprintf(stderr, "%s\n", m);
    exit(1);
}

static void rtPrintInt(int64_t v) { printf("%lld", (long long)v); }

static void rtPrintFloat(double v) {
    char s[40];
    if (v != v) {
        fputs("nan", stdout);
        return;
    }
    if (v == INFINITY || v == -INFINITY) {
        fputs(v > 0 ? "inf" : "-inf", stdout);
        return;
    }
    /* the shortest text that reads back as the same number, and a Float always shows its point */
    for (int digits = 1; digits <= 17; digits++) {
        snprintf(s, sizeof s, "%.*g", digits, v);
        if (strtod(s, NULL) != v) continue;
        double size = fabs(v);
        if (strchr(s, 'e') && size >= 1e-4 && size < 1e16) { /* 130, not 1.3e+02 */
            int decimals = digits - 1 - (int)floor(log10(size));
            snprintf(s, sizeof s, "%.*f", decimals > 0 ? decimals : 0, v);
        }
        break;
    }
    fputs(s, stdout);
    if (!strpbrk(s, ".eEn")) fputs(".0", stdout);
}

static void rtPrintBool(int64_t v) { fputs(v ? "true" : "false", stdout); }
static void rtPrintText(const char* p, int64_t n) { fwrite(p, 1, (size_t)n, stdout); }
static void rtPrintSep(void) { fputc(' ', stdout); }
static void rtPrintEnd(void) { fputc('\n', stdout); }

/* ---- the arena ----------------------------------------------------------------------------------- */

typedef struct ArenaBlock {
    struct ArenaBlock* next;
    size_t size, used;
    char* data; /* 16-aligned */
} ArenaBlock;
static ArenaBlock* blocks;  /* the first */
static ArenaBlock* current; /* where allocation is */

static void* rtAlloc(int64_t count, int64_t bytesEach) {
    if (count < 0 || (count && bytesEach > (INT64_C(1) << 40) / count)) rtPanic(PANIC_SIZE, 0, count, 0);
    size_t bytes = ((size_t)(count * bytesEach) + 15) & ~(size_t)15;
    if (bytes == 0) bytes = 16;
    while (current && current->used + bytes > current->size) {
        if (!current->next || current->next->size < bytes) break;
        current = current->next; /* blocks after the current one are empty and zeroed */
    }
    if (!current || current->used + bytes > current->size) {
        size_t size = bytes > ((size_t)16 << 20) ? bytes : ((size_t)16 << 20);
        ArenaBlock* blk = calloc(1, sizeof(ArenaBlock) + size + 16);
        if (!blk) {
            snprintf(qkPanicText, sizeof qkPanicText, "%s: out of memory", qkRunPath);
            rtPanic(PANIC_SIZE, 0, count, 0);
        }
        blk->size = size;
        blk->data = (char*)(((uintptr_t)(blk + 1) + 15) & ~(uintptr_t)15);
        if (current) { /* in after the current block */
            blk->next = current->next;
            current->next = blk;
        } else {
            blocks = blk;
        }
        current = blk;
    }
    void* p = current->data + current->used;
    current->used += bytes;
    return p;
}

/* Everything the last call allocated goes; the memory stays, zeroed again (allocations are zeroed). */
/* Arrays that grow (push): each one's room, so the next push writes in place. */
typedef struct Growing {
    char* ptr;
    int64_t capBytes;
} Growing;
static Growing growing[64];
static int growingNext;

void qkArenaReset(void) {
    for (ArenaBlock* blk = blocks; blk; blk = blk->next) {
        memset(blk->data, 0, blk->used);
        blk->used = 0;
    }
    current = blocks;
    memset(growing, 0, sizeof growing);
}

/* xs.push(v): the array's address, with room for one more (in place when it has room, else moved to twice
 * the room). The caller stores v at index len and adds 1 to len. */
static void* rtPush(char* ptr, int64_t len, int64_t bytesEach) {
    int64_t need = (len + 1) * bytesEach;
    for (int i = 0; ptr && i < 64; i++)
        if (growing[i].ptr == ptr) {
            if (need <= growing[i].capBytes) return ptr;
            int64_t cap = len * 2 > 8 ? len * 2 : 8;
            char* p = rtAlloc(cap, bytesEach);
            memcpy(p, ptr, (size_t)(len * bytesEach));
            growing[i].ptr = p;
            growing[i].capBytes = cap * bytesEach;
            return p;
        }
    int64_t cap = len * 2 > 8 ? len * 2 : 8;
    char* p = rtAlloc(cap, bytesEach);
    if (len) memcpy(p, ptr, (size_t)(len * bytesEach));
    growing[growingNext].ptr = p;
    growing[growingNext].capBytes = cap * bytesEach;
    growingNext = (growingNext + 1) % 64;
    return p;
}

/* ---- text: made in the arena as [length: 8 bytes][bytes]; the code reads both ---------------------- */

static char* rtText(const char* bytes, int64_t len) {
    char* p = rtAlloc(len + 8, 1);
    memcpy(p, &len, 8);
    if (len) memcpy(p + 8, bytes, (size_t)len);
    return p;
}

static char* rtTextJoin(const char* a, int64_t la, const char* b, int64_t lb) {
    char* p = rtAlloc(la + lb + 8, 1);
    int64_t n = la + lb;
    memcpy(p, &n, 8);
    if (la) memcpy(p + 8, a, (size_t)la);
    if (lb) memcpy(p + 8 + la, b, (size_t)lb);
    return p;
}

static int64_t rtTextCompare(const char* a, int64_t la, const char* b, int64_t lb) {
    int c = memcmp(a, b, (size_t)(la < lb ? la : lb));
    if (c) return c < 0 ? -1 : 1;
    return la < lb ? -1 : la > lb ? 1 : 0;
}

static char* rtTextOfInt(int64_t v) {
    char s[32];
    int n = snprintf(s, sizeof s, "%lld", (long long)v);
    return rtText(s, n);
}

static char* rtTextOfFloat(double v) {
    char s[48];
    int n;
    if (v != v) n = snprintf(s, sizeof s, "nan");
    else if (v == INFINITY || v == -INFINITY) n = snprintf(s, sizeof s, "%s", v > 0 ? "inf" : "-inf");
    else {
        n = 0;
        for (int digits = 1; digits <= 17; digits++) { /* the shortest that reads back, like print */
            n = snprintf(s, sizeof s, "%.*g", digits, v);
            if (strtod(s, NULL) != v) continue;
            double size = fabs(v);
            if (strchr(s, 'e') && size >= 1e-4 && size < 1e16) {
                int decimals = digits - 1 - (int)floor(log10(size));
                n = snprintf(s, sizeof s, "%.*f", decimals > 0 ? decimals : 0, v);
            }
            break;
        }
        if (!strpbrk(s, ".eEn")) n += snprintf(s + n, sizeof s - (size_t)n, ".0");
    }
    return rtText(s, n);
}

static void* rtGrow(void* ptr, int64_t oldCount, int64_t newCount, int64_t bytesEach) {
    void* p = rtAlloc(newCount, bytesEach);
    memcpy(p, ptr, (size_t)(oldCount * bytesEach));
    return p;
}

static void* rtCopy(void* ptr, int64_t count, int64_t bytesEach) {
    void* p = rtAlloc(count, bytesEach);
    if (count) memcpy(p, ptr, (size_t)(count * bytesEach));
    return p;
}

static double rtNow(void) {
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    LARGE_INTEGER now;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
#endif
}

static double rtSin(double x) { return sin(x); }
static double rtCos(double x) { return cos(x); }
static double rtTan(double x) { return tan(x); }
static double rtAsin(double x) { return asin(x); }
static double rtAcos(double x) { return acos(x); }
static double rtAtan(double x) { return atan(x); }
static double rtAtan2(double y, double x) { return atan2(y, x); }
static double rtExp(double x) { return exp(x); }
static double rtLog(double x) { return log(x); }
static double rtPow(double x, double y) { return pow(x, y); }
static void rtFuelOut(int64_t line) { rtPanic(PANIC_FUEL, line, 0, 0); }
static void rtPrintHandle(const char* name, int64_t len, int64_t v) { printf("%.*s#%lld", (int)len, name, (long long)v); }

const RtFn qkRuntime[RT_COUNT] = {
    [RT_PANIC] = {"panic", (void*)rtPanic, "IIII", 'V'},
    [RT_PRINT_INT] = {"printInt", (void*)rtPrintInt, "I", 'V'},
    [RT_PRINT_FLOAT] = {"printFloat", (void*)rtPrintFloat, "F", 'V'},
    [RT_PRINT_BOOL] = {"printBool", (void*)rtPrintBool, "I", 'V'},
    [RT_PRINT_TEXT] = {"printText", (void*)rtPrintText, "II", 'V'},
    [RT_PRINT_SEP] = {"printSep", (void*)rtPrintSep, "", 'V'},
    [RT_PRINT_END] = {"printEnd", (void*)rtPrintEnd, "", 'V'},
    [RT_ALLOC] = {"alloc", (void*)rtAlloc, "II", 'I'},
    [RT_GROW] = {"grow", (void*)rtGrow, "IIII", 'I'},
    [RT_COPY] = {"copy", (void*)rtCopy, "III", 'I'},
    [RT_NOW] = {"now", (void*)rtNow, "", 'F'},
    [RT_SIN] = {"sin", (void*)rtSin, "F", 'F'},
    [RT_COS] = {"cos", (void*)rtCos, "F", 'F'},
    [RT_TAN] = {"tan", (void*)rtTan, "F", 'F'},
    [RT_ASIN] = {"asin", (void*)rtAsin, "F", 'F'},
    [RT_ACOS] = {"acos", (void*)rtAcos, "F", 'F'},
    [RT_ATAN] = {"atan", (void*)rtAtan, "F", 'F'},
    [RT_ATAN2] = {"atan2", (void*)rtAtan2, "FF", 'F'},
    [RT_EXP] = {"exp", (void*)rtExp, "F", 'F'},
    [RT_LOG] = {"log", (void*)rtLog, "F", 'F'},
    [RT_POW] = {"pow", (void*)rtPow, "FF", 'F'},
    [RT_FUEL_OUT] = {"fuelOut", (void*)rtFuelOut, "I", 'V'},
    [RT_PRINT_HANDLE] = {"printHandle", (void*)rtPrintHandle, "III", 'V'},
    [RT_TEXT_JOIN] = {"textJoin", (void*)rtTextJoin, "IIII", 'I'},
    [RT_TEXT_COMPARE] = {"textCompare", (void*)rtTextCompare, "IIII", 'I'},
    [RT_TEXT_OF_INT] = {"textOfInt", (void*)rtTextOfInt, "I", 'I'},
    [RT_TEXT_OF_FLOAT] = {"textOfFloat", (void*)rtTextOfFloat, "F", 'I'},
    [RT_PUSH] = {"push", (void*)rtPush, "III", 'I'},
};

/* ---- loading machine code ------------------------------------------------------------------------ */

bool qkLoad(const uint8_t* code, size_t codeLen, const uint8_t* data, size_t dataLen, const Reloc* relocs, int relocCount,
            int32_t enter, int32_t escape, void* const* hosts, Loaded* out) {
    memset(out, 0, sizeof *out);
    uint8_t* dataCopy = malloc(dataLen + 16);
    if (!dataCopy) return false;
    memcpy(dataCopy, data, dataLen);
#ifdef _WIN32
    uint8_t* mem = VirtualAlloc(NULL, codeLen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    uint8_t* mem = mmap(NULL, codeLen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) mem = NULL;
#endif
    if (!mem) {
        free(dataCopy);
        return false;
    }
    memcpy(mem, code, codeLen);
    for (int i = 0; i < relocCount; i++) {
        uint64_t address = 0;
        switch (relocs[i].kind) {
        case RELOC_RUNTIME: address = (uint64_t)(uintptr_t)qkRuntime[relocs[i].target].addr; break;
        case RELOC_DATA: address = (uint64_t)(uintptr_t)(dataCopy + relocs[i].target); break;
        case RELOC_FUEL: address = (uint64_t)(uintptr_t)&qkFuel; break;
        case RELOC_HOST: address = (uint64_t)(uintptr_t)(hosts ? hosts[relocs[i].target] : NULL); break;
        case RELOC_ESCAPE: address = (uint64_t)(uintptr_t)&qkEscapeRsp; break;
        }
        memcpy(mem + relocs[i].at, &address, 8);
    }
#ifdef _WIN32
    DWORD old;
    VirtualProtect(mem, codeLen, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), mem, codeLen);
#else
    mprotect(mem, codeLen, PROT_READ | PROT_EXEC);
#endif
    out->code = mem;
    out->codeLen = codeLen;
    out->data = dataCopy;
    void* at = mem + enter;
    memcpy(&out->enter, &at, sizeof at);
    at = mem + escape;
    memcpy(&out->escape, &at, sizeof at);
    return true;
}

void qkUnload(Loaded* l) {
    if (l->code) {
#ifdef _WIN32
        VirtualFree(l->code, 0, MEM_RELEASE);
#else
        munmap(l->code, l->codeLen);
#endif
    }
    free(l->data);
    memset(l, 0, sizeof *l);
}

bool qkRunEntry(const Loaded* l, void* fn, const uint64_t* args, int argCount, uint64_t* results, int resultCount) {
    if (!qkEscapeRsp) qkArenaReset(); /* the outermost call: what the last one allocated goes */
    void (*savedStub)(void) = qkEscapeStub;
    qkEscapeStub = l->escape;
    qkPanicText[0] = '\0';
    QkEnter e = {fn, args, argCount, results, resultCount};
    int64_t stopped = l->enter(&e);
    qkEscapeStub = savedStub;
    return stopped == 0;
}
