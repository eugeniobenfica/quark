/* Quark — the runtime compiled code calls: printing, memory, time, the math library, and stopping with a
 * message. Memory comes from an arena: a program allocates by bumping a pointer and frees everything when it
 * ends (a host would reset the arena after each call). Also the loader that puts machine code in memory. */
#include "quark.h"

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

static void rtPanic(int64_t code, int64_t line, int64_t a, int64_t b) {
    fflush(stdout);
    switch (code) {
    case PANIC_INDEX:
        fprintf(stderr, "%s:%lld: index %lld is outside the array (its length is %lld)\n", qkRunPath, (long long)line, (long long)a, (long long)b);
        break;
    case PANIC_DIVIDE: fprintf(stderr, "%s:%lld: division by zero\n", qkRunPath, (long long)line); break;
    case PANIC_FUEL: fprintf(stderr, "%s:%lld: the program ran too long (its budget ran out)\n", qkRunPath, (long long)line); break;
    case PANIC_HISTOGRAM:
        fprintf(stderr, "%s:%lld: histogram: bin %lld is outside 0..%lld\n", qkRunPath, (long long)line, (long long)a, (long long)b);
        break;
    case PANIC_EMPTY: fprintf(stderr, "%s:%lld: min / max of nothing (the list is empty)\n", qkRunPath, (long long)line); break;
    case PANIC_SIZE: fprintf(stderr, "%s:%lld: an array of %lld elements cannot be made\n", qkRunPath, (long long)line, (long long)a); break;
    default: fprintf(stderr, "%s:%lld: stopped (%lld)\n", qkRunPath, (long long)line, (long long)code);
    }
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

static char* arena;
static size_t arenaLeft;

static void* rtAlloc(int64_t count, int64_t bytesEach) {
    if (count < 0 || (count && bytesEach > (INT64_C(1) << 40) / count)) rtPanic(PANIC_SIZE, 0, count, 0);
    size_t bytes = ((size_t)(count * bytesEach) + 15) & ~(size_t)15;
    if (bytes == 0) bytes = 16;
    if (bytes > arenaLeft) {
        size_t size = bytes > ((size_t)64 << 20) ? bytes : ((size_t)64 << 20);
        arena = calloc(1, size);
        if (!arena) {
            fprintf(stderr, "%s: out of memory\n", qkRunPath);
            exit(1);
        }
        arenaLeft = size;
    }
    void* p = arena;
    arena += bytes;
    arenaLeft -= bytes;
    return p;
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
};

/* ---- loading machine code ------------------------------------------------------------------------ */

void (*qkLoad(const uint8_t* code, size_t codeLen, const uint8_t* data, size_t dataLen, const Reloc* relocs,
              int relocCount, int32_t entry))(void) {
    uint8_t* dataCopy = malloc(dataLen + 16);
    if (!dataCopy) return NULL;
    memcpy(dataCopy, data, dataLen);
#ifdef _WIN32
    uint8_t* mem = VirtualAlloc(NULL, codeLen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    uint8_t* mem = mmap(NULL, codeLen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    if (!mem) return NULL;
    memcpy(mem, code, codeLen);
    for (int i = 0; i < relocCount; i++) {
        uint64_t address = 0;
        if (relocs[i].kind == 0) address = (uint64_t)(uintptr_t)qkRuntime[relocs[i].target].addr;
        else if (relocs[i].kind == 1) address = (uint64_t)(uintptr_t)(dataCopy + relocs[i].target);
        else address = (uint64_t)(uintptr_t)&qkFuel;
        memcpy(mem + relocs[i].at, &address, 8);
    }
#ifdef _WIN32
    DWORD old;
    VirtualProtect(mem, codeLen, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), mem, codeLen);
#else
    mprotect(mem, codeLen, PROT_READ | PROT_EXEC);
#endif
    void (*fn)(void);
    void* at = mem + entry;
    memcpy(&fn, &at, sizeof fn);
    return fn;
}
