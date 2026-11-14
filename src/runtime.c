/* Quark — the runtime compiled code calls: printing, memory, time, the math library, and stopping with a
 * message. Memory comes from an arena: a program allocates by bumping a pointer and frees everything when it
 * ends (a host would reset the arena after each call). Also the loader that puts machine code in memory. */
#define _DEFAULT_SOURCE /* POSIX: mmap's MAP_ANONYMOUS, opendir, clock_gettime */
#define _GNU_SOURCE     /* Linux: pthread_getattr_np, for the stack's bounds */
#define _DARWIN_C_SOURCE
#include "qk.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0602
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0602 /* GetCurrentThreadStackLimits */
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#endif
#include <time.h>

bool qkUseAvx;
#ifdef __APPLE__
#define QK_SYM(name) "_" name
#else
#define QK_SYM(name) name
#endif

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
char** qkProgramArgs;
int qkProgramArgCount;


/* ---- the stack of calls, when a program stops ------------------------------------------------------ */

struct Trace {
    char** names;     /* each function's */
    int32_t* fnAt;
    int fnCount;
    CallSite* sites;
    int siteCount;
    InlineCall* inlines;
    char** inlineNames;
    int inlineCount;
};

#define running (qkCtx->running)
#define runningEntry (qkCtx->runningEntry)

static int functionAt(const struct Trace* t, int32_t offset) {
    for (int i = 0; i < t->fnCount; i++)
        if (t->fnAt[i] == offset) return i;
    return -1;
}

static void add(char* m, size_t size, size_t* n, const char* fmt, const char* name, int line) {
    if (*n >= size) return;
    *n += (size_t)snprintf(m + *n, size - *n, fmt, name, line);
    if (*n >= size) *n = size - 1;
}

/* code of function fn, inside the inlined calls inl: "called from g at line 12" for each one, outwards */
static void addInlined(char* m, size_t size, size_t* n, const struct Trace* t, int inl, const char* fn) {
    while (inl > 0 && inl < t->inlineCount) {
        const InlineCall* c = &t->inlines[inl];
        const char* outer = c->parent > 0 ? t->inlineNames[c->parent] : fn;
        add(m, size, n, "\n  called from %s at line %d", outer, c->line);
        inl = c->parent;
    }
}

/* "  in pick, called from middle at line 12, called from main at line 20": from where it stopped outwards,
 * through inlined calls (which have no frames) and real ones (every Quark function keeps rbp). */
static bool sameCall(const CallSite* a, const CallSite* b) { return a->caller == b->caller && a->line == b->line; }

static void appendTrace(char* m, size_t size, int inl) {
    const Loaded* l = running;
    if (!l || !l->trace || !qkPanicFrame) return;
    const struct Trace* t = l->trace;
    int64_t* frame = qkPanicFrame;
    qkPanicFrame = NULL;
    size_t n = strlen(m);
    const CallSite* calls[16];
    int depth = 0;
    for (; frame && depth < 16; frame = (int64_t*)(intptr_t)frame[0]) { /* the frames, out to the way in from C */
        uintptr_t ret = (uintptr_t)frame[1];
        if (ret < (uintptr_t)l->code || ret >= (uintptr_t)l->code + l->codeLen) break;
        int32_t at = (int32_t)(ret - (uintptr_t)l->code);
        const CallSite* s = NULL;
        for (int i = 0; i < t->siteCount && !s; i++)
            if (t->sites[i].ret == at) s = &t->sites[i];
        if (!s) break;
        calls[depth++] = s;
    }
    if (!depth && inl <= 0) return; /* it stopped in the function the call began with: nothing to add */
    int entry = functionAt(t, runningEntry);
    const char* here = depth ? (calls[0]->callee >= 0 ? t->names[calls[0]->callee] : "a function kept as a value")
                             : entry >= 0 ? t->names[entry] : "?";
    add(m, size, &n, "\n  in %s%.0d", inl > 0 && inl < t->inlineCount ? t->inlineNames[inl] : here, 0);
    addInlined(m, size, &n, t, inl, here);
    for (int i = 0; i < depth; i++) {
        const CallSite* s = calls[i];
        const char* caller = t->names[s->caller];
        if (i >= 2 && sameCall(s, calls[i - 1]) && sameCall(s, calls[i - 2])) { /* the same call again and again: said once */
            while (i + 1 < depth && sameCall(calls[i + 1], s)) i++;
            add(m, size, &n, "\n  ... called from %s at line %d, again and again", caller, s->line);
            continue;
        }
        add(m, size, &n, "\n  called from %s at line %d", s->inl > 0 && s->inl < t->inlineCount ? t->inlineNames[s->inl] : caller, s->line);
        addInlined(m, size, &n, t, s->inl, caller);
    }
}

void qkLoadTrace(Loaded* l, const MachineCode* mc, const IrProgram* ir) {
    struct Trace* t = calloc(1, sizeof *t);
    t->fnCount = ir->count;
    t->names = calloc((size_t)(ir->count ? ir->count : 1), sizeof(char*));
    t->fnAt = calloc((size_t)(ir->count ? ir->count : 1), sizeof(int32_t));
    for (int i = 0; i < ir->count; i++) {
        size_t n = strlen(ir->fns[i].name);
        t->names[i] = malloc(n + 1);
        memcpy(t->names[i], ir->fns[i].name, n + 1);
        t->fnAt[i] = mc->fnAt[i];
    }
    t->siteCount = mc->siteCount;
    t->sites = malloc(sizeof(CallSite) * (size_t)(mc->siteCount ? mc->siteCount : 1));
    if (mc->siteCount) memcpy(t->sites, mc->sites, sizeof(CallSite) * (size_t)mc->siteCount);
    t->inlineCount = ir->inlineCount;
    t->inlines = malloc(sizeof(InlineCall) * (size_t)(ir->inlineCount ? ir->inlineCount : 1));
    t->inlineNames = calloc((size_t)(ir->inlineCount ? ir->inlineCount : 1), sizeof(char*));
    for (int i = 0; i < ir->inlineCount; i++) {
        t->inlines[i] = ir->inlines[i];
        size_t n = strlen(ir->inlines[i].callee);
        t->inlineNames[i] = malloc(n + 1);
        memcpy(t->inlineNames[i], ir->inlines[i].callee, n + 1);
        t->inlines[i].callee = t->inlineNames[i];
    }
    l->trace = t;
}

static void freeTrace(struct Trace* t) {
    if (!t) return;
    for (int i = 0; i < t->fnCount; i++) free(t->names[i]);
    free(t->names);
    free(t->fnAt);
    free(t->sites);
    for (int i = 0; i < t->inlineCount; i++) free(t->inlineNames[i]);
    free(t->inlineNames);
    free(t->inlines);
    free(t);
}

/* The message, then: back to the host's call (the trampoline returns 1), or the end of the process. */
static void rtPanic(int64_t code, int64_t line, int64_t a, int64_t b) {
    int inl = (int)(line >> 32); /* the inlined calls it is in ride above the line */
    line &= 0xFFFFFFFF;
    char* m = qkPanicText;
    size_t n = sizeof qkPanicText;
    long long L = (long long)line, A = (long long)a, B = (long long)b;
    switch (code) {
    case PANIC_INDEX: snprintf(m, n, "%s:%lld: index %lld is outside the array (its length is %lld)", qkRunPath, L, A, B); break;
    case PANIC_DIVIDE: snprintf(m, n, "%s:%lld: division by zero", qkRunPath, L); break;
    case PANIC_FUEL: snprintf(m, n, "%s:%lld: the program ran too long (its budget ran out)", qkRunPath, L); break;
    case PANIC_EMPTY: snprintf(m, n, "%s:%lld: min, max, minBy, maxBy or mean of nothing (the list is empty)", qkRunPath, L); break;
    case PANIC_SIZE: snprintf(m, n, "%s:%lld: an array of %lld elements cannot be made", qkRunPath, L, A); break;
    case PANIC_SLICE_END: snprintf(m, n, "%s:%lld: a slice to %lld goes past the end (the length is %lld)", qkRunPath, L, A, B - 1); break;
    case PANIC_SLICE_START: snprintf(m, n, "%s:%lld: a slice from %lld starts after its end %lld", qkRunPath, L, A, B - 1); break;
    case PANIC_TEXT_CUT: snprintf(m, n, "%s:%lld: a slice at byte %lld cuts a character in two (texts are UTF-8: chars(t) gives the characters)", qkRunPath, L, A - 1); break;
    case PANIC_EMPTY_RANGE: snprintf(m, n, "%s:%lld: randomInt(%lld, %lld) has no Int to give (the end is not included)", qkRunPath, L, A, B); break;
    case PANIC_TOO_BIG: snprintf(m, n, "%s:%lld: this Float is too big for an Int (or not a number)", qkRunPath, L); break;
    case PANIC_TASK: snprintf(m, n, "%s:%lld: there is no task %lld (step goes on until it is done, then it is gone)", qkRunPath, L, A); break;
    case PANIC_OVERFLOW: snprintf(m, n, "%s:%lld: an Int went past its limits (overflow)", qkRunPath, L); break;
    case PANIC_MEMORY: snprintf(m, n, "%s: the program used more memory than it is given (%lld MB)", qkRunPath, A >> 20); break;
    case PANIC_STACK: snprintf(m, n, "%s:%lld: too many calls deep (the stack is full): does a function call itself without end?", qkRunPath, L); break;
    default: snprintf(m, n, "%s:%lld: stopped (%lld)", qkRunPath, L, (long long)code);
    }
    appendTrace(m, n, inl);
    if (qkEscapeRsp && qkEscapeStub) qkEscapeStub(); /* never returns */
    fflush(stdout);
    fprintf(stderr, "%s\n", m);
    exit(1);
}

static FILE* rtOutFile; /* print's stream: stdout, or stderr while printErr prints */
#define rtOut (rtOutFile ? rtOutFile : stdout)

/* where print writes: the stream, or (a debugger capturing a value's text) a buffer */
static QkBuf* capture;
static void outWrite(const char* p, size_t n) {
    if (capture) qkBufAdd(capture, p, n);
    else fwrite(p, 1, n, rtOut);
}
static void outPuts(const char* s) { outWrite(s, strlen(s)); }

static void rtPrintInt(int64_t v) {
    char s[32];
    snprintf(s, sizeof s, "%lld", (long long)v);
    outPuts(s);
}

static void rtPrintFloat(double v) {
    char s[40];
    if (v != v) {
        outPuts("nan");
        return;
    }
    if (v == INFINITY || v == -INFINITY) {
        outPuts(v > 0 ? "inf" : "-inf");
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
    outPuts(s);
    if (!strpbrk(s, ".eEn")) outPuts(".0");
}

static void rtPrintBool(int64_t v) { outPuts(v ? "true" : "false"); }
static void rtPrintText(const char* p, int64_t n) { outWrite(p, (size_t)n); }
static void rtPrintSep(void) { outPuts(" "); }
static void rtPrintEnd(void) { outPuts("\n"); }
static void rtPrintTo(int64_t stream) { /* 2: stderr (printErr), 1: stdout */
    fflush(stdout);
    rtOutFile = stream == 2 ? stderr : stdout;
}

/* ---- the arena ------------------------------------------------------------------------------------
 * Memory comes from blocks, by bumping a pointer: blocks after the current one are empty, so the list is in
 * the order things were made, and going back to a mark frees everything made after it at once. A loop that
 * makes things marks where it starts; when its turns have made enough, the values its vars hold (and only
 * those) are copied out to a survivor block and the arena goes back to the mark. So a program that runs for
 * days in a loop uses the memory its live values need, with no collector walking the heap. */

typedef struct ArenaBlock {
    struct ArenaBlock* next;
    size_t size, used;
    char* data; /* 16-aligned */
} ArenaBlock;

/* Survivors: what loops copied out, outside the blocks. Each knows when it was made. */
typedef struct Extra {
    struct Extra* next;
    int64_t time;
    size_t size, used;
    char* data;
} Extra;

/* Arrays that grow (push): each one's room, so the next push writes in place. */
typedef struct Growing {
    char* ptr;
    int64_t capBytes;
} Growing;

struct QkArena {
    ArenaBlock* blocks;  /* the first */
    ArenaBlock* current; /* where allocation is */
    Extra* extras;
    int64_t clock;       /* counts survivors and marks: what came later has a larger time */
    int64_t allocated;   /* bytes handed out, ever: how much a loop made since its mark */
    Growing growing[64];
    int growingNext;
    char* lastText;      /* the last text made: s + t appends to it in place when it is still the arena's top */
    int64_t reserved;    /* bytes of blocks held */
    int64_t limit;       /* at most this many (0: no limit) */
};

void qkArenaLimit(QkArena* a, int64_t bytes) {
    if (a) a->limit = bytes > 0 ? bytes : 0;
}

static QkArena mainArena;
QkCtx qkMainCtx = {.fuel = INT64_MAX, .arena = &mainArena};
_Thread_local QkCtx* qkCtx = &qkMainCtx;

QkArena* qkArenaNew(void) { return calloc(1, sizeof(QkArena)); }

static void freeExtras(QkArena* a, int64_t after, int64_t before) { /* those made in (after, before) */
    Extra** at = &a->extras;
    while (*at) {
        Extra* x = *at;
        if (x->time > after && x->time < before) {
            *at = x->next;
            free(x);
        } else {
            at = &x->next;
        }
    }
}

void qkArenaFree(QkArena* a) {
    if (!a) return;
    for (ArenaBlock* b = a->blocks; b;) {
        ArenaBlock* next = b->next;
        free(b);
        b = next;
    }
    freeExtras(a, INT64_MIN, INT64_MAX);
    if (a != &mainArena) free(a);
    else memset(a, 0, sizeof *a);
}

QkArena* qkArenaUse(QkArena* a) {
    QkArena* was = qkArena;
    qkArena = a ? a : &mainArena;
    return was;
}

static void* rtAlloc(int64_t count, int64_t bytesEach) {
    QkArena* a = qkArena;
    if (count < 0 || (count && bytesEach > (INT64_C(1) << 40) / count)) rtPanic(PANIC_SIZE, 0, count, 0);
    size_t bytes = ((size_t)(count * bytesEach) + 15) & ~(size_t)15;
    if (bytes == 0) bytes = 16;
    while (a->current && a->current->used + bytes > a->current->size) {
        if (!a->current->next || a->current->next->size < bytes) break;
        a->current = a->current->next; /* blocks after the current one are empty and zeroed */
    }
    if (!a->current || a->current->used + bytes > a->current->size) {
        size_t size = bytes > ((size_t)16 << 20) ? bytes : ((size_t)16 << 20);
        if (a->limit && a->reserved + (int64_t)size > a->limit) {
            if (a->reserved + (int64_t)bytes > a->limit) rtPanic(PANIC_MEMORY, 0, a->limit, 0);
            size = bytes; /* the last room the limit leaves */
        }
        ArenaBlock* blk = calloc(1, sizeof(ArenaBlock) + size + 16);
        if (!blk) {
            snprintf(qkPanicText, sizeof qkPanicText, "%s: out of memory", qkRunPath);
            rtPanic(PANIC_SIZE, 0, count, 0);
        }
        blk->size = size;
        a->reserved += (int64_t)size;
        blk->data = (char*)(((uintptr_t)(blk + 1) + 15) & ~(uintptr_t)15);
        if (a->current) { /* in after the current block */
            blk->next = a->current->next;
            a->current->next = blk;
        } else {
            a->blocks = blk;
        }
        a->current = blk;
    }
    void* p = a->current->data + a->current->used;
    a->current->used += bytes;
    a->allocated += (int64_t)bytes;
    return p;
}

/* Back to a mark (block, used): what was made after it goes, zeroed again (allocations are zeroed). */
static void resetTo(QkArena* a, ArenaBlock* block, size_t used) {
    ArenaBlock* from = block ? block->next : a->blocks;
    for (ArenaBlock* b = from; b && b->used; b = b->next) {
        memset(b->data, 0, b->used);
        b->used = 0;
    }
    if (block) {
        memset(block->data + used, 0, block->used - used);
        block->used = used;
    }
    a->current = block ? block : a->blocks;
    memset(a->growing, 0, sizeof a->growing); /* the room they knew may be gone */
}

/* Everything the last call allocated goes; the memory stays. */
void qkArenaReset(void) {
    resetTo(qkArena, NULL, 0);
    freeExtras(qkArena, INT64_MIN, INT64_MAX);
}


/* xs.push(v): the array's address, with room for one more (in place when it has room, else moved to twice
 * the room). The caller stores v at index len and adds 1 to len. */
static void* rtPush(char* ptr, int64_t len, int64_t bytesEach) {
    Growing* growing = qkArena->growing;
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
    int next = qkArena->growingNext;
    growing[next].ptr = p;
    growing[next].capBytes = cap * bytesEach;
    qkArena->growingNext = (next + 1) % 64;
    return p;
}

/* ---- text: made in the arena as [length: 8 bytes][bytes]; the code reads both ---------------------- */

static char* rtText(const char* bytes, int64_t len) {
    char* p = rtAlloc(len + 8, 1);
    qkArena->lastText = p;
    memcpy(p, &len, 8);
    if (len) memcpy(p + 8, bytes, (size_t)len);
    return p;
}

/* s + t where s is the last text the arena made, and nothing came after it: t goes on its end, in place, so a
 * text built one piece at a time costs what its length does (no copy each time). Nothing else can see the
 * bytes past s's end, and s's own bytes do not change. */
static char* appendInPlace(const char* a, int64_t la, const char* b, int64_t lb) {
    QkArena* ar = qkArena;
    ArenaBlock* cur = ar->current;
    char* p = ar->lastText;
    if (!cur || !p || p + 8 != a || la <= 0) return NULL;
    int64_t had;
    memcpy(&had, p, 8);
    size_t oldBytes = ((size_t)(had + 8) + 15) & ~(size_t)15, newBytes = ((size_t)(la + lb + 8) + 15) & ~(size_t)15;
    if (had != la || p + oldBytes != cur->data + cur->used || cur->used - oldBytes + newBytes > cur->size) return NULL;
    cur->used += newBytes - oldBytes;
    ar->allocated += (int64_t)(newBytes - oldBytes);
    memmove(p + 8 + la, b, (size_t)lb);
    int64_t n = la + lb;
    memcpy(p, &n, 8);
    return p;
}

static char* rtTextJoin(const char* a, int64_t la, const char* b, int64_t lb) {
    char* q = appendInPlace(a, la, b, lb);
    if (q) return q;
    char* p = rtAlloc(la + lb + 8, 1);
    qkArena->lastText = p;
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

/* ---- parallelMap: the elements shared out to threads ---------------------------------------------- */

static void rtStop(void);

typedef struct Worker {
    const Loaded* from;
    int64_t entry;          /* the function's offset in the code */
    int64_t env;            /* its captured values, after the element's slots */
    const int64_t* in;
    int64_t* out;
    int64_t first, last;
    int64_t inSlots, outSlots;
    int64_t fuel;
    bool failed;
    char text[512];
} Worker;

/* A worker: a context of its own, the code loaded again (its addresses are this thread's), f on its elements. */
static void runWorker(Worker* w) {
    QkCtx ctx = {0};
    ctx.fuel = w->fuel;
    ctx.arena = qkArenaNew();
    qkCtx = &ctx;
    const Loaded* l = w->from;
    Loaded copy;
    bool loaded = !l->linked && l->source && qkLoad(l->source, l->codeLen, l->source + l->codeLen, l->dataLen, l->relocs, l->relocCount,
                                      l->enterAt, l->escapeAt, l->hosts, &copy);
    if (!loaded) {
        w->failed = true;
        snprintf(w->text, sizeof w->text, "parallelMap: a thread could not load the program");
    } else {
        copy.trace = l->trace; /* the same offsets: the original's names and calls */
        copy.copy = true;
        int64_t args[64];
        for (int64_t i = w->first; i < w->last && !w->failed; i++) {
            memcpy(args, w->in + i * w->inSlots, (size_t)w->inSlots * 8);
            args[w->inSlots] = w->env;
            if (!qkRunEntry(&copy, copy.code + w->entry, (const uint64_t*)args, (int)w->inSlots + 1,
                            (uint64_t*)(w->out + i * w->outSlots), (int)w->outSlots)) {
                w->failed = true;
                memcpy(w->text, ctx.panicText, sizeof w->text);
            }
        }
        qkUnload(&copy);
    }
    qkArenaFree(ctx.arena);
    qkCtx = &qkMainCtx;
}

#ifdef _WIN32
static DWORD WINAPI workerThread(LPVOID p) {
    runWorker(p);
    return 0;
}
static int processorCount(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}
#else
static void* workerThread(void* p) {
    runWorker(p);
    return NULL;
}
static int processorCount(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}
#endif

static void* rtParallelMap(void* fn, int64_t env, const int64_t* in, int64_t n, int64_t inSlots, int64_t outSlots, int64_t line) {
    if (inSlots > 63) rtPanic(PANIC_SIZE, line, inSlots, 0);
    int64_t* out = rtAlloc(n, outSlots * 8);
    const Loaded* l = running;
    int threads = processorCount();
    if (threads > 64) threads = 64;
    if (n < 2 * threads) threads = n > 1 ? (int)n : 1; /* few elements: fewer threads */
    if (!l || threads <= 1 || n < 2 || l->linked || qkDebugger.interactive || qkDebugger.tracePath) { /* a debugger: one thread */ /* here, one after the other (linked code is not loaded again) */
        int64_t args[64];
        for (int64_t i = 0; i < n; i++) {
            memcpy(args, in + i * inSlots, (size_t)inSlots * 8);
            args[inSlots] = env;
            if (!qkRunEntry(l, fn, (const uint64_t*)args, (int)inSlots + 1, (uint64_t*)(out + i * outSlots), (int)outSlots)) rtStop();
        }
        return out;
    }
    Worker* w = calloc((size_t)threads, sizeof(Worker));
    if (!w) rtPanic(PANIC_SIZE, line, n, 0);
    int64_t each = (n + threads - 1) / threads;
    for (int t = 0; t < threads; t++)
        w[t] = (Worker){l, (int64_t)((uint8_t*)fn - l->code), env, in, out, t * each, (t + 1) * each < n ? (t + 1) * each : n, inSlots, outSlots,
                        qkFuel, false, {0}};
#ifdef _WIN32
    HANDLE* h = calloc((size_t)threads, sizeof(HANDLE));
    for (int t = 0; t < threads; t++) h[t] = CreateThread(NULL, 64 << 20, workerThread, &w[t], STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    for (int t = 0; t < threads; t++) {
        if (h[t]) {
            WaitForSingleObject(h[t], INFINITE);
            CloseHandle(h[t]);
        } else {
            runWorker(&w[t]);
        }
    }
    free(h);
#else
    pthread_t* h = calloc((size_t)threads, sizeof(pthread_t));
    bool* started = calloc((size_t)threads, sizeof(bool));
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 << 20);
    for (int t = 0; t < threads; t++) started[t] = !pthread_create(&h[t], &attr, workerThread, &w[t]);
    for (int t = 0; t < threads; t++) {
        if (started[t]) pthread_join(h[t], NULL);
        else runWorker(&w[t]);
    }
    pthread_attr_destroy(&attr);
    free(started);
    free(h);
#endif
    for (int t = 0; t < threads; t++)
        if (w[t].failed) {
            memcpy(qkPanicText, w[t].text, sizeof w[t].text);
            free(w);
            rtStop();
        }
    free(w);
    return out;
}

/* xs + ys: a new array, xs's elements then ys's */
static void* rtArrayJoin(void* a, int64_t an, void* b, int64_t bn, int64_t bytesEach) {
    char* p = rtAlloc(an + bn, bytesEach);
    if (an) memcpy(p, a, (size_t)(an * bytesEach));
    if (bn) memcpy(p + an * bytesEach, b, (size_t)(bn * bytesEach));
    return p;
}

/* ---- random numbers: xoshiro256**, seeded from the clock the first time, or by seedRandom ---- */

static uint64_t rng[4];
static bool rngSeeded;

static uint64_t splitmix(uint64_t* x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static void rtSeedRandom(int64_t seed) {
    uint64_t x = (uint64_t)seed;
    for (int i = 0; i < 4; i++) rng[i] = splitmix(&x);
    rngSeeded = true;
}

static uint64_t nextRandom(void) {
    if (!rngSeeded) rtSeedRandom((int64_t)time(NULL) ^ (int64_t)(intptr_t)&rngSeeded);
    uint64_t r = rng[1] * 5;
    r = (r << 7 | r >> 57) * 9;
    uint64_t t = rng[1] << 17;
    rng[2] ^= rng[0];
    rng[3] ^= rng[1];
    rng[1] ^= rng[2];
    rng[0] ^= rng[3];
    rng[2] ^= t;
    rng[3] = rng[3] << 45 | rng[3] >> 19;
    return r;
}

static double rtRandom(void) { return (double)(nextRandom() >> 11) * (1.0 / 9007199254740992.0); }

static void rtPanic(int64_t code, int64_t line, int64_t a, int64_t b);

static int64_t rtRandomInt(int64_t lo, int64_t hi, int64_t line) {
    if (hi <= lo) rtPanic(PANIC_EMPTY_RANGE, line, lo, hi);
    uint64_t span = (uint64_t)hi - (uint64_t)lo, limit = UINT64_MAX - UINT64_MAX % span, r;
    do r = nextRandom(); while (r >= limit); /* no bias toward the low numbers */
    return (int64_t)((uint64_t)lo + r % span);
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

/* ---- the outside world, and text ------------------------------------------------------------------ */

/* An optional, as the code reads it: [1 or 0][the value's slots]. */
static int64_t* rtOptional(int64_t has, int64_t a, int64_t b) {
    int64_t* p = rtAlloc(3, 8);
    p[0] = has;
    p[1] = has ? a : 0;
    p[2] = has ? b : 0;
    return p;
}

/* A text as a C string (paths), in the arena. */
static char* rtCString(const char* p, int64_t n) {
    char* s = rtAlloc(n + 1, 1);
    if (n) memcpy(s, p, (size_t)n);
    return s;
}

/* A result, as the code reads it: [1 or 0][the message (bytes, length)][the value's two slots]. */
static int64_t* rtResult(bool ok, int64_t a, int64_t b, const char* message) {
    int64_t* p = rtAlloc(5, 8);
    p[0] = ok;
    if (!ok) {
        int64_t n = (int64_t)strlen(message);
        char* m = rtAlloc(n + 1, 1);
        memcpy(m, message, (size_t)n);
        p[1] = (int64_t)(intptr_t)m;
        p[2] = n;
    }
    p[3] = a;
    p[4] = b;
    return p;
}

static int64_t* rtCannot(const char* what, const char* path) {
    char m[600];
    snprintf(m, sizeof m, "cannot %s %s: %s", what, path, strerror(errno));
    return rtResult(false, 0, 0, m);
}

/* readFile(path) -> Text!: Err says why it cannot be read. */
static int64_t* rtReadFile(const char* path, int64_t pathLen) {
    const char* name = rtCString(path, pathLen);
    FILE* f = fopen(name, "rb");
    if (!f) return rtCannot("read", name);
    QkBuf b = {0};
    char chunk[1 << 14];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) qkBufAdd(&b, chunk, got);
    bool failed = ferror(f);
    fclose(f);
    if (failed) {
        qkBufFree(&b);
        return rtCannot("read", name);
    }
    char* bytes = rtCString(b.data, (int64_t)b.len);
    int64_t n = (int64_t)b.len;
    qkBufFree(&b);
    return rtResult(true, (int64_t)(intptr_t)bytes, n, NULL);
}

/* writeFile(path, text) -> Bool: the whole file, replaced. */
static int64_t rtWriteFile(const char* path, int64_t pathLen, const char* text, int64_t textLen) {
    return qkWriteFile(rtCString(path, pathLen), text, (size_t)textLen);
}

/* readLine() -> Text?: the next line of the input, without its line break; None at the end. */
static int64_t* rtReadLine(void) {
    fflush(stdout); /* what was printed before the question shows first */
    QkBuf b = {0};
    int c;
    bool any = false;
    while ((c = fgetc(stdin)) != EOF) {
        any = true;
        if (c == '\n') break;
        qkBufByte(&b, (uint8_t)c);
    }
    if (!any) return rtOptional(0, 0, 0);
    if (b.len && b.data[b.len - 1] == '\r') b.len--;
    char* bytes = rtCString(b.data, (int64_t)b.len);
    int64_t n = (int64_t)b.len;
    qkBufFree(&b);
    return rtOptional(1, (int64_t)(intptr_t)bytes, n);
}

/* An array of texts as the code reads it: [count][(bytes, length)...]. */
static int64_t* rtTextArray(int64_t count) {
    int64_t* p = rtAlloc(1 + 2 * count, 8);
    p[0] = count;
    return p;
}

/* args() -> [Text]: the command line after the program. */
static int64_t* rtArgs(void) {
    int64_t* p = rtTextArray(qkProgramArgCount);
    for (int i = 0; i < qkProgramArgCount; i++) {
        p[1 + 2 * i] = (int64_t)(intptr_t)qkProgramArgs[i];
        p[2 + 2 * i] = (int64_t)strlen(qkProgramArgs[i]);
    }
    return p;
}

static bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* The pieces of t between separators; writes them into fill (when given) and returns how many. */
static int64_t splitScan(const char* t, int64_t n, const char* sep, int64_t sn, int64_t* fill) {
    int64_t count = 0, i = 0;
    while (i <= n) {
        int64_t start, end;
        if (sn == 0) { /* words */
            while (i < n && isSpace(t[i])) i++;
            if (i >= n) break;
            start = i;
            while (i < n && !isSpace(t[i])) i++;
            end = i;
        } else {
            start = i;
            end = n;
            for (int64_t k = i; k + sn <= n; k++)
                if (!memcmp(t + k, sep, (size_t)sn)) {
                    end = k;
                    break;
                }
            i = end + sn; /* past the separator; past the end after the last piece */
            if (end == n) i = n + 1;
        }
        if (fill) {
            fill[1 + 2 * count] = (int64_t)(intptr_t)(t + start);
            fill[2 + 2 * count] = end - start;
        }
        count++;
    }
    return count;
}

/* split(text, separator) -> [Text]: the pieces between separators (they share the text's bytes). An empty
 * separator splits at runs of spaces, tabs and line breaks, and keeps no empty piece. */
static int64_t* rtSplit(const char* t, int64_t n, const char* sep, int64_t sn) {
    int64_t* p = rtTextArray(splitScan(t, n, sep, sn, NULL));
    splitScan(t, n, sep, sn, p);
    return p;
}

/* trim(text) -> Text: without spaces, tabs and line breaks at either end. */
static char* rtTrim(const char* t, int64_t n) {
    int64_t a = 0, b = n;
    while (a < b && isSpace(t[a])) a++;
    while (b > a && isSpace(t[b - 1])) b--;
    return rtText(t + a, b - a);
}

/* parseInt(text) -> Int? and parseFloat(text) -> Float?: the whole text is the number, or None. */
static int64_t* rtParseInt(const char* t, int64_t n) {
    char* s = rtCString(t, n);
    char* end;
    errno = 0;
    long long v = n ? strtoll(s, &end, 10) : 0;
    bool ok = n && end == s + n && errno == 0 && !isSpace(s[0]);
    return rtOptional(ok, ok ? (int64_t)v : 0, 0);
}

static int64_t* rtParseFloat(const char* t, int64_t n) {
    char* s = rtCString(t, n);
    char* end;
    double v = n ? strtod(s, &end) : 0;
    bool ok = n && end == s + n && !isSpace(s[0]);
    int64_t bits;
    memcpy(&bits, &v, 8);
    return rtOptional(ok, ok ? bits : 0, 0);
}
static void rtPrintHandle(const char* name, int64_t len, int64_t v) {
    char s[32];
    outWrite(name, (size_t)len);
    snprintf(s, sizeof s, "#%lld", (long long)v);
    outPuts(s);
}

/* ---- the debugger: lines, values, frames ------------------------------------------------------------ */

QkDebugger qkDebugger;

static QkBuf captured;
static void rtCaptureBegin(void) {
    captured.len = 0;
    capture = &captured;
}
static char* rtCaptureEnd(void) { /* the text print wrote, as a text: [length][bytes] */
    capture = NULL;
    size_t n = captured.len > 300 ? 300 : captured.len; /* a long array: its start */
    char* p = rtAlloc((int64_t)n + 8, 1);
    int64_t len = (int64_t)n;
    memcpy(p, &len, 8);
    if (n) memcpy(p + 8, captured.data, n);
    return p;
}

typedef struct DebugVar {
    char* name;
    char* key;   /* the graph's key of its node */
    char* text;  /* its last value, as print writes it */
    int64_t hits;
} DebugVar;

typedef struct DebugFrame {
    char* fn;
    int line;
    DebugVar* vars;
    int varCount, varCap;
} DebugFrame;

static DebugFrame* frames;
static int frameCount, frameCap;
static DebugVar* traced; /* --trace: every name's last value, by key */
static int tracedCount, tracedCap;

static char* dupN(const char* p, int64_t n) {
    char* s = malloc((size_t)n + 1);
    memcpy(s, p, (size_t)n);
    s[n] = 0;
    return s;
}

static void setVar(DebugVar** vars, int* count, int* cap, const char* name, int64_t nn, const char* key, int64_t kn,
                   const char* text, int64_t tn) {
    for (int i = 0; i < *count; i++)
        if ((int64_t)strlen((*vars)[i].key) == kn && !memcmp((*vars)[i].key, key, (size_t)kn)) {
            free((*vars)[i].text);
            (*vars)[i].text = dupN(text, tn);
            (*vars)[i].hits++;
            return;
        }
    if (*count == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *vars = realloc(*vars, sizeof(DebugVar) * (size_t)*cap);
    }
    (*vars)[(*count)++] = (DebugVar){dupN(name, nn), dupN(key, kn), dupN(text, tn), 1};
}

static void rtDebugEnter(const char* fn, int64_t n) {
    if (frameCount == frameCap) {
        frameCap = frameCap ? frameCap * 2 : 32;
        frames = realloc(frames, sizeof(DebugFrame) * (size_t)frameCap);
    }
    frames[frameCount++] = (DebugFrame){dupN(fn, n), 0, NULL, 0, 0};
}

static void dropFrame(DebugFrame* f) {
    for (int i = 0; i < f->varCount; i++) {
        free(f->vars[i].name);
        free(f->vars[i].key);
        free(f->vars[i].text);
    }
    free(f->vars);
    free(f->fn);
}

static void rtDebugLeave(void) {
    if (frameCount) dropFrame(&frames[--frameCount]);
}

static void rtDebugValue(const char* name, int64_t nn, const char* key, int64_t kn, const char* text, int64_t tn) {
    if (frameCount) {
        DebugFrame* f = &frames[frameCount - 1];
        setVar(&f->vars, &f->varCount, &f->varCap, name, nn, key, kn, text, tn);
    }
    if (qkDebugger.tracePath) setVar(&traced, &tracedCount, &tracedCap, name, nn, key, kn, text, tn);
}

static void debugShowLocals(void) {
    if (!frameCount) return;
    DebugFrame* f = &frames[frameCount - 1];
    for (int i = 0; i < f->varCount; i++) fprintf(stderr, "  %s = %s\n", f->vars[i].name, f->vars[i].text);
    if (!f->varCount) fputs("  (no names yet)\n", stderr);
}

static bool isBreak(int line) {
    for (int i = 0; i < qkDebugger.breakCount; i++)
        if (qkDebugger.breaks[i] == line) return true;
    return false;
}

/* a statement starts: stop when it is a breakpoint, or the debugger is stepping; then read commands */
static void rtDebugLine(int64_t line) {
    if (frameCount) frames[frameCount - 1].line = (int)line;
    if (qkDebugger.stopped) {
        if (qkDebugger.stepping || isBreak((int)line)) qkDebugger.stopped((int)line);
        return;
    }
    if (!qkDebugger.interactive || !(qkDebugger.stepping || isBreak((int)line))) return;
    fflush(stdout);
    fprintf(stderr, "stopped at %s:%lld in %s\n", qkRunPath, (long long)line, frameCount ? frames[frameCount - 1].fn : "?");
    debugShowLocals();
    char cmd[256];
    for (;;) {
        fputs("(quark) ", stderr);
        fflush(stderr);
        if (!fgets(cmd, sizeof cmd, stdin)) {
            qkDebugger.interactive = false;
            return;
        }
        char* c = cmd;
        while (*c == ' ') c++;
        size_t n = strlen(c);
        while (n && (c[n - 1] == '\n' || c[n - 1] == '\r' || c[n - 1] == ' ')) c[--n] = 0;
        if (!strcmp(c, "c") || !strcmp(c, "continue")) {
            qkDebugger.stepping = false;
            return;
        }
        if (!strcmp(c, "s") || !strcmp(c, "step") || !strcmp(c, "n") || !n) {
            qkDebugger.stepping = true;
            return;
        }
        if (!strcmp(c, "l") || !strcmp(c, "locals")) debugShowLocals();
        else if (!strcmp(c, "bt") || !strcmp(c, "where")) {
            for (int i = frameCount - 1; i >= 0; i--) fprintf(stderr, "  %s at line %d\n", frames[i].fn, frames[i].line);
        } else if (c[0] == 'p' && c[1] == ' ') {
            const char* want = c + 2;
            bool found = false;
            for (int i = 0; frameCount && i < frames[frameCount - 1].varCount; i++)
                if (!strcmp(frames[frameCount - 1].vars[i].name, want)) {
                    fprintf(stderr, "  %s = %s\n", want, frames[frameCount - 1].vars[i].text);
                    found = true;
                }
            if (!found) fprintf(stderr, "  %s has no value here yet\n", want);
        } else if (c[0] == 'b' && c[1] == ' ') {
            int at = atoi(c + 2);
            if (qkDebugger.breakCount == qkDebugger.breakCap) {
                qkDebugger.breakCap = qkDebugger.breakCap ? qkDebugger.breakCap * 2 : 8;
                qkDebugger.breaks = realloc(qkDebugger.breaks, sizeof(int) * (size_t)qkDebugger.breakCap);
            }
            qkDebugger.breaks[qkDebugger.breakCount++] = at;
            fprintf(stderr, "  a breakpoint at line %d\n", at);
        } else if (c[0] == 'd' && c[1] == ' ') {
            int at = atoi(c + 2);
            for (int i = 0; i < qkDebugger.breakCount; i++)
                if (qkDebugger.breaks[i] == at) qkDebugger.breaks[i--] = qkDebugger.breaks[--qkDebugger.breakCount];
        } else if (!strcmp(c, "q") || !strcmp(c, "quit")) {
            fflush(stdout);
            exit(0);
        } else {
            fputs("  c continue, s step (or Enter), p name, l locals, bt calls, b LINE, d LINE, q quit\n", stderr);
        }
    }
}

int qkDebugDepth(void) { return frameCount; }

const char* qkDebugFrame(int depth, int* line) {
    if (depth < 0 || depth >= frameCount) return NULL;
    if (line) *line = frames[depth].line;
    return frames[depth].fn;
}

int qkDebugVarCount(int depth) { return depth >= 0 && depth < frameCount ? frames[depth].varCount : 0; }

const char* qkDebugVar(int depth, int i, const char** text) {
    if (depth < 0 || depth >= frameCount || i < 0 || i >= frames[depth].varCount) return NULL;
    if (text) *text = frames[depth].vars[i].text;
    return frames[depth].vars[i].name;
}

static void jsonText(FILE* f, const char* s) {
    fputc('"', f);
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') fprintf(f, "\\%c", ch);
        else if (ch == '\n') fputs("\\n", f);
        else if (ch < 32) fprintf(f, "\\u%04x", ch);
        else fputc(ch, f);
    }
    fputc('"', f);
}

/* --trace: {"key": {"value": "...", "hits": n}}: what an editor shows on each node's wire */
void qkDebugFinish(void) {
    if (!qkDebugger.tracePath) return;
    FILE* f = fopen(qkDebugger.tracePath, "wb");
    if (!f) return;
    fputs("{", f);
    for (int i = 0; i < tracedCount; i++) {
        fputs(i ? ",\n " : "\n ", f);
        jsonText(f, traced[i].key);
        fputs(": {\"value\": ", f);
        jsonText(f, traced[i].text);
        fprintf(f, ", \"hits\": %lld}", (long long)traced[i].hits);
    }
    fputs("\n}\n", f);
    fclose(f);
    qkDebugger.tracePath = NULL;
}

/* ---- stopping with a message, leaving -------------------------------------------------------------- */

/* The message, then back to the host's call, or the end of the process (status 1). */
static void rtStop(void) {
    while (frameCount) rtDebugLeave(); /* the debugger's frames: the calls are gone */
    if (qkEscapeRsp && qkEscapeStub) qkEscapeStub(); /* never returns */
    qkDebugFinish();
    fflush(stdout);
    fprintf(stderr, "%s\n", qkPanicText);
    exit(1);
}

/* x? in main, a failed assert: "path:line: message" */
static void rtFail(const char* p, int64_t n, int64_t line) {
    int inl = (int)(line >> 32);
    line &= 0xFFFFFFFF;
    snprintf(qkPanicText, sizeof qkPanicText, "%s:%lld: %.*s", qkRunPath, (long long)line, (int)(n < 400 ? n : 400), p);
    appendTrace(qkPanicText, sizeof qkPanicText, inl);
    rtStop();
}

bool qkExitEnds; /* the command line: exit(code) ends the process; a host's call only stops */

static void rtExit(int64_t code) {
    fflush(stdout);
    if (!qkExitEnds && qkEscapeRsp && qkEscapeStub) { /* a host's call: it ends, the host goes on */
        snprintf(qkPanicText, sizeof qkPanicText, "%s: exit(%lld)", qkRunPath, (long long)code);
        qkEscapeStub();
    }
    qkDebugFinish();
    exit((int)code);
}

/* ---- more text ------------------------------------------------------------------------------------- */

/* words(t): the pieces between runs of spaces */
static int64_t* rtWords(const char* t, int64_t n) { return rtSplit(t, n, "", 0); }

/* lines(t): the lines, without their line breaks (\n or \r\n); no empty last line after a final break */
static int64_t* rtLines(const char* t, int64_t n) {
    if (n && t[n - 1] == '\n') n--;
    if (!n) return rtTextArray(0);
    int64_t* p = rtSplit(t, n, "\n", 1);
    for (int64_t i = 0; i < p[0]; i++) {
        const char* s = (const char*)(intptr_t)p[1 + 2 * i];
        if (p[2 + 2 * i] && s[p[2 + 2 * i] - 1] == '\r') p[2 + 2 * i]--;
    }
    return p;
}

/* chars(t): each character (UTF-8: one to four bytes) */
static int64_t* rtChars(const char* t, int64_t n) {
    int64_t count = 0;
    for (int64_t i = 0; i < n; i++)
        if (((unsigned char)t[i] & 0xC0) != 0x80) count++;
    int64_t* p = rtTextArray(count);
    int64_t k = 0;
    for (int64_t i = 0; i < n;) {
        int64_t j = i + 1;
        while (j < n && ((unsigned char)t[j] & 0xC0) == 0x80) j++;
        p[1 + 2 * k] = (int64_t)(intptr_t)(t + i);
        p[2 + 2 * k] = j - i;
        k++;
        i = j;
    }
    return p;
}

/* ---- characters (UTF-8) ---- */

/* The character at t[*i] (moving *i past it); a byte that starts no character is itself (as Latin-1). */
static uint32_t decodeChar(const unsigned char* t, int64_t n, int64_t* i) {
    uint32_t c = t[*i];
    int more = c >= 0xF0 && c < 0xF8 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (c >= 0xF8 || (c >= 0x80 && c < 0xC0) || *i + more >= n + (more ? 0 : 1)) more = 0;
    uint32_t v = more == 3 ? c & 7 : more == 2 ? c & 15 : more == 1 ? c & 31 : c;
    for (int k = 1; k <= more; k++) {
        if ((t[*i + k] & 0xC0) != 0x80) {
            more = 0;
            v = c;
            break;
        }
        v = v << 6 | (t[*i + k] & 63);
    }
    *i += 1 + more;
    return v;
}

static int encodeChar(uint32_t c, char* out) {
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) { out[0] = (char)(0xC0 | c >> 6); out[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | c >> 12); out[1] = (char)(0x80 | (c >> 6 & 63)); out[2] = (char)(0x80 | (c & 63));
        return 3;
    }
    out[0] = (char)(0xF0 | c >> 18); out[1] = (char)(0x80 | (c >> 12 & 63)); out[2] = (char)(0x80 | (c >> 6 & 63));
    out[3] = (char)(0x80 | (c & 63));
    return 4;
}

/* A letter's other case: Latin (with its accents), Greek, Cyrillic, Armenian, the full-width Latin letters. */
static uint32_t otherCase(uint32_t c, bool upper) {
    if (upper) {
        if (c >= 'a' && c <= 'z') return c - 32;
        if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 32;
        if (c == 0xFF) return 0x178;
        if (((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) && (c & 1)) return c - 1;
        if (((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) && !(c & 1)) return c - 1;
        if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
        if (c == 0x3C2) return 0x3A3;
        if (c == 0x3AC) return 0x386;
        if (c >= 0x3AD && c <= 0x3AF) return c - 37;
        if (c == 0x3CC) return 0x38C;
        if (c == 0x3CD || c == 0x3CE) return c - 63;
        if (c >= 0x430 && c <= 0x44F) return c - 32;
        if (c >= 0x450 && c <= 0x45F) return c - 80;
        if (((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F)) && (c & 1)) return c - 1;
        if (c >= 0x561 && c <= 0x586) return c - 48;
        if (((c >= 0x1E00 && c <= 0x1E95) || (c >= 0x1EA0 && c <= 0x1EFF)) && (c & 1)) return c - 1;
        if (c >= 0xFF41 && c <= 0xFF5A) return c - 32;
        return c;
    }
    if (c >= 'A' && c <= 'Z') return c + 32;
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7)) return c + 32;
    if (c == 0x178) return 0xFF;
    if (((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) && !(c & 1)) return c + 1;
    if (((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) && (c & 1)) return c + 1;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
    if (c == 0x386) return 0x3AC;
    if (c >= 0x388 && c <= 0x38A) return c + 37;
    if (c == 0x38C) return 0x3CC;
    if (c == 0x38E || c == 0x38F) return c + 63;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    if (((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F)) && !(c & 1)) return c + 1;
    if (c >= 0x531 && c <= 0x556) return c + 48;
    if (((c >= 0x1E00 && c <= 0x1E95) || (c >= 0x1EA0 && c <= 0x1EFF)) && !(c & 1)) return c + 1;
    if (c >= 0xFF21 && c <= 0xFF3A) return c + 32;
    return c;
}

static char* rtCase(const char* t, int64_t n, bool upper) {
    bool ascii = true;
    for (int64_t i = 0; i < n && ascii; i++) ascii = (unsigned char)t[i] < 0x80;
    if (ascii) {
        char* p = rtText(t, n);
        for (int64_t i = 0; i < n; i++) {
            char c = p[8 + i];
            if (upper && c >= 'a' && c <= 'z') p[8 + i] = (char)(c - 32);
            if (!upper && c >= 'A' && c <= 'Z') p[8 + i] = (char)(c + 32);
        }
        return p;
    }
    QkBuf b = {0};
    for (int64_t i = 0; i < n;) {
        int64_t at = i;
        uint32_t c = decodeChar((const unsigned char*)t, n, &i);
        if (c >= 0x80 && i - at == 1) { /* a stray byte: as it was */
            qkBufByte(&b, (uint8_t)c);
            continue;
        }
        char out[4];
        qkBufAdd(&b, out, (size_t)encodeChar(otherCase(c, upper), out));
    }
    char* p = rtText(b.data ? b.data : "", (int64_t)b.len);
    qkBufFree(&b);
    return p;
}

/* t[lo..hi] of a text: 0 when both ends are between characters, else 1 + the byte that cuts one */
static int64_t rtTextCut(const char* t, int64_t n, int64_t lo, int64_t hi) {
    if (lo > 0 && lo < n && ((unsigned char)t[lo] & 0xC0) == 0x80) return lo + 1;
    if (hi > 0 && hi < n && ((unsigned char)t[hi] & 0xC0) == 0x80) return hi + 1;
    return 0;
}
static char* rtUpper(const char* t, int64_t n) { return rtCase(t, n, true); }
static char* rtLower(const char* t, int64_t n) { return rtCase(t, n, false); }

static int64_t rtStartsWith(const char* t, int64_t n, const char* s, int64_t sn) { return sn <= n && !memcmp(t, s, (size_t)sn); }
static int64_t rtEndsWith(const char* t, int64_t n, const char* s, int64_t sn) { return sn <= n && !memcmp(t + n - sn, s, (size_t)sn); }

/* where part starts in t (bytes), or -1 */
static int64_t rtFind(const char* t, int64_t n, const char* part, int64_t pn) {
    for (int64_t i = 0; i + pn <= n; i++)
        if (!memcmp(t + i, part, (size_t)pn)) return i;
    return -1;
}

static char* rtReplace(const char* t, int64_t n, const char* old, int64_t on, const char* with, int64_t wn) {
    if (!on) return rtText(t, n);
    QkBuf b = {0};
    int64_t i = 0;
    while (i < n) {
        if (i + on <= n && !memcmp(t + i, old, (size_t)on)) {
            qkBufAdd(&b, with, (size_t)wn);
            i += on;
        } else {
            qkBufByte(&b, (uint8_t)t[i++]);
        }
    }
    char* p = rtText(b.data ? b.data : "", (int64_t)b.len);
    qkBufFree(&b);
    return p;
}

static char* rtRepeat(const char* t, int64_t n, int64_t times) {
    if (times <= 0 || !n) return rtText("", 0);
    char* p = rtAlloc(n * times + 8, 1);
    int64_t total = n * times;
    memcpy(p, &total, 8);
    for (int64_t i = 0; i < times; i++) memcpy(p + 8 + i * n, t, (size_t)n);
    return p;
}

/* join(texts, separator): an array of (bytes, length) pairs */
static char* rtJoin(const int64_t* items, int64_t count, const char* sep, int64_t sn) {
    int64_t total = 0;
    for (int64_t i = 0; i < count; i++) total += items[2 * i + 1] + (i ? sn : 0);
    char* p = rtAlloc(total + 8, 1);
    memcpy(p, &total, 8);
    char* w = p + 8;
    for (int64_t i = 0; i < count; i++) {
        if (i && sn) {
            memcpy(w, sep, (size_t)sn);
            w += sn;
        }
        memcpy(w, (const char*)(intptr_t)items[2 * i], (size_t)items[2 * i + 1]);
        w += items[2 * i + 1];
    }
    return p;
}

/* "{x:.2}": a number with so many decimals */
static char* rtTextFixed(double v, int64_t decimals) {
    char s[400];
    int d = decimals < 0 ? 0 : decimals > 40 ? 40 : (int)decimals;
    int n = snprintf(s, sizeof s, "%.*f", d, v);
    return rtText(s, n < (int)sizeof s ? n : (int)sizeof s - 1);
}

/* "{x:8}": at least so wide, with spaces in front */
static char* rtPad(const char* t, int64_t n, int64_t width) {
    int64_t chars = 0;
    for (int64_t i = 0; i < n; i++)
        if (((unsigned char)t[i] & 0xC0) != 0x80) chars++;
    int64_t pad = width > chars ? width - chars : 0;
    char* p = rtAlloc(n + pad + 8, 1);
    int64_t total = n + pad;
    memcpy(p, &total, 8);
    memset(p + 8, ' ', (size_t)pad);
    memcpy(p + 8 + pad, t, (size_t)n);
    return p;
}

/* ---- the system ------------------------------------------------------------------------------------ */

static int64_t* rtEnv(const char* name, int64_t n) {
    const char* v = getenv(rtCString(name, n));
    if (!v) return rtOptional(0, 0, 0);
    return rtOptional(1, (int64_t)(intptr_t)v, (int64_t)strlen(v));
}

static int64_t rtFileExists(const char* path, int64_t n) {
    FILE* f = fopen(rtCString(path, n), "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int compareNames(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }

/* listDir(path) -> [Text]!: the names in a folder, sorted */
static int64_t* rtListDir(const char* path, int64_t n) {
    const char* dir = rtCString(path, n);
    char** names = NULL;
    int count = 0, cap = 0;
#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = ENOENT;
        return rtCannot("list", dir);
    }
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (count == cap) names = realloc(names, sizeof(char*) * (size_t)(cap = cap ? cap * 2 : 16));
        names[count++] = rtCString(fd.cFileName, (int64_t)strlen(fd.cFileName));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir);
    if (!d) return rtCannot("list", dir);
    struct dirent* ent;
    while ((ent = readdir(d))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        if (count == cap) names = realloc(names, sizeof(char*) * (size_t)(cap = cap ? cap * 2 : 16));
        names[count++] = rtCString(ent->d_name, (int64_t)strlen(ent->d_name));
    }
    closedir(d);
#endif
    if (count) qsort(names, (size_t)count, sizeof(char*), compareNames);
    int64_t* arr = rtTextArray(count);
    for (int i = 0; i < count; i++) {
        arr[1 + 2 * i] = (int64_t)(intptr_t)names[i];
        arr[2 + 2 * i] = (int64_t)strlen(names[i]);
    }
    free(names);
    return rtResult(true, (int64_t)(intptr_t)(arr + 1), count, NULL);
}

/* time(): seconds since 1970 (the wall clock) */
static double rtTime(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* ---- maps: entries in the order their keys came, and a table of where each key is ---------------------
 * An entry: [alive][key: 2 slots][value]. A text key is (bytes, length); any other key is one Int. */

typedef struct QkMap {
    int64_t count;      /* keys in it */
    int64_t used;       /* entries used, removed ones too */
    int64_t cap;        /* room for entries */
    int64_t valueBytes;
    int64_t keyText;    /* 1: a text (bytes, length); 2: two plain slots (a record key: Vec2i, (Int, Int)); 0: one */
    int64_t tableCap;   /* a power of two, or 0 */
    char* entries;
    int32_t* table;     /* entry index + 1, 0: empty */
} QkMap;

static int64_t entryBytes(const QkMap* m) { return 24 + m->valueBytes; }
static int64_t* entryAt(const QkMap* m, int64_t i) { return (int64_t*)(m->entries + i * entryBytes(m)); }

static uint64_t keyHash(const QkMap* m, int64_t k0, int64_t k1) {
    uint64_t h;
    if (m->keyText == 2) { /* two slots: each mixed in */
        h = (uint64_t)k0 * 0x9E3779B97F4A7C15ULL ^ ((uint64_t)k1 + 0x632BE59BD9B4E019ULL);
        h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ULL;
        h = (h ^ (h >> 27)) * 0x94D049BB133111EBULL;
        h ^= h >> 31;
    } else if (m->keyText) {
        h = 1469598103934665603ULL;
        const unsigned char* s = (const unsigned char*)(intptr_t)k0;
        for (int64_t i = 0; i < k1; i++) h = (h ^ s[i]) * 1099511628211ULL;
    } else {
        h = (uint64_t)k0 + 0x9E3779B97F4A7C15ULL;
        h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ULL;
        h = (h ^ (h >> 27)) * 0x94D049BB133111EBULL;
        h ^= h >> 31;
    }
    return h;
}

static bool keyEqual(const QkMap* m, const int64_t* e, int64_t k0, int64_t k1) {
    if (!m->keyText) return e[1] == k0;
    if (m->keyText == 2) return e[1] == k0 && e[2] == k1;
    return e[2] == k1 && !memcmp((const void*)(intptr_t)e[1], (const void*)(intptr_t)k0, (size_t)k1);
}

static int64_t mapFind(const QkMap* m, int64_t k0, int64_t k1) {
    if (!m->tableCap) return -1;
    uint64_t mask = (uint64_t)m->tableCap - 1;
    for (uint64_t i = keyHash(m, k0, k1) & mask;; i = (i + 1) & mask) {
        int32_t at = m->table[i];
        if (!at) return -1;
        int64_t* e = entryAt(m, at - 1);
        if (e[0] && keyEqual(m, e, k0, k1)) return at - 1;
    }
}

static void mapPlace(QkMap* m, int64_t index) {
    int64_t* e = entryAt(m, index);
    uint64_t mask = (uint64_t)m->tableCap - 1;
    uint64_t i = keyHash(m, e[1], e[2]) & mask;
    while (m->table[i]) i = (i + 1) & mask;
    m->table[i] = (int32_t)(index + 1);
}

static void mapRehash(QkMap* m, int64_t tableCap) {
    m->tableCap = tableCap;
    m->table = rtAlloc(tableCap, 4);
    for (int64_t i = 0; i < m->used; i++)
        if (entryAt(m, i)[0]) mapPlace(m, i);
}

static QkMap* rtMapNew(int64_t keyText, int64_t valueBytes) {
    QkMap* m = rtAlloc(1, sizeof(QkMap));
    m->keyText = keyText;
    m->valueBytes = valueBytes;
    return m;
}

/* m[k] = ...: where the value goes (the key added when new) */
static void* rtMapSet(QkMap* m, int64_t k0, int64_t k1) {
    int64_t at = mapFind(m, k0, k1);
    if (at >= 0) return entryAt(m, at) + 3;
    if (m->used == m->cap) {
        int64_t cap = m->cap ? m->cap * 2 : 8;
        char* entries = rtAlloc(cap, entryBytes(m));
        if (m->used) memcpy(entries, m->entries, (size_t)(m->used * entryBytes(m)));
        m->entries = entries;
        m->cap = cap;
    }
    if ((m->used + 1) * 2 > m->tableCap) mapRehash(m, m->tableCap ? m->tableCap * 2 : 16);
    int64_t* e = entryAt(m, m->used);
    memset(e, 0, (size_t)entryBytes(m));
    e[0] = 1;
    e[1] = k0;
    e[2] = m->keyText ? k1 : 0;
    mapPlace(m, m->used);
    m->used++;
    m->count++;
    return e + 3;
}

static void* rtMapGet(QkMap* m, int64_t k0, int64_t k1) {
    int64_t at = mapFind(m, k0, k1);
    return at < 0 ? NULL : entryAt(m, at) + 3;
}

static void rtMapRemove(QkMap* m, int64_t k0, int64_t k1) {
    int64_t at = mapFind(m, k0, k1);
    if (at < 0) return;
    entryAt(m, at)[0] = 0; /* its table slot stays: lookups go past it */
    m->count--;
}

/* A map of its own (a var's): the same keys and values, removed entries left out. */
static QkMap* rtMapCopy(const QkMap* m) {
    QkMap* c = rtMapNew(m->keyText, m->valueBytes);
    if (!m->count) return c;
    c->cap = m->count;
    c->entries = rtAlloc(c->cap, entryBytes(m));
    for (int64_t i = 0; i < m->used; i++) {
        int64_t* e = entryAt(m, i);
        if (e[0]) memcpy(entryAt(c, c->used++), e, (size_t)entryBytes(m));
    }
    c->count = c->used;
    int64_t tableCap = 16;
    while (tableCap < c->used * 2) tableCap *= 2;
    mapRehash(c, tableCap);
    return c;
}

static int64_t rtMapLen(const QkMap* m) { return m->count; }
static int64_t rtMapUsed(const QkMap* m) { return m->used; }
/* entry i's key (its value 16 bytes on), or NULL when it was removed */
static void* rtMapEntry(const QkMap* m, int64_t i) {
    int64_t* e = entryAt(m, i);
    return e[0] ? e + 1 : NULL;
}

/* keys(m), values(m): [count][each one's slots] */
static int64_t* rtMapList(const QkMap* m, bool keys) {
    int64_t each = keys ? (m->keyText ? 16 : 8) : m->valueBytes;
    int64_t* p = rtAlloc(1 + (m->count * each + 7) / 8, 8);
    p[0] = m->count;
    char* w = (char*)(p + 1);
    for (int64_t i = 0; i < m->used; i++) {
        int64_t* e = entryAt(m, i);
        if (!e[0]) continue;
        memcpy(w, keys ? (void*)(e + 1) : (void*)(e + 3), (size_t)each);
        w += each;
    }
    return p;
}
static int64_t* rtMapKeys(const QkMap* m) { return rtMapList(m, true); }
static int64_t* rtMapValues(const QkMap* m) { return rtMapList(m, false); }

/* ---- loops: a mark at the start, and the vars' values copied out when the turns made enough ------------
 * A loop's frame: [0] the mark's block, [1] its used bytes, [2] what had been allocated at the mark,
 * [3] how much may be made before the next collection, [4] the mark's time, [5] -, [6...] the vars' slots. */

enum { FRAME_BLOCK, FRAME_USED, FRAME_ALLOCATED, FRAME_LIMIT, FRAME_TIME, FRAME_ROOTS = 6 };
#define LOOP_FIRST_LIMIT ((int64_t)4 << 20)

static int64_t* rtLoopStart(int64_t rootSlots) {
    QkArena* a = qkArena;
    int64_t* f = rtAlloc(FRAME_ROOTS + rootSlots, 8);
    f[FRAME_BLOCK] = (int64_t)(intptr_t)a->current;
    f[FRAME_USED] = a->current ? (int64_t)a->current->used : 0;
    f[FRAME_ALLOCATED] = a->allocated;
    f[FRAME_LIMIT] = LOOP_FIRST_LIMIT;
    f[FRAME_TIME] = ++a->clock;
    return f;
}

static int64_t rtLoopCheck(const int64_t* f) {
    if (qkCtx->liveTasks) return 0; /* tasks share the arena: a loop's mark may be under another task's values */
    return qkArena->allocated - f[FRAME_ALLOCATED] > f[FRAME_LIMIT];
}

/* The copier: each value reachable from the vars, once (shared ones stay shared), into survivor blocks. */
typedef struct Copier {
    QkArena* a;
    int64_t time;
    Extra* now;
    size_t total;
    struct Moved { uintptr_t from; int64_t len; char kind; uintptr_t to; }* moved;
    size_t movedCap, movedCount;
    struct Pending { int64_t* slots; const char* desc; const char* base; }* pending; /* boxes copied, their insides not yet: a list of
                                                                     * a million boxes would be a million calls deep */
    size_t pendingCount, pendingCap;
} Copier;

static void* survive(Copier* c, size_t bytes) {
    bytes = (bytes + 15) & ~(size_t)15;
    if (!bytes) bytes = 16;
    if (!c->now || c->now->used + bytes > c->now->size) {
        size_t size = bytes > ((size_t)1 << 20) ? bytes : ((size_t)1 << 20);
        Extra* x = calloc(1, sizeof(Extra) + size + 16);
        if (!x) {
            snprintf(qkPanicText, sizeof qkPanicText, "%s: out of memory", qkRunPath);
            rtStop();
        }
        x->size = size;
        x->time = c->time;
        x->data = (char*)(((uintptr_t)(x + 1) + 15) & ~(uintptr_t)15);
        x->next = c->a->extras;
        c->a->extras = x;
        c->now = x;
    }
    void* p = c->now->data + c->now->used;
    c->now->used += bytes;
    c->total += bytes;
    return p;
}

static uintptr_t* movedSlot(Copier* c, uintptr_t from, int64_t len, char kind, bool* found) {
    if (c->movedCount * 2 >= c->movedCap) {
        size_t cap = c->movedCap ? c->movedCap * 2 : 1024;
        struct Moved* grown = calloc(cap, sizeof *grown);
        for (size_t i = 0; i < c->movedCap; i++) {
            struct Moved* m = &c->moved[i];
            if (!m->from) continue;
            size_t k = (size_t)((m->from >> 4) ^ (uint64_t)m->len * 31 ^ (uint64_t)m->kind) & (cap - 1);
            while (grown[k].from) k = (k + 1) & (cap - 1);
            grown[k] = *m;
        }
        free(c->moved);
        c->moved = grown;
        c->movedCap = cap;
    }
    size_t k = (size_t)((from >> 4) ^ (uint64_t)len * 31 ^ (uint64_t)kind) & (c->movedCap - 1);
    while (c->moved[k].from) {
        struct Moved* m = &c->moved[k];
        if (m->from == from && m->len == len && m->kind == kind) {
            *found = true;
            return &m->to;
        }
        k = (k + 1) & (c->movedCap - 1);
    }
    c->moved[k] = (struct Moved){from, len, kind, 0};
    c->movedCount++;
    *found = false;
    return &c->moved[k].to;
}

/* A type's slots, as the compiler wrote them: I a plain slot, T a text, A<elem> an array, M<key><value> a
 * map, (<fields>) a record, E<n><case's fields>.<...> an enum with data (n + 32 its cases). */
static int descSlots(const char** d);
static void skipDesc(const char** d) { /* past one type (a reference is not followed: it may be the type around it) */
    if (**d == 'R') *d += 4;
    else (void)descSlots(d);
}
#define descBase (qkCtx->descBase) /* the whole description: R<at> points into it */

static const char* descRef(const char* d) {
    int at = 0;
    for (int i = 0; i < 3; i++) at = at * 64 + (d[i] - '0');
    return descBase + at;
}

static int descSlots(const char** d) {
    char k = *(*d)++;
    switch (k) {
    case 'I': return 1;
    case 'C': return 1; /* a function's captured values: one address, their description inside */
    case 'B': /* a box: one slot, whatever it holds (a reference is not followed: it may hold this very box) */
        if (**d == 'R') *d += 4;
        else skipDesc(d);
        return 1;
    case 'R': {
        const char* r = descRef(*d);
        *d += 3;
        return descSlots(&r);
    }
    case 'T': return 2;
    case 'A': skipDesc(d); return 2;
    case 'M': skipDesc(d); skipDesc(d); return 1;
    case '(': {
        int n = 0;
        while (**d != ')') n += descSlots(d);
        (*d)++;
        return n;
    }
    case 'E': {
        int cases = *(*d)++ - 32;
        int n = 1;
        for (int i = 0; i < cases; i++) {
            while (**d != '.') n += descSlots(d);
            (*d)++; /* the case's end */
        }
        return n;
    }
    default: return 0;
    }
}

static void copyValue(Copier* c, int64_t* slots, const char** d);

static void copyMapInto(Copier* c, int64_t* slot, const char** d) {
    const char* keyDesc = *d;
    skipDesc(d);
    const char* valueDesc = *d;
    skipDesc(d);
    const QkMap* m = (const QkMap*)(intptr_t)*slot;
    if (!m) return;
    bool found;
    uintptr_t* to = movedSlot(c, (uintptr_t)m, 0, 'M', &found);
    if (found) {
        *slot = (int64_t)*to;
        return;
    }
    QkMap* n = survive(c, sizeof(QkMap));
    *to = (uintptr_t)n;
    *slot = (int64_t)(uintptr_t)n;
    *n = *m;
    n->cap = m->count ? m->count : 1;
    n->used = 0;
    n->entries = survive(c, (size_t)(n->cap * entryBytes(m)));
    for (int64_t i = 0; i < m->used; i++) {
        int64_t* e = entryAt(m, i);
        if (!e[0]) continue;
        int64_t* ne = entryAt(n, n->used++);
        memcpy(ne, e, (size_t)entryBytes(m));
        const char* kd = keyDesc;
        copyValue(c, ne + 1, &kd);
        const char* vd = valueDesc;
        copyValue(c, ne + 3, &vd);
    }
    n->count = n->used;
    n->tableCap = 16;
    while (n->tableCap < n->used * 2) n->tableCap *= 2;
    n->table = survive(c, (size_t)n->tableCap * 4);
    memset(n->table, 0, (size_t)n->tableCap * 4);
    for (int64_t i = 0; i < n->used; i++) mapPlace(n, i);
}

/* Copies what the slots point to (as desc says), and points them at the copies; d moves past the type. */
static void copyValue(Copier* c, int64_t* slots, const char** d) {
    char k = *(*d)++;
    switch (k) {
    case 'I': return;
    case 'T': {
        int64_t len = slots[1];
        if (len <= 0 || !slots[0]) return;
        bool found;
        uintptr_t* to = movedSlot(c, (uintptr_t)slots[0], len, 'T', &found);
        if (!found) {
            char* p = survive(c, (size_t)len + 8);
            memcpy(p, &len, 8);
            memcpy(p + 8, (const void*)(intptr_t)slots[0], (size_t)len);
            *to = (uintptr_t)(p + 8);
        }
        slots[0] = (int64_t)*to;
        return;
    }
    case 'A': {
        const char* elem = *d;
        int each = descSlots(d) * 8;
        int64_t len = slots[1];
        if (len <= 0 || !slots[0]) return;
        bool found;
        uintptr_t* to = movedSlot(c, (uintptr_t)slots[0], len, 'A', &found);
        if (found) {
            slots[0] = (int64_t)*to;
            return;
        }
        char* p = survive(c, (size_t)(len * each));
        memcpy(p, (const void*)(intptr_t)slots[0], (size_t)(len * each));
        *to = (uintptr_t)p; /* before the elements: the table may grow while they are copied */
        slots[0] = (int64_t)(uintptr_t)p;
        bool plain = true;
        for (const char* q = elem; q < *d; q++) plain &= *q == 'I';
        for (int64_t i = 0; !plain && i < len; i++) {
            const char* e = elem;
            copyValue(c, (int64_t*)(p + i * each), &e);
        }
        return;
    }
    case 'M': copyMapInto(c, slots, d); return;
    case 'R': { /* the type a box of a type that holds itself holds */
        const char* r = descRef(*d);
        *d += 3;
        copyValue(c, slots, &r);
        return;
    }
    case 'B': { /* a box: the value it points to, copied, and copied into */
        const char* inner = *d;
        *d -= 1;
        skipDesc(d);
        if (!slots[0]) return;
        const char* sizer = inner;
        int n = descSlots(&sizer);
        bool found;
        uintptr_t* to = movedSlot(c, (uintptr_t)slots[0], n, 'B', &found);
        if (found) {
            slots[0] = (int64_t)*to;
            return;
        }
        int64_t* p = survive(c, (size_t)n * 8);
        memcpy(p, (const void*)(intptr_t)slots[0], (size_t)n * 8);
        *to = (uintptr_t)p;
        slots[0] = (int64_t)(intptr_t)p;
        if (c->pendingCount == c->pendingCap) {
            c->pendingCap = c->pendingCap ? c->pendingCap * 2 : 64;
            c->pending = realloc(c->pending, c->pendingCap * sizeof *c->pending);
        }
        c->pending[c->pendingCount++] = (struct Pending){p, inner, descBase};
        return;
    }
    case 'C': { /* a lambda's captured values: [their description][their slots], copied, then copied into */
        if (!slots[0]) return;
        const char* desc = (const char*)(intptr_t)((int64_t*)(intptr_t)slots[0])[0];
        int n = 1;
        for (const char* q = desc; *q;) n += descSlots(&q);
        bool found;
        uintptr_t* to = movedSlot(c, (uintptr_t)slots[0], n, 'C', &found);
        if (found) {
            slots[0] = (int64_t)*to;
            return;
        }
        int64_t* p = survive(c, (size_t)n * 8);
        memcpy(p, (const void*)(intptr_t)slots[0], (size_t)n * 8);
        *to = (uintptr_t)p;
        slots[0] = (int64_t)(intptr_t)p;
        int64_t* at = p + 1;
        const char* saved = descBase;
        descBase = desc;
        for (const char* q = desc; *q;) {
            const char* f = q;
            int k = descSlots(&f);
            copyValue(c, at, &q);
            at += k;
        }
        descBase = saved;
        return;
    }
    case '(': { /* a record: its fields one after another */
        int64_t* at = slots;
        while (**d != ')') {
            const char* f = *d;
            int n = descSlots(&f);
            copyValue(c, at, d);
            at += n;
        }
        (*d)++;
        return;
    }
    case 'E': {
        int cases = *(*d)++ - 32;
        int64_t tag = slots[0];
        int64_t* at = slots + 1;
        for (int i = 0; i < cases; i++) {
            while (**d != '.') {
                const char* f = *d;
                int n = descSlots(&f);
                if (i == tag) copyValue(c, at, d);
                else *d = f;
                at += n;
            }
            (*d)++; /* the case's end */
        }
        return;
    }
    }
}

/* The loop's turns made enough: its vars' values to a survivor, and the arena back to the mark. */
static void rtLoopCollect(int64_t* f, const char* desc) {
    QkArena* a = qkArena;
    Copier c = {a, ++a->clock, NULL, 0, NULL, 0, 0};
    const char* d = desc;
    descBase = desc;
    int64_t* slots = f + FRAME_ROOTS;
    while (*d) {
        const char* start = d;
        int n = descSlots(&start);
        copyValue(&c, slots, &d);
        slots += n;
    }
    while (c.pendingCount) { /* the boxes' insides, one after another */
        struct Pending w = c.pending[--c.pendingCount];
        descBase = w.base;
        copyValue(&c, w.slots, &w.desc);
    }
    free(c.pending);
    free(c.moved);
    freeExtras(a, f[FRAME_TIME], c.time); /* inner loops' survivors and this loop's last ones: copied already */
    resetTo(a, (ArenaBlock*)(intptr_t)f[FRAME_BLOCK], (size_t)f[FRAME_USED]);
    f[FRAME_ALLOCATED] = a->allocated;
    int64_t limit = 2 * (int64_t)c.total;
    f[FRAME_LIMIT] = limit > LOOP_FIRST_LIMIT ? limit : LOOP_FIRST_LIMIT;
}
static int64_t rtSpawn(void* fn, int64_t env);
static void rtCaptureBegin(void);
static char* rtCaptureEnd(void);
static void rtDebugEnter(const char* fn, int64_t n);
static void rtDebugLeave(void);
static void rtDebugLine(int64_t line);
static void rtDebugValue(const char* name, int64_t nn, const char* key, int64_t kn, const char* text, int64_t tn);
static int64_t rtStep(int64_t n, int64_t line);
static void rtYield(void);
static void rtWait(double seconds);

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
    [RT_READ_FILE] = {"readFile", (void*)rtReadFile, "II", 'I'},
    [RT_WRITE_FILE] = {"writeFile", (void*)rtWriteFile, "IIII", 'I'},
    [RT_READ_LINE] = {"readLine", (void*)rtReadLine, "", 'I'},
    [RT_ARGS] = {"args", (void*)rtArgs, "", 'I'},
    [RT_SPLIT] = {"split", (void*)rtSplit, "IIII", 'I'},
    [RT_TRIM] = {"trim", (void*)rtTrim, "II", 'I'},
    [RT_PARSE_INT] = {"parseInt", (void*)rtParseInt, "II", 'I'},
    [RT_PARSE_FLOAT] = {"parseFloat", (void*)rtParseFloat, "II", 'I'},
    [RT_FAIL] = {"fail", (void*)rtFail, "III", 'V'},
    [RT_EXIT] = {"exit", (void*)rtExit, "I", 'V'},
    [RT_PRINT_TO] = {"printTo", (void*)rtPrintTo, "I", 'V'},
    [RT_WORDS] = {"words", (void*)rtWords, "II", 'I'},
    [RT_LINES] = {"lines", (void*)rtLines, "II", 'I'},
    [RT_CHARS] = {"chars", (void*)rtChars, "II", 'I'},
    [RT_UPPER] = {"upper", (void*)rtUpper, "II", 'I'},
    [RT_LOWER] = {"lower", (void*)rtLower, "II", 'I'},
    [RT_STARTS_WITH] = {"startsWith", (void*)rtStartsWith, "IIII", 'I'},
    [RT_ENDS_WITH] = {"endsWith", (void*)rtEndsWith, "IIII", 'I'},
    [RT_REPLACE] = {"replace", (void*)rtReplace, "IIIIII", 'I'},
    [RT_REPEAT] = {"repeat", (void*)rtRepeat, "III", 'I'},
    [RT_JOIN] = {"join", (void*)rtJoin, "IIII", 'I'},
    [RT_FIND] = {"find", (void*)rtFind, "IIII", 'I'},
    [RT_TEXT_FIXED] = {"textFixed", (void*)rtTextFixed, "FI", 'I'},
    [RT_PAD] = {"pad", (void*)rtPad, "III", 'I'},
    [RT_ENV] = {"env", (void*)rtEnv, "II", 'I'},
    [RT_LIST_DIR] = {"listDir", (void*)rtListDir, "II", 'I'},
    [RT_FILE_EXISTS] = {"fileExists", (void*)rtFileExists, "II", 'I'},
    [RT_TIME] = {"time", (void*)rtTime, "", 'F'},
    [RT_MAP_NEW] = {"mapNew", (void*)rtMapNew, "II", 'I'},
    [RT_MAP_SET] = {"mapSet", (void*)rtMapSet, "III", 'I'},
    [RT_MAP_GET] = {"mapGet", (void*)rtMapGet, "III", 'I'},
    [RT_MAP_REMOVE] = {"mapRemove", (void*)rtMapRemove, "III", 'V'},
    [RT_MAP_COPY] = {"mapCopy", (void*)rtMapCopy, "I", 'I'},
    [RT_MAP_LEN] = {"mapLen", (void*)rtMapLen, "I", 'I'},
    [RT_MAP_USED] = {"mapUsed", (void*)rtMapUsed, "I", 'I'},
    [RT_MAP_ENTRY] = {"mapEntry", (void*)rtMapEntry, "II", 'I'},
    [RT_MAP_KEYS] = {"mapKeys", (void*)rtMapKeys, "I", 'I'},
    [RT_MAP_VALUES] = {"mapValues", (void*)rtMapValues, "I", 'I'},
    [RT_LOOP_START] = {"loopStart", (void*)rtLoopStart, "I", 'I'},
    [RT_LOOP_CHECK] = {"loopCheck", (void*)rtLoopCheck, "I", 'I'},
    [RT_LOOP_COLLECT] = {"loopCollect", (void*)rtLoopCollect, "II", 'V'},
    [RT_TEXT_CUT] = {"textCut", (void*)rtTextCut, "IIII", 'I'},
    [RT_ARRAY_JOIN] = {"arrayJoin", (void*)rtArrayJoin, "IIIII", 'I'},
    [RT_PARALLEL_MAP] = {"parallelMap", (void*)rtParallelMap, "IIIIIII", 'I'},
    [RT_RANDOM] = {"random", (void*)rtRandom, "", 'F'},
    [RT_RANDOM_INT] = {"randomInt", (void*)rtRandomInt, "III", 'I'},
    [RT_SEED_RANDOM] = {"seedRandom", (void*)rtSeedRandom, "I", 'V'},
    [RT_SPAWN] = {"spawn", (void*)rtSpawn, "II", 'I'},
    [RT_STEP] = {"step", (void*)rtStep, "II", 'I'},
    [RT_YIELD] = {"yield", (void*)rtYield, "", 'V'},
    [RT_WAIT] = {"wait", (void*)rtWait, "F", 'V'},
    [RT_CAPTURE_BEGIN] = {"captureBegin", (void*)rtCaptureBegin, "", 'V'},
    [RT_CAPTURE_END] = {"captureEnd", (void*)rtCaptureEnd, "", 'I'},
    [RT_DEBUG_ENTER] = {"debugEnter", (void*)rtDebugEnter, "II", 'V'},
    [RT_DEBUG_LEAVE] = {"debugLeave", (void*)rtDebugLeave, "", 'V'},
    [RT_DEBUG_LINE] = {"debugLine", (void*)rtDebugLine, "I", 'V'},
    [RT_DEBUG_VALUE] = {"debugValue", (void*)rtDebugValue, "IIIIII", 'V'},
};

/* ---- loading machine code ------------------------------------------------------------------------ */

#ifdef _WIN32
/* Testing the System V code on Windows (quark run --abi sysv): the runtime through adapters. */
static uint8_t* sysvAdapters;
static int32_t sysvAt[RT_COUNT];

static void* runtimeAddress(int id) {
    if (qkAbi != QK_ABI_SYSV) return qkRuntime[id].addr;
    if (!sysvAdapters) {
        QkBuf b = {0};
        qkGenSysvAdapters(&b, sysvAt);
        sysvAdapters = VirtualAlloc(NULL, b.len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        memcpy(sysvAdapters, b.data, b.len);
        DWORD old;
        VirtualProtect(sysvAdapters, b.len, PAGE_EXECUTE_READ, &old);
        qkBufFree(&b);
    }
    return sysvAdapters + sysvAt[id];
}
#else
static void* runtimeAddress(int id) { return qkRuntime[id].addr; }
#endif

void* qkRelocTarget(int kind, int64_t target, uint8_t* data, void* const* hosts) {
    switch (kind) {
    case RELOC_RUNTIME: return runtimeAddress((int)target);
    case RELOC_DATA: return data + target;
    case RELOC_FUEL: return &qkFuel;
    case RELOC_HOST: return hosts ? hosts[target] : NULL;
    case RELOC_ESCAPE: return &qkEscapeRsp;
    case RELOC_PANIC_FRAME: return &qkPanicFrame;
    case RELOC_STACK_LIMIT: return &qkStackLimit;
    }
    return NULL;
}

bool qkLoad(const uint8_t* code, size_t codeLen, const uint8_t* data, size_t dataLen, const Reloc* relocs, int relocCount,
            int32_t enter, int32_t escape, void* const* hosts, Loaded* out) {
    memset(out, 0, sizeof *out);
    uint8_t* dataCopy = malloc(dataLen + 16);
    if (!dataCopy) return false;
    memcpy(dataCopy, data, dataLen);
#ifdef _WIN32
    uint8_t* mem = VirtualAlloc(NULL, codeLen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#elif defined(__APPLE__) && defined(__aarch64__)
    /* Apple Silicon: memory that is written, then run, is MAP_JIT, writable only while this thread says so */
    uint8_t* mem = mmap(NULL, codeLen, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
    if (mem == MAP_FAILED) mem = NULL;
    else pthread_jit_write_protect_np(0);
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
        case RELOC_RUNTIME: address = (uint64_t)(uintptr_t)runtimeAddress((int)relocs[i].target); break;
        case RELOC_DATA: address = (uint64_t)(uintptr_t)(dataCopy + relocs[i].target); break;
        case RELOC_FUEL: address = (uint64_t)(uintptr_t)&qkFuel; break;
        case RELOC_HOST: address = (uint64_t)(uintptr_t)(hosts ? hosts[relocs[i].target] : NULL); break;
        case RELOC_ESCAPE: address = (uint64_t)(uintptr_t)&qkEscapeRsp; break;
        case RELOC_PANIC_FRAME: address = (uint64_t)(uintptr_t)&qkPanicFrame; break;
        case RELOC_STACK_LIMIT: address = (uint64_t)(uintptr_t)&qkStackLimit; break;
        }
        memcpy(mem + relocs[i].at, &address, 8);
    }
#ifdef _WIN32
    DWORD old;
    VirtualProtect(mem, codeLen, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), mem, codeLen);
#elif defined(__APPLE__) && defined(__aarch64__)
    pthread_jit_write_protect_np(1);
    __builtin___clear_cache((char*)mem, (char*)mem + codeLen);
#else
    mprotect(mem, codeLen, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char*)mem, (char*)mem + codeLen); /* ARM: the instruction cache sees the new code */
#endif
    out->code = mem;
    out->codeLen = codeLen;
    out->data = dataCopy;
    out->source = malloc(codeLen + dataLen + 16); /* what another thread's copy is loaded from */
    if (out->source) {
        memcpy(out->source, code, codeLen);
        memcpy(out->source + codeLen, data, dataLen);
    }
    out->relocs = malloc(sizeof(Reloc) * (size_t)(relocCount ? relocCount : 1));
    if (out->relocs) memcpy(out->relocs, relocs, sizeof(Reloc) * (size_t)relocCount);
    out->relocCount = relocCount;
    out->dataLen = dataLen;
    out->enterAt = enter;
    out->escapeAt = escape;
    for (int i = 0; i < relocCount; i++)
        if (relocs[i].kind == RELOC_HOST && relocs[i].target + 1 > out->hostCount) out->hostCount = (int)relocs[i].target + 1;
    out->hosts = calloc((size_t)(out->hostCount ? out->hostCount : 1), sizeof(void*));
    for (int i = 0; hosts && out->hosts && i < out->hostCount; i++) out->hosts[i] = hosts[i];
    void* at = mem + enter;
    memcpy(&out->enter, &at, sizeof at);
    at = mem + escape;
    memcpy(&out->escape, &at, sizeof at);
    return true;
}

void qkUnload(Loaded* l) {
    if (l->linked) { /* the host's own: only what was made for it */
        if (!l->copy) freeTrace(l->trace);
        memset(l, 0, sizeof *l);
        return;
    }
    if (l->code) {
#ifdef _WIN32
        VirtualFree(l->code, 0, MEM_RELEASE);
#else
        munmap(l->code, l->codeLen);
#endif
    }
    free(l->data);
    if (!l->copy) freeTrace(l->trace);
    free(l->source);
    free(l->relocs);
    free(l->hosts);
    memset(l, 0, sizeof *l);
}


/* The lowest address this thread's stack may reach, keeping room for the runtime (a panic prints, C calls). */
static uintptr_t stackLimit(void) {
    const uintptr_t room = 256 * 1024;
    uintptr_t here = (uintptr_t)&here;
    if (qkCtx->taskStackLow) return qkCtx->taskStackLow + 64 * 1024; /* a task: its own stack */
#if defined(_WIN32)
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    if (low && here > low + room) return (uintptr_t)low + room;
#elif defined(__APPLE__)
    pthread_t self = pthread_self();
    uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(self);
    size_t size = pthread_get_stacksize_np(self);
    if (top && size > room && here > top - size + room) return top - size + room;
#elif defined(__linux__)
    pthread_attr_t attr;
    void* low = NULL;
    size_t size = 0;
    if (!pthread_getattr_np(pthread_self(), &attr)) {
        pthread_attr_getstack(&attr, &low, &size);
        pthread_attr_destroy(&attr);
        if (low && here > (uintptr_t)low + room) return (uintptr_t)low + room;
    }
#endif
    return here > 1024 * 1024 ? here - 1024 * 1024 + room : 0; /* unknown: a megabyte from here */
}

/* ---- tasks: a function on a stack of its own, run a step at a time ------------------------------------
 * qkSwitch saves the registers calls keep and the stack pointer, and takes up another stack where it was left:
 * the code Quark makes needs nothing of its own for it. A task's first switch lands in taskStart, which calls
 * taskMain with the task (in a register qkSwitch restores). */
#define TASK_STACK (8 << 20)

struct QkTask {
    void* sp;                 /* where the task's stack was left */
    void* callerSp;           /* where the caller's was */
    char* stack;
    const Loaded* l;
    void* fn;
    uint64_t args[16];
    int argCount;
    int state;                /* 0: not started, 1: gave its turn back, 2: done, 3: stopped */
    char error[512];
    QkTask* caller;           /* the task that stepped it, or NULL */
    void* escapeRsp;          /* its panic path and stack, while another runs */
    void (*escapeStub)(void);
    uintptr_t stackLimit;
    const Loaded* code;       /* what it runs, and where it was called */
    int32_t codeEntry;
    void* outerEscape;        /* the caller's, while it runs */
    void (*outerStub)(void);
    uintptr_t outerLimit, outerLow;
    const Loaded* outerRunning;
    int32_t outerEntry;
};

void qkSwitch(void** save, void* to);
void qkTaskBegin(void);

#if defined(__x86_64__) && defined(_WIN32)
__asm__(".text\n.globl qkSwitch\nqkSwitch:\n"
        "pushq %rbp\npushq %rbx\npushq %rdi\npushq %rsi\npushq %r12\npushq %r13\npushq %r14\npushq %r15\n"
        "subq $168, %rsp\n"
        "movdqu %xmm6, 0(%rsp)\nmovdqu %xmm7, 16(%rsp)\nmovdqu %xmm8, 32(%rsp)\nmovdqu %xmm9, 48(%rsp)\n"
        "movdqu %xmm10, 64(%rsp)\nmovdqu %xmm11, 80(%rsp)\nmovdqu %xmm12, 96(%rsp)\nmovdqu %xmm13, 112(%rsp)\n"
        "movdqu %xmm14, 128(%rsp)\nmovdqu %xmm15, 144(%rsp)\n"
        "movq %rsp, (%rcx)\nmovq %rdx, %rsp\n"
        "movdqu 0(%rsp), %xmm6\nmovdqu 16(%rsp), %xmm7\nmovdqu 32(%rsp), %xmm8\nmovdqu 48(%rsp), %xmm9\n"
        "movdqu 64(%rsp), %xmm10\nmovdqu 80(%rsp), %xmm11\nmovdqu 96(%rsp), %xmm12\nmovdqu 112(%rsp), %xmm13\n"
        "movdqu 128(%rsp), %xmm14\nmovdqu 144(%rsp), %xmm15\n"
        "addq $168, %rsp\n"
        "popq %r15\npopq %r14\npopq %r13\npopq %r12\npopq %rsi\npopq %rdi\npopq %rbx\npopq %rbp\nret\n"
        ".globl qkTaskBegin\nqkTaskBegin:\nmovq %rbx, %rcx\nsubq $40, %rsp\ncall taskMain\nud2\n");
#elif defined(__x86_64__)
__asm__(".text\n.globl " QK_SYM("qkSwitch") "\n" QK_SYM("qkSwitch") ":\n"
        "pushq %rbp\npushq %rbx\npushq %r12\npushq %r13\npushq %r14\npushq %r15\n"
        "movq %rsp, (%rdi)\nmovq %rsi, %rsp\n"
        "popq %r15\npopq %r14\npopq %r13\npopq %r12\npopq %rbx\npopq %rbp\nret\n"
        ".globl " QK_SYM("qkTaskBegin") "\n" QK_SYM("qkTaskBegin") ":\nmovq %rbx, %rdi\ncall " QK_SYM("taskMain") "\nud2\n");
#elif defined(__aarch64__)
__asm__(".text\n.globl " QK_SYM("qkSwitch") "\n.p2align 2\n" QK_SYM("qkSwitch") ":\n"
        "sub sp, sp, #176\n"
        "stp x19, x20, [sp, #0]\nstp x21, x22, [sp, #16]\nstp x23, x24, [sp, #32]\nstp x25, x26, [sp, #48]\n"
        "stp x27, x28, [sp, #64]\nstp x29, x30, [sp, #80]\nstp d8, d9, [sp, #96]\nstp d10, d11, [sp, #112]\n"
        "stp d12, d13, [sp, #128]\nstp d14, d15, [sp, #144]\n"
        "mov x2, sp\nstr x2, [x0]\nmov sp, x1\n"
        "ldp x19, x20, [sp, #0]\nldp x21, x22, [sp, #16]\nldp x23, x24, [sp, #32]\nldp x25, x26, [sp, #48]\n"
        "ldp x27, x28, [sp, #64]\nldp x29, x30, [sp, #80]\nldp d8, d9, [sp, #96]\nldp d10, d11, [sp, #112]\n"
        "ldp d12, d13, [sp, #128]\nldp d14, d15, [sp, #144]\n"
        "add sp, sp, #176\nret\n"
        ".globl " QK_SYM("qkTaskBegin") "\n.p2align 2\n" QK_SYM("qkTaskBegin") ":\nmov x0, x19\nbl " QK_SYM("taskMain") "\nbrk #0\n");
#endif

/* the context's per-call fields: the task's in, the caller's out (or back) */
static void enterTask(QkTask* t) {
    t->outerEscape = qkEscapeRsp;
    t->outerStub = qkEscapeStub;
    t->outerLimit = qkStackLimit;
    t->outerLow = qkCtx->taskStackLow;
    t->outerRunning = running;
    t->outerEntry = runningEntry;
    t->caller = qkCtx->task;
    qkCtx->task = t;
    qkEscapeRsp = t->escapeRsp;
    qkEscapeStub = t->escapeStub;
    qkStackLimit = t->stackLimit;
    qkCtx->taskStackLow = (uintptr_t)t->stack;
    running = t->code;
    runningEntry = t->codeEntry;
}

static void leaveTask(QkTask* t) {
    t->escapeRsp = qkEscapeRsp;
    t->escapeStub = qkEscapeStub;
    t->stackLimit = qkStackLimit;
    t->code = running;
    t->codeEntry = runningEntry;
    qkEscapeRsp = t->outerEscape;
    qkEscapeStub = t->outerStub;
    qkStackLimit = t->outerLimit;
    qkCtx->taskStackLow = t->outerLow;
    running = t->outerRunning;
    runningEntry = t->outerEntry;
    qkCtx->task = t->caller;
}

void taskMain(QkTask* t);
void taskMain(QkTask* t) {
    qkEscapeRsp = (void*)1; /* not the outermost call: the arena is the caller's, it stays */
    bool ok = qkRunEntry(t->l, t->fn, t->args, t->argCount, NULL, 0);
    if (!ok) snprintf(t->error, sizeof t->error, "%s", qkPanicText);
    t->state = ok ? 2 : 3;
    qkCtx->liveTasks--;
    leaveTask(t);
    void* dead;
    qkSwitch(&dead, t->callerSp); /* never back */
}

QkTask* qkTaskNew(const Loaded* l, void* fn, const uint64_t* args, int argCount) {
    QkTask* t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->stack = malloc(TASK_STACK);
    if (!t->stack || argCount > 16) {
        free(t->stack);
        free(t);
        return NULL;
    }
    t->l = l;
    t->fn = fn;
    t->argCount = argCount;
    if (argCount) memcpy(t->args, args, sizeof(uint64_t) * (size_t)argCount);
    t->code = l;
    t->stackLimit = (uintptr_t)t->stack + 64 * 1024;
    uintptr_t top = ((uintptr_t)t->stack + TASK_STACK) & ~(uintptr_t)15;
    uint64_t* sp;
#if defined(__x86_64__) && defined(_WIN32)
    uint64_t* ret = (uint64_t*)(top - 16); /* after the ret: 8 mod 16, then sub 40: a call's alignment */
    ret[0] = (uint64_t)(uintptr_t)qkTaskBegin;
    sp = ret - 8;                          /* r15 r14 r13 r12 rsi rdi rbx rbp */
    memset(sp, 0, 8 * 8);
    sp[6] = (uint64_t)(uintptr_t)t;        /* rbx */
    sp = (uint64_t*)((char*)sp - 168);
    memset(sp, 0, 168);
#elif defined(__x86_64__)
    uint64_t* ret = (uint64_t*)(top - 8);  /* after the ret: 0 mod 16, as a call wants */
    ret[0] = (uint64_t)(uintptr_t)qkTaskBegin;
    sp = ret - 6;                          /* r15 r14 r13 r12 rbx rbp */
    memset(sp, 0, 6 * 8);
    sp[4] = (uint64_t)(uintptr_t)t;        /* rbx */
#else
    sp = (uint64_t*)(top - 176);           /* x19-x28, x29, x30, d8-d15 */
    memset(sp, 0, 176);
    sp[0] = (uint64_t)(uintptr_t)t;        /* x19 */
    sp[11] = (uint64_t)(uintptr_t)qkTaskBegin; /* x30: where its ret goes */
#endif
    t->sp = sp;
    qkCtx->liveTasks++;
    return t;
}

int qkTaskStep(QkTask* t) {
    if (!t || t->state >= 2) return t && t->state == 2 ? 0 : -1;
    enterTask(t);
    qkSwitch(&t->callerSp, t->sp); /* back when it gives its turn back, or ends (it left the context as it was) */
    return t->state == 1 ? 1 : t->state == 2 ? 0 : -1;
}

const char* qkTaskError(const QkTask* t) { return t ? t->error : ""; }

void qkTaskFree(QkTask* t) {
    if (!t) return;
    if (t->state < 2) qkCtx->liveTasks--;
    free(t->stack);
    free(t);
}

/* yield(): the turn back to whoever stepped this task (nothing, outside one) */
static void rtYield(void) {
    QkTask* t = qkCtx->task;
    if (!t) return;
    t->state = 1;
    leaveTask(t);
    qkSwitch(&t->sp, t->callerSp);
}

static double rtNow(void);

/* wait(seconds): in a task, the turn back until the time went by; outside one, a sleep */
static void rtWait(double seconds) {
    double until = rtNow() + seconds;
    if (!qkCtx->task) {
        while (rtNow() < until) {
#ifdef _WIN32
            Sleep(1);
#else
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, NULL);
#endif
        }
        return;
    }
    do rtYield();
    while (rtNow() < until);
}

/* the tasks spawn made, by number */
static QkTask** tasks;
static int64_t taskCount, taskCap;

static int64_t rtSpawn(void* fn, int64_t env) {
    uint64_t args[1] = {(uint64_t)env};
    QkTask* t = qkTaskNew(running, fn, args, 1);
    if (!t) rtPanic(PANIC_SIZE, 0, TASK_STACK, 0);
    if (taskCount == taskCap) {
        taskCap = taskCap ? taskCap * 2 : 16;
        tasks = realloc(tasks, sizeof(QkTask*) * (size_t)taskCap);
    }
    tasks[taskCount] = t;
    return taskCount++;
}

static int64_t rtStep(int64_t n, int64_t line) {
    if (n < 0 || n >= taskCount || !tasks[n]) rtPanic(PANIC_TASK, line, n, 0);
    int r = qkTaskStep(tasks[n]);
    if (r < 0) { /* the task stopped: so does the one that stepped it, with its message */
        snprintf(qkPanicText, sizeof qkPanicText, "%s", tasks[n]->error);
        rtStop();
    }
    if (r == 0) {
        qkTaskFree(tasks[n]);
        tasks[n] = NULL;
    }
    return r == 1;
}

bool qkRunEntry(const Loaded* l, void* fn, const uint64_t* args, int argCount, uint64_t* results, int resultCount) {
    if (!qkEscapeRsp) qkArenaReset(); /* the outermost call: what the last one allocated goes */
    qkStackLimit = stackLimit();
    void (*savedStub)(void) = qkEscapeStub;
    qkEscapeStub = l->escape;
    qkPanicText[0] = '\0';
    QkEnter e = {fn, args, argCount, results, resultCount};
    const Loaded* wasRunning = running;
    int32_t wasEntry = runningEntry;
    running = l;
    runningEntry = (int32_t)((uint8_t*)fn - l->code);
    qkPanicFrame = NULL;
#if defined(_WIN32) && defined(__GNUC__)
    int64_t stopped;
    if (qkAbi == QK_ABI_SYSV) { /* the trampoline takes its argument the System V way */
        int64_t (__attribute__((sysv_abi)) * enter)(QkEnter*);
        memcpy(&enter, &l->enter, sizeof enter);
        stopped = enter(&e);
    } else {
        stopped = l->enter(&e);
    }
#else
    int64_t stopped = l->enter(&e);
#endif
    qkEscapeStub = savedStub;
    running = wasRunning;
    runningEntry = wasEntry;
    return stopped == 0;
}
