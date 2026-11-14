/* Quark — the embedding API (include/quark.h): what a host application or an editor uses. It wraps the compiler
 * (parse, check, lower, optimize, x64), the loader and the trampoline, collects errors instead of printing
 * them, and frees the compiler's memory after every compile: a module keeps only its machine code and a
 * small table of its functions. */
#include "qk.h"
#include "../include/quark.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct HostFn {
    char* name;
    void* fn;
} HostFn;

struct QkVm {
    char* prelude;
    char* capabilities; /* what the programs it compiles may call (NULL: everything) */
    HostFn* hosts;
    int hostCount, hostCap;
    int64_t fuel;
    bool noAvx;
    bool checked;
    QkDiagnostic* diags;
    int diagCount, diagCap;
    char lastError[512];
    QkArena* arena; /* the machine's own memory: its calls never touch another machine's values */
};

struct QkTaskHandle {
    QkTask* task;
    QkModule* module;
};

typedef struct ModFn {
    char* name;
    char* signature;
    char* kinds; /* parameters' slots, then results' */
    int params, results;
    int32_t at;
} ModFn;

struct QkModule {
    QkVm* vm;
    char* name;
    Loaded loaded;
    ModFn* fns;
    int count;
};

static char* dupText(const char* s) {
    size_t n = strlen(s);
    char* d = malloc(n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

/* ---- errors -------------------------------------------------------------------------------------- */

static QkVm* collecting;

static void collect(const char* path, int line, int col, const char* message) {
    QkVm* vm = collecting;
    if (!vm) return;
    if (vm->diagCount == vm->diagCap) {
        int cap = vm->diagCap ? vm->diagCap * 2 : 8;
        QkDiagnostic* d = realloc(vm->diags, sizeof(QkDiagnostic) * (size_t)cap);
        if (!d) return;
        vm->diags = d;
        vm->diagCap = cap;
    }
    QkDiagnostic* d = &vm->diags[vm->diagCount++];
    memset(d, 0, sizeof *d);
    d->line = line;
    d->col = col;
    snprintf(d->message, sizeof d->message, "%s:%d:%d: %s", path, line, col, message);
}

static void begin(QkVm* vm) {
    vm->diagCount = 0;
    collecting = vm;
    qkErrorSink = collect;
    qkNewReset();
    qkTypesReset();
}

static void end(void) {
    collecting = NULL;
    qkErrorSink = NULL;
    qkNewReset();
    qkTypesReset();
}

static void fail(QkVm* vm, const char* fmt, const char* a) {
    collecting = vm;
    char message[300];
    snprintf(message, sizeof message, fmt, a);
    collect("quark", 0, 0, message);
}

/* ---- the machine --------------------------------------------------------------------------------- */

QkVm* qkVmNew(void) {
    QkVm* vm = calloc(1, sizeof(QkVm));
    if (vm) vm->arena = qkArenaNew();
    return vm;
}

void qkVmFree(QkVm* vm) {
    if (!vm) return;
    for (int i = 0; i < vm->hostCount; i++) free(vm->hosts[i].name);
    free(vm->hosts);
    free(vm->diags);
    free(vm->prelude);
    free(vm->capabilities);
    qkArenaFree(vm->arena);
    free(vm);
}

static bool parseText(const char* name, const char* text, size_t len, Program* out) {
    char* copy = qkNew(len + 1);
    memcpy(copy, text, len);
    QkSource src = {qkDup(name, strlen(name)), copy, len};
    return qkParse(&src, out);
}

/* The program, after the prelude's declarations. */
static bool parseWithPrelude(QkVm* vm, const char* name, const char* text, size_t len, Program* out) {
    if (!parseText(name, text ? text : "", text ? len : 0, out)) return false;
    if (vm->prelude) {
        Program prelude;
        if (!parseText("prelude", vm->prelude, strlen(vm->prelude), &prelude)) return false;
        qkAddPrelude(out, &prelude);
    }
    return qkLoadUses(out, name); /* use geometry: geometry.qk next to the program's name (qkModuleReader reads it) */
}

bool qkVmPrelude(QkVm* vm, const char* text) {
    begin(vm);
    Program p;
    bool ok = parseText("prelude", text, strlen(text), &p) && qkCheck(&p, false);
    end();
    if (!ok) return false;
    free(vm->prelude);
    vm->prelude = dupText(text);
    return vm->prelude != NULL;
}

void qkVmCapabilities(QkVm* vm, const char* list) {
    free(vm->capabilities);
    vm->capabilities = NULL;
    if (!list) return;
    size_t n = strlen(list);
    vm->capabilities = malloc(n + 1);
    if (vm->capabilities) memcpy(vm->capabilities, list, n + 1);
}

void qkVmHost(QkVm* vm, const char* name, void* function) {
    for (int i = 0; i < vm->hostCount; i++)
        if (!strcmp(vm->hosts[i].name, name)) {
            vm->hosts[i].fn = function;
            return;
        }
    if (vm->hostCount == vm->hostCap) {
        int cap = vm->hostCap ? vm->hostCap * 2 : 16;
        HostFn* h = realloc(vm->hosts, sizeof(HostFn) * (size_t)cap);
        if (!h) return;
        vm->hosts = h;
        vm->hostCap = cap;
    }
    vm->hosts[vm->hostCount].name = dupText(name);
    vm->hosts[vm->hostCount].fn = function;
    vm->hostCount++;
}

void qkVmFuel(QkVm* vm, int64_t steps) { vm->fuel = steps > 0 ? steps : 0; }
void qkVmMemoryLimit(QkVm* vm, int64_t bytes) { qkArenaLimit(vm->arena, bytes); }
void qkVmChecked(QkVm* vm, bool checked) { vm->checked = checked; }
void qkVmNoAvx(QkVm* vm, bool noAvx) { vm->noAvx = noAvx; }
int qkDiagnosticCount(const QkVm* vm) { return vm->diagCount; }
const QkDiagnostic* qkDiagnosticAt(const QkVm* vm, int i) { return i >= 0 && i < vm->diagCount ? &vm->diags[i] : NULL; }
const char* qkLastError(const QkVm* vm) { return vm->lastError; }

/* ---- compiling ----------------------------------------------------------------------------------- */

static void* hostFor(const QkVm* vm, const char* name) {
    for (int i = 0; i < vm->hostCount; i++)
        if (!strcmp(vm->hosts[i].name, name)) return vm->hosts[i].fn;
    return NULL;
}

static char* signatureOf(const Decl* d) {
    QkBuf b = {0};
    qkBufPrintf(&b, "fn %s(", d->name);
    for (int i = 0; i < d->paramCount; i++) {
        qkBufPrintf(&b, "%s%s: ", i ? ", " : "", d->params[i].name);
        qkTypeName(d->params[i].binding->type, &b);
    }
    qkBufPuts(&b, ")");
    if (d->retType && d->retType->kind != TY_VOID) {
        qkBufPuts(&b, " -> ");
        qkTypeName(d->retType, &b);
    }
    char* s = dupText(b.data);
    qkBufFree(&b);
    return s;
}

QkModule* qkCompile(QkVm* vm, const char* name, const char* text, size_t len) {
    if (!name) name = "program";
    begin(vm);
    Program prog;
    qkCapabilities = vm->capabilities;
    bool checked = parseWithPrelude(vm, name, text, len, &prog) && qkCheck(&prog, false);
    qkCapabilities = NULL;
    if (!checked) {
        end();
        return NULL;
    }
    qkUseAvx = !vm->noAvx && qkCpuHasAvx();
    IrProgram ir;
    qkChecked = vm->checked;
    qkLower(&prog, &ir, vm->fuel > 0);
    qkChecked = false;
    qkOptimize(&ir);
    MachineCode mc;
    qkGenCode(&ir, &mc);
    void** hosts = qkNew(sizeof(void*) * (size_t)(ir.externCount ? ir.externCount : 1));
    for (int i = 0; i < ir.externCount; i++) {
        hosts[i] = hostFor(vm, ir.externs[i].name);
        if (!hosts[i]) {
            Decl* d = qkFindDecl(&prog, ir.externs[i].name);
            char message[200];
            snprintf(message, sizeof message, "extern fn %s has no function in this host (qkVmHost)", ir.externs[i].name);
            collect(prog.src.path, d && !d->fromPrelude ? d->line : 0, d && !d->fromPrelude ? d->col : 0, message);
        }
    }
    if (vm->diagCount) {
        end();
        return NULL;
    }
    QkModule* m = calloc(1, sizeof *m);
    if (!m || !qkLoad((const uint8_t*)mc.code.data, mc.code.len, (const uint8_t*)ir.data.data, ir.data.len, mc.relocs,
                      mc.relocCount, mc.enter, mc.escape, hosts, &m->loaded)) {
        free(m);
        fail(vm, "%s: cannot get executable memory", name);
        end();
        return NULL;
    }
    qkLoadTrace(&m->loaded, &mc, &ir); /* a panic names the functions it went through */
    m->vm = vm;
    m->name = dupText(name);
    m->fns = calloc((size_t)(ir.count ? ir.count : 1), sizeof(ModFn));
    for (int i = 0, k = 0; i < prog.count; i++) {
        Decl* d = prog.decls[i];
        if (d->kind != D_FN || d->isExtern || d->isTemplate || d->inlineOnly) continue; /* as qkLower: the IR's own */
        ModFn* f = &m->fns[m->count++];
        const IrFn* irf = &ir.fns[k];
        f->name = dupText(d->name);
        f->signature = signatureOf(d);
        f->params = irf->paramCount;
        f->results = irf->resultCount;
        f->kinds = calloc((size_t)(f->params + f->results + 1), 1);
        for (int j = 0; j < f->params; j++) f->kinds[j] = irf->kinds[irf->params[j]];
        if (f->results) qkTypeSlotKinds(d->retType, f->kinds + f->params);
        f->at = mc.fnAt[k];
        k++;
    }
    end();
    return m;
}

/* The canonical text of a graph, and its errors pointed at the graph's nodes. */
QkModule* qkCompileGraph(QkVm* vm, const char* name, const char* json, size_t len) {
    char* text = qkGraphToText(vm, json, len);
    if (!text) return NULL;
    QkModule* m = qkCompile(vm, name, text, strlen(text));
    if (!m && vm->diagCount) {
        int count = vm->diagCount;
        QkDiagnostic* saved = malloc(sizeof(QkDiagnostic) * (size_t)count);
        if (saved) memcpy(saved, vm->diags, sizeof(QkDiagnostic) * (size_t)count);
        begin(vm);
        Program prog;
        if (saved && parseText(name ? name : "program", text, strlen(text), &prog))
            for (int i = 0; i < count; i++) qkKeyAtLine(&prog, saved[i].line, saved[i].key, sizeof saved[i].key);
        end();
        if (saved) {
            memcpy(vm->diags, saved, sizeof(QkDiagnostic) * (size_t)count);
            vm->diagCount = count;
            free(saved);
        }
    }
    qkFree(text);
    return m;
}

void qkModuleFree(QkModule* m) {
    if (!m) return;
    qkUnload(&m->loaded);
    for (int i = 0; i < m->count; i++) {
        free(m->fns[i].name);
        free(m->fns[i].signature);
        free(m->fns[i].kinds);
    }
    free(m->fns);
    free(m->name);
    free(m);
}

int qkFunctionCount(const QkModule* m) { return m->count; }

int qkFind(const QkModule* m, const char* name) {
    for (int i = 0; i < m->count; i++)
        if (!strcmp(m->fns[i].name, name)) return i;
    return -1;
}

static const ModFn* fnAt(const QkModule* m, int fn) { return fn >= 0 && fn < m->count ? &m->fns[fn] : NULL; }
const char* qkFunctionName(const QkModule* m, int fn) { return fnAt(m, fn) ? m->fns[fn].name : NULL; }
const char* qkFunctionSignature(const QkModule* m, int fn) { return fnAt(m, fn) ? m->fns[fn].signature : NULL; }
int qkParamSlots(const QkModule* m, int fn) { return fnAt(m, fn) ? m->fns[fn].params : -1; }
int qkResultSlots(const QkModule* m, int fn) { return fnAt(m, fn) ? m->fns[fn].results : -1; }
const char* qkSlotKinds(const QkModule* m, int fn) { return fnAt(m, fn) ? m->fns[fn].kinds : NULL; }

/* A script's function as a task: qkTaskResume runs it until it calls yield() or wait(), or ends. */
QkTaskHandle* qkTaskStart(QkModule* m, int fn, const QkValue* args, int argSlots) {
    QkVm* vm = m->vm;
    const ModFn* f = fnAt(m, fn);
    vm->lastError[0] = 0;
    if (!f || argSlots != f->params || f->results) {
        snprintf(vm->lastError, sizeof vm->lastError, "%s: a task runs a function that gives nothing, with its %d slots", m->name,
                 f ? f->params : 0);
        return NULL;
    }
    QkArena* savedArena = qkArenaUse(vm->arena);
    QkTask* t = qkTaskNew(&m->loaded, m->loaded.code + f->at, (const uint64_t*)args, argSlots);
    qkArenaUse(savedArena);
    QkTaskHandle* h = t ? calloc(1, sizeof *h) : NULL;
    if (h) {
        h->task = t;
        h->module = m;
    }
    return h;
}

int qkTaskResume(QkTaskHandle* h) {
    if (!h) return -1;
    QkVm* vm = h->module->vm;
    int64_t savedFuel = qkFuel;
    const char* savedPath = qkRunPath;
    if (vm->fuel > 0) qkFuel = vm->fuel;
    qkRunPath = h->module->name;
    QkArena* savedArena = qkArenaUse(vm->arena);
    int r = qkTaskStep(h->task);
    qkArenaUse(savedArena);
    qkFuel = savedFuel;
    qkRunPath = savedPath;
    if (r < 0) snprintf(vm->lastError, sizeof vm->lastError, "%s", qkTaskError(h->task));
    return r;
}

void qkTaskEnd(QkTaskHandle* h) {
    if (!h) return;
    qkTaskFree(h->task);
    free(h);
}

QkModule* qkModuleStatic(QkVm* vm, QkStaticProgram* p) {
    vm->lastError[0] = 0;
    if (!p || p->magic != 0x316B72617551ULL) {
        snprintf(vm->lastError, sizeof vm->lastError, "not a program made by quark object");
        return NULL;
    }
    void** hosts = calloc((size_t)(p->externCount ? p->externCount : 1), sizeof(void*));
    for (int64_t i = 0; i < p->externCount; i++) {
        for (int k = 0; k < vm->hostCount; k++)
            if (!strcmp(vm->hosts[k].name, p->externs[i])) hosts[i] = vm->hosts[k].fn;
        if (!hosts[i]) {
            snprintf(vm->lastError, sizeof vm->lastError, "extern fn %s has no function in this host (qkVmHost)", p->externs[i]);
            free(hosts);
            return NULL;
        }
    }
    for (int64_t i = 0; i < p->tableCount; i++) /* the addresses the code reads: data, not code, so this is allowed */
        p->table[i] = qkRelocTarget((int)p->relocs[2 * i], p->relocs[2 * i + 1], p->data, hosts);
    QkModule* m = calloc(1, sizeof *m);
    m->vm = vm;
    m->name = dupText("program");
    m->loaded.code = (uint8_t*)p->code;
    m->loaded.codeLen = (size_t)p->codeLen;
    m->loaded.linked = true;
    void* at = (uint8_t*)p->code + p->enter;
    memcpy(&m->loaded.enter, &at, sizeof at);
    at = (uint8_t*)p->code + p->escape;
    memcpy(&m->loaded.escape, &at, sizeof at);
    m->fns = calloc((size_t)(p->fnCount ? p->fnCount : 1), sizeof(ModFn));
    for (int64_t i = 0; i < p->fnCount; i++) {
        ModFn* f = &m->fns[m->count++];
        f->name = dupText(p->fns[i].name);
        f->signature = dupText(p->fns[i].name);
        f->params = (int)p->fns[i].params;
        f->results = (int)p->fns[i].results;
        f->kinds = dupText(p->fns[i].kinds);
        f->at = (int32_t)p->fns[i].at;
    }
    free(hosts);
    return m;
}

bool qkCall(QkModule* m, int fn, const QkValue* args, int argSlots, QkValue* results, int resultSlots) {
    QkVm* vm = m->vm;
    const ModFn* f = fnAt(m, fn);
    vm->lastError[0] = '\0';
    if (!f) {
        snprintf(vm->lastError, sizeof vm->lastError, "%s: no function %d", m->name, fn);
        return false;
    }
    if (argSlots != f->params || (results && resultSlots != f->results)) {
        snprintf(vm->lastError, sizeof vm->lastError, "%s: %s takes %d slots and gives %d (called with %d and %d)", m->name,
                 f->signature, f->params, f->results, argSlots, resultSlots);
        return false;
    }
    uint64_t scratch[64];
    uint64_t* out = (uint64_t*)results;
    if (!results && f->results) {
        if (f->results > 64) {
            snprintf(vm->lastError, sizeof vm->lastError, "%s: pass room for the %d result slots", m->name, f->results);
            return false;
        }
        out = scratch;
    }
    int64_t savedFuel = qkFuel;
    const char* savedPath = qkRunPath;
    if (vm->fuel > 0) qkFuel = vm->fuel;
    qkRunPath = m->name;
    QkArena* savedArena = qkArenaUse(vm->arena);
    bool ok = qkRunEntry(&m->loaded, m->loaded.code + f->at, (const uint64_t*)args, argSlots, out, f->results);
    qkArenaUse(savedArena);
    qkFuel = savedFuel;
    qkRunPath = savedPath;
    if (!ok) snprintf(vm->lastError, sizeof vm->lastError, "%s", qkPanicText);
    return ok;
}

/* ---- text and graphs ----------------------------------------------------------------------------- */

static char* takeText(QkBuf* b) {
    char* s = malloc(b->len + 1);
    if (s) {
        memcpy(s, b->data ? b->data : "", b->len);
        s[b->len] = '\0';
    }
    qkBufFree(b);
    return s;
}

void qkFree(void* p) { free(p); }

char* qkFormatText(QkVm* vm, const char* text, size_t len) {
    begin(vm);
    Program p;
    char* s = NULL;
    if (parseText("program", text, len, &p)) {
        QkBuf out = {0};
        qkFormat(&p, &out);
        s = takeText(&out);
    }
    end();
    return s;
}

char* qkTextToGraph(QkVm* vm, const char* text, size_t len, const char* layout) {
    begin(vm);
    Program p;
    char* s = NULL;
    if (parseWithPrelude(vm, "program", text, len, &p) && qkCheck(&p, false)) {
        QkBuf out = {0};
        qkGraphExportWith(&p, &out, layout);
        s = takeText(&out);
    }
    end();
    return s;
}

char* qkGraphToText(QkVm* vm, const char* json, size_t len) {
    begin(vm);
    Program p;
    char* s = NULL;
    if (qkGraphImport(json, len, "graph", &p)) {
        QkBuf out = {0};
        qkFormat(&p, &out);
        s = takeText(&out);
    }
    end();
    return s;
}

/* ---- the catalog: every node an editor can offer ------------------------------------------------- */

typedef struct BuiltinNode {
    const char* name;
    const char* category;
    const char* params; /* "name: Type, ..." (Number: Int or Float; T, U: any; Loop<T>: an array, a range or a loop) */
    const char* returns;
    const char* doc;
} BuiltinNode;

static const BuiltinNode kBuiltins[] = {
    {"sqrt", "Math", "x: Float", "Float", "square root"},
    {"sin", "Math", "x: Float", "Float", "sine (radians)"},
    {"cos", "Math", "x: Float", "Float", "cosine (radians)"},
    {"tan", "Math", "x: Float", "Float", "tangent (radians)"},
    {"asin", "Math", "x: Float", "Float", "arc sine"},
    {"acos", "Math", "x: Float", "Float", "arc cosine"},
    {"atan", "Math", "x: Float", "Float", "arc tangent"},
    {"atan2", "Math", "y: Float, x: Float", "Float", "the angle of (x, y)"},
    {"exp", "Math", "x: Float", "Float", "e to the x"},
    {"log", "Math", "x: Float", "Float", "natural logarithm"},
    {"pow", "Math", "x: Float, y: Float", "Float", "x to the y"},
    {"abs", "Math", "x: Number", "Number", "without its sign"},
    {"floor", "Math", "x: Float", "Int", "rounded down"},
    {"ceil", "Math", "x: Float", "Int", "rounded up"},
    {"round", "Math", "x: Float", "Int", "rounded to the nearest (halves up)"},
    {"toFloat", "Math", "x: Int", "Float", "an Int as a Float"},
    {"toInt", "Math", "x: Float", "Int", "a Float without its fraction"},
    {"min", "Math", "a: Number, b: Number", "Number", "the smaller"},
    {"max", "Math", "a: Number, b: Number", "Number", "the larger"},
    {"clamp", "Math", "value: Number, low: Number, high: Number", "Number", "kept between low and high"},
    {"lerp", "Math", "a: Float, b: Float, t: Float", "Float", "from a to b as t goes 0 to 1"},
    {"map", "Loops", "source: Loop<T>, f: fn(T, Int) -> U", "Loop<U>", "each value changed (f may take the index)"},
    {"filter", "Loops", "source: Loop<T>, keep: fn(T, Int) -> Bool", "Loop<T>", "only the values kept"},
    {"fold", "Loops", "source: Loop<T>, start: A, f: fn(A, T) -> A", "A", "all values into one, from start"},
    {"sum", "Loops", "source: Loop<Number>", "Number", "the total"},
    {"min", "Loops", "source: Loop<Number>", "Number", "the smallest (stops on an empty list)"},
    {"max", "Loops", "source: Loop<Number>", "Number", "the largest (stops on an empty list)"},
    {"any", "Loops", "source: Loop<T>, test: fn(T) -> Bool", "Bool", "true if one passes"},
    {"all", "Loops", "source: Loop<T>, test: fn(T) -> Bool", "Bool", "true if every one passes"},
    {"len", "Loops", "source: Loop<T>", "Int", "how many values"},
    {"toArray", "Loops", "source: Loop<T>", "[T]", "the values, kept in an array"},
    {"fill", "Arrays", "count: Int, value: T", "[T]", "an array of count copies"},
    {"push", "Arrays", "list: [T] (a var), value: T", "", "adds value at the end of a var array"},
    {"first", "Loops", "source: Loop<T>, test: fn(T) -> Bool (may be left out)", "T?", "the first value (that passes the test)"},
    {"last", "Loops", "source: Loop<T>, test: fn(T) -> Bool (may be left out)", "T?", "the last value (that passes the test)"},
    {"count", "Loops", "source: Loop<T>, test: fn(T) -> Bool", "Int", "how many pass the test"},
    {"contains", "Loops", "source: Loop<T>, value: T", "Bool", "true if value is among them"},
    {"indexOf", "Loops", "source: Loop<T>, value: T", "Int?", "where value first is"},
    {"minBy", "Loops", "source: Loop<T>, key: fn(T) -> K", "T", "the value with the smallest key (stops on an empty list)"},
    {"maxBy", "Loops", "source: Loop<T>, key: fn(T) -> K", "T", "the value with the largest key (stops on an empty list)"},
    {"sort", "Arrays", "source: Loop<T>, descending: Bool (may be left out)", "[T]", "in order (numbers, texts, cases, or T.compare); stable"},
    {"sortBy", "Arrays", "source: Loop<T>, key: fn(T) -> K, descending: Bool (may be left out)", "[T]", "in the order of the key; stable"},
    {"take", "Loops", "source: Loop<T>, count: Int", "Loop<T>", "the first count values (fewer if there are fewer)"},
    {"skip", "Loops", "source: Loop<T>, count: Int", "Loop<T>", "all but the first count values"},
    {"countBy", "Loops", "source: Loop<T>, key: fn(T) -> K", "[K: Int]", "how many values give each key"},
    {"mean", "Loops", "source: Loop<Number>", "Float", "the average (stops on an empty list)"},
    {"count", "Loops", "source: Loop<T>", "Int", "how many values"},
    {"get", "Arrays", "list: [T], index: Int", "T?", "the value at index, or None when it is outside"},
    {"panic", "Tests", "message: Text", "Never", "stops the program (the host's call) with the message"},
    {"parallelMap", "Loops", "source: Loop<T>, f: fn(T) -> U", "[U]", "f on every value, on the processor's threads"},
    {"random", "Random", "", "Float", "effect: a Float in [0, 1)"},
    {"randomInt", "Random", "low: Int, high: Int", "Int", "effect: an Int from low to high (high not included)"},
    {"seedRandom", "Random", "seed: Int", "", "effect: starts the random sequence again from seed"},
    {"spawn", "Tasks", "f: fn()", "Int", "effect: a task of f, run a step at a time"},
    {"step", "Tasks", "task: Int", "Bool", "effect: runs the task until it yields; true while it has more"},
    {"yield", "Tasks", "", "", "effect: gives the turn back to whoever stepped the task"},
    {"wait", "Tasks", "seconds: Float", "", "effect: gives the turn back until the time went by"},
    {"reverse", "Arrays", "source: Loop<T>", "[T]", "last first"},
    {"keys", "Maps", "map: [K: V]", "[K]", "the keys, in the order they came"},
    {"values", "Maps", "map: [K: V]", "[V]", "the values, in the order their keys came"},
    {"contains", "Maps", "map: [K: V], key: K", "Bool", "true if the key is in it"},
    {"remove", "Maps", "map: [K: V] (a var), key: K", "", "takes the key out of a var map"},
    {"len", "Maps", "map: [K: V]", "Int", "how many keys"},
    {"Some", "Optionals", "value: T", "T?", "a value that is there (None: none)"},
    {"Ok", "Results", "value: T", "T!", "a value: it worked"},
    {"Err", "Results", "message: Text", "T!", "it did not work, and why"},
    {"index", "Enums", "case: an enum", "Int", "the case's place among its enum's cases (from 0)"},
    {"words", "Text", "text: Text", "[Text]", "the pieces between runs of spaces"},
    {"lines", "Text", "text: Text", "[Text]", "the lines, without their line breaks"},
    {"chars", "Text", "text: Text", "[Text]", "each character (UTF-8)"},
    {"contains", "Text", "text: Text, part: Text", "Bool", "true if part is in the text"},
    {"indexOf", "Text", "text: Text, part: Text", "Int?", "where part starts (in bytes)"},
    {"startsWith", "Text", "text: Text, part: Text", "Bool", "true if the text starts with part"},
    {"endsWith", "Text", "text: Text, part: Text", "Bool", "true if the text ends with part"},
    {"replace", "Text", "text: Text, old: Text, new: Text", "Text", "every old replaced by new"},
    {"upper", "Text", "text: Text", "Text", "A to Z for a to z"},
    {"lower", "Text", "text: Text", "Text", "a to z for A to Z"},
    {"repeat", "Text", "text: Text, times: Int", "Text", "the text so many times"},
    {"join", "Text", "texts: Loop<Text>, separator: Text", "Text", "the texts, separated"},
    {"assert", "Tests", "condition: Bool, message: Text (may be left out)", "", "stops (the test fails) when the condition is false"},
    {"toText", "Text", "value: Int | Float | Bool | Text | a case", "Text", "the value as text (\"{x}\" in a text does it too)"},
    {"len", "Text", "text: Text", "Int", "how many bytes"},
    {"split", "Text", "text: Text, separator: Text", "[Text]", "the pieces between separators (\"\": between words)"},
    {"trim", "Text", "text: Text", "Text", "without spaces and line breaks at either end"},
    {"parseInt", "Text", "text: Text", "Int?", "the Int the text is, or None"},
    {"parseFloat", "Text", "text: Text", "Float?", "the Float the text is, or None"},
    {"print", "Out", "values: Any...", "", "effect: shows the values, separated by spaces"},
    {"now", "Out", "", "Float", "effect: seconds, for timing"},
    {"readLine", "Out", "", "Text?", "effect: the next line of the input (None at its end)"},
    {"readFile", "Out", "path: Text", "Text!", "effect: a file's contents (Err says why it cannot be read)"},
    {"writeFile", "Out", "path: Text, text: Text", "Bool", "effect: replaces a file's contents (false if it cannot)"},
    {"args", "Out", "", "[Text]", "effect: the command line after the program's name"},
    {"printErr", "Out", "values: Any...", "", "effect: like print, to the error stream"},
    {"exit", "Out", "code: Int", "", "effect: ends the program with this status"},
    {"env", "Out", "name: Text", "Text?", "effect: an environment variable"},
    {"fileExists", "Out", "path: Text", "Bool", "effect: true if the file can be read"},
    {"listDir", "Out", "path: Text", "[Text]!", "effect: the names in a folder, sorted"},
    {"time", "Out", "", "Float", "effect: seconds since 1970 (the wall clock)"},
};

/* Is `name` the name of a built-in's value at place `at` (fn split(text, separator): 1 is separator)? The
 * catalog is the one list of the built-ins' values, for named values (split(t, separator: ",")) and for editors. */
bool qkBuiltinParam(const char* fn, int at, const char* name, QkBuf* expected) {
    bool any = false;
    for (size_t i = 0; i < sizeof kBuiltins / sizeof kBuiltins[0]; i++) {
        if (strcmp(kBuiltins[i].name, fn)) continue;
        const char* p = kBuiltins[i].params;
        for (int k = 0; *p && k < at; k++) { /* to the value at place `at` */
            p = strstr(p, ", ");
            if (!p) break;
            p += 2;
        }
        if (!p || !*p) continue;
        size_t n = strcspn(p, ":");
        if (strlen(name) == n && !strncmp(p, name, n)) return true;
        if (expected && !any) qkBufAdd(expected, p, n);
        any = true;
    }
    return false;
}

static void jsonText(QkBuf* b, const char* s) {
    qkBufByte(b, '"');
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            qkBufByte(b, '\\');
            qkBufByte(b, c);
        } else if (c == '\n') {
            qkBufPuts(b, "\\n");
        } else if (c < 0x20) {
            qkBufPrintf(b, "\\u%04x", c);
        } else {
            qkBufByte(b, c);
        }
    }
    qkBufByte(b, '"');
}

static void typeExprText(const TypeExpr* t, QkBuf* b) {
    QkBuf s = {0};
    qkFormatTypeExpr(t, &s);
    jsonText(b, s.data);
    qkBufFree(&s);
}

static void declEntry(QkBuf* out, const Decl* d, bool checked, bool* first) {
    if (d->kind == D_USE) return;
    qkBufPuts(out, *first ? "\n    {\"name\": " : ",\n    {\"name\": ");
    *first = false;
    jsonText(out, d->name);
    const char* kind = d->kind == D_ENUM ? "enum" : d->kind == D_TYPE ? (d->isExtern ? "extern type" : "type")
                     : d->kind == D_CONST ? "const" : (d->isExtern ? "extern fn" : "fn");
    qkBufPuts(out, ", \"kind\": ");
    jsonText(out, kind);
    qkBufPrintf(out, ", \"host\": %s", d->fromPrelude ? "true" : "false");
    if (d->isEffect) qkBufPuts(out, ", \"effect\": true"); /* an action node: on the execution wire */
    if (d->leading) {
        qkBufPuts(out, ", \"doc\": ");
        jsonText(out, d->leading);
    }
    if (d->kind == D_ENUM) {
        qkBufPuts(out, ", \"cases\": [");
        for (int k = 0; k < d->caseCount; k++) {
            if (k) qkBufPuts(out, ", ");
            jsonText(out, d->cases[k]);
        }
        qkBufPuts(out, "]}");
        return;
    }
    if (!(d->kind == D_TYPE && d->isExtern)) {
        qkBufPuts(out, d->kind == D_TYPE ? ", \"fields\": [" : ", \"params\": [");
        for (int k = 0; k < d->paramCount; k++) {
            qkBufPuts(out, k ? ", {\"name\": " : "{\"name\": ");
            jsonText(out, d->params[k].name);
            qkBufPuts(out, ", \"type\": ");
            typeExprText(d->params[k].type, out);
            qkBufPuts(out, "}");
        }
        qkBufPuts(out, "]");
    }
    if (d->kind == D_FN) {
        if (checked && d->retType) {
            QkBuf r = {0};
            if (d->retType->kind != TY_VOID) qkTypeName(d->retType, &r);
            qkBufPuts(out, ", \"returns\": ");
            jsonText(out, r.data ? r.data : "");
            qkBufFree(&r);
        } else if (d->ret) {
            qkBufPuts(out, ", \"returns\": ");
            typeExprText(d->ret, out);
        }
    }
    qkBufPuts(out, "}");
}

char* qkCatalog(QkVm* vm, const char* text, size_t len) {
    QkBuf out = {0};
    qkBufPrintf(&out, "{\"quark\": %d,\n \"builtins\": [", QUARK_VERSION);
    for (size_t i = 0; i < sizeof kBuiltins / sizeof kBuiltins[0]; i++) {
        const BuiltinNode* n = &kBuiltins[i];
        qkBufPuts(&out, i ? ",\n    {\"name\": " : "\n    {\"name\": ");
        jsonText(&out, n->name);
        qkBufPuts(&out, ", \"category\": ");
        jsonText(&out, n->category);
        qkBufPuts(&out, ", \"params\": ");
        jsonText(&out, n->params);
        qkBufPuts(&out, ", \"returns\": ");
        jsonText(&out, n->returns);
        qkBufPuts(&out, ", \"doc\": ");
        jsonText(&out, n->doc);
        qkBufPuts(&out, "}");
    }
    qkBufPuts(&out, "],\n \"types\": [\"Int\", \"Float\", \"Bool\", \"Text\", \"[T]\", \"[K: V]\", \"T?\", \"T!\"],\n"
                    " \"operators\": [\"+\", \"-\", \"*\", \"/\", \"%\", \"==\", \"!=\", \"<\", \"<=\", \">\", \">=\", \"and\", \"or\","
                    " \"not\", \"neg\", \"..\", \"??\", \"?\", \"with\"],\n"
                    " \"statements\": [\"let\", \"var\", \"set\", \"do\", \"if\", \"match\", \"for\", \"while\", \"return\", \"break\","
                    " \"continue\"],\n"
                    " \"decls\": [");
    begin(vm);
    Program p;
    bool first = true;
    if (parseWithPrelude(vm, "program", text, len, &p)) {
        int errors = qkErrorCount;
        bool checked = qkCheck(&p, false) && qkErrorCount == errors;
        for (int i = 0; i < p.count; i++)
            if (!p.decls[i]->templateOf && !p.decls[i]->isTest && !p.decls[i]->lifted) declEntry(&out, p.decls[i], checked, &first);
    }
    end();
    qkBufPuts(&out, "]}\n");
    return takeText(&out);
}
