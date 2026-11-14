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

/* ---- versions side by side: diffs and merges by node key ----------------------------------------- */

/* a program's graph, malloc'd: given as its graph (JSON: '{' first) or as its text */
static char* graphFrom(QkVm* vm, const char* s, size_t len, size_t* outLen) {
    size_t i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++;
    char* json = i < len && s[i] == '{' ? memcpy(malloc(len + 1), s, len) : qkTextToGraph(vm, s, len, NULL);
    if (!json) return NULL;
    if (i < len && s[i] == '{') json[len] = '\0';
    *outLen = strlen(json);
    return json;
}

char* qkDiff(QkVm* vm, const char* a, size_t aLen, const char* b, size_t bLen, bool json) {
    size_t la, lb;
    char* ga = graphFrom(vm, a, aLen, &la);
    char* gb = ga ? graphFrom(vm, b, bLen, &lb) : NULL;
    char* s = NULL;
    if (gb) {
        begin(vm);
        QkBuf out = {0};
        if (qkGraphDiff(ga, la, gb, lb, json, &out) >= 0) s = takeText(&out);
        else qkBufFree(&out);
        end();
    }
    free(ga);
    free(gb);
    return s;
}

char* qkMerge(QkVm* vm, const char* base, size_t baseLen, const char* ours, size_t oursLen, const char* theirs, size_t theirsLen,
              bool graph, char** conflicts) {
    if (conflicts) *conflicts = NULL;
    size_t lens[3];
    char* gs[3] = {NULL, NULL, NULL};
    const char* texts[3] = {base, ours, theirs};
    size_t sizes[3] = {baseLen, oursLen, theirsLen};
    for (int i = 0; i < 3; i++)
        if (!(gs[i] = graphFrom(vm, texts[i], sizes[i], &lens[i]))) {
            for (int k = 0; k < i; k++) free(gs[k]);
            return NULL;
        }
    begin(vm);
    QkBuf merged = {0}, found = {0}, text = {0};
    Program p;
    bool ok = qkGraphMerge(gs[0], lens[0], gs[1], lens[1], gs[2], lens[2], &merged, true, &found) >= 0 &&
              qkGraphImport(merged.data, merged.len, "merge", &p);
    if (ok) qkFormat(&p, &text);
    end();
    qkBufFree(&merged);
    for (int i = 0; i < 3; i++) free(gs[i]);
    if (conflicts) *conflicts = takeText(&found);
    else qkBufFree(&found);
    if (!ok) {
        qkBufFree(&text);
        return NULL;
    }
    if (!graph) return takeText(&text);
    char* g = qkTextToGraph(vm, text.data, text.len, NULL); /* the merged program checked: NULL if it has errors */
    qkBufFree(&text);
    return g;
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

/* ---- pin shapes: a pin's type as data, so an editor checks a wire without reading type text -----------
 *
 * {"kind": "int" | "float" | "bool" | "text" | "any" | "never" | "nothing" | "enum" (any enum's case)}
 * {"kind": "array" | "loop" (an array, a range or a loop) | "range" | "optional" | "result", "element": shape}
 * {"kind": "map", "key": shape, "element": shape}       {"kind": "tuple", "items": [shape...]}
 * {"kind": "fn", "params": [shape...], "returns": shape, "effect": true?}
 * {"kind": "named", "name": "Point", "args": [shape...]?}  a record, an enum, an extern type (Pair<Int, Text>)
 * {"kind": "param", "name": "T", "oneOf": [shape...]?}   a type parameter: every pin with the same name takes the
 *                                                        same type (Number: one of Int, Float)
 * {"kind": "oneOf", "options": [shape...]}               any of these (Int | Float | Text) */

typedef struct Shaper {
    const char* p;
    const char* const* params; /* the type parameters' names */
    int paramCount;
    bool builtin;              /* a built-in's text: its type parameters are single capital letters */
} Shaper;

static void shapeSpaces(Shaper* s) {
    while (*s->p == ' ') s->p++;
}

static bool shapeEat(Shaper* s, const char* word) {
    shapeSpaces(s);
    size_t n = strlen(word);
    if (strncmp(s->p, word, n)) return false;
    s->p += n;
    return true;
}

static size_t shapeWord(Shaper* s) {
    shapeSpaces(s);
    size_t n = 0;
    for (;; n++) { /* geo.Point: a dot inside a name (not "Any...") */
        char c = s->p[n];
        bool letter = ((c | 32) >= 'a' && (c | 32) <= 'z') || c == '_';
        if (!letter && !(n && c >= '0' && c <= '9') && !(n && c == '.' && ((s->p[n + 1] | 32) >= 'a' && (s->p[n + 1] | 32) <= 'z'))) break;
    }
    return n;
}

static void shapeUnion(Shaper* s, QkBuf* out);

static void shapeList(Shaper* s, QkBuf* out, char close) { /* "(A, B)" after its opening: [shape, shape] */
    qkBufByte(out, '[');
    for (int i = 0; !shapeEat(s, close == ')' ? ")" : close == ']' ? "]" : ">") && *s->p; i++) {
        if (i) qkBufPuts(out, ", ");
        const char* at = s->p;
        size_t n = shapeWord(s);
        if (n && s->p[n] == ':' && close == ')') s->p += n + 1; /* (loud: Bool) -> Text: the name is not the type */
        else s->p = at;
        shapeUnion(s, out);
        shapeEat(s, ",");
        if (s->p == at) break;
    }
    qkBufByte(out, ']');
}

static void shapeFn(Shaper* s, QkBuf* out, bool effect) { /* after "(": params, then -> returns */
    qkBufPuts(out, "{\"kind\": \"fn\", \"params\": ");
    shapeList(s, out, ')');
    qkBufPuts(out, ", \"returns\": ");
    if (shapeEat(s, "->")) shapeUnion(s, out);
    else qkBufPuts(out, "{\"kind\": \"nothing\"}");
    if (effect) qkBufPuts(out, ", \"effect\": true");
    qkBufByte(out, '}');
}

static void shapeOne(Shaper* s, QkBuf* out) {
    QkBuf base = {0};
    shapeSpaces(s);
    if (shapeEat(s, "[")) {
        QkBuf first = {0};
        shapeUnion(s, &first);
        if (shapeEat(s, ":")) {
            qkBufPrintf(&base, "{\"kind\": \"map\", \"key\": %s, \"element\": ", first.data);
            shapeUnion(s, &base);
            qkBufByte(&base, '}');
        } else {
            qkBufPrintf(&base, "{\"kind\": \"array\", \"element\": %s}", first.data);
        }
        qkBufFree(&first);
        shapeEat(s, "]");
    } else if (shapeEat(s, "effect (") || shapeEat(s, "effect fn(")) {
        shapeFn(s, &base, true);
    } else if (shapeEat(s, "fn(") || shapeEat(s, "fn (")) {
        shapeFn(s, &base, false);
    } else if (shapeEat(s, "(")) { /* (A, B) -> R, or the tuple (A, B) */
        const char* at = s->p;
        QkBuf items = {0};
        shapeList(s, &items, ')');
        shapeSpaces(s);
        if (!strncmp(s->p, "->", 2)) {
            s->p = at;
            shapeFn(s, &base, false);
        } else {
            qkBufPrintf(&base, "{\"kind\": \"tuple\", \"items\": %s}", items.data);
        }
        qkBufFree(&items);
    } else if (shapeEat(s, "an enum") || shapeEat(s, "a case")) {
        qkBufPuts(&base, "{\"kind\": \"enum\"}");
    } else if (shapeEat(s, "a lambda")) {
        qkBufPuts(&base, "{\"kind\": \"fn\", \"params\": [], \"returns\": {\"kind\": \"any\"}}");
    } else if (shapeEat(s, "a range of ")) {
        qkBufPuts(&base, "{\"kind\": \"range\", \"element\": ");
        shapeOne(s, &base);
        qkBufByte(&base, '}');
    } else if (shapeEat(s, "a loop of ")) {
        qkBufPuts(&base, "{\"kind\": \"loop\", \"element\": ");
        shapeOne(s, &base);
        qkBufByte(&base, '}');
    } else {
        size_t n = shapeWord(s);
        char name[96];
        snprintf(name, sizeof name, "%.*s", (int)(n < sizeof name ? n : sizeof name - 1), s->p);
        s->p += n;
        static const char* const plain[][2] = {{"Int", "int"}, {"Float", "float"}, {"Bool", "bool"}, {"Text", "text"},
                                               {"Any", "any"}, {"Never", "never"}, {"nothing", "nothing"}, {"", "any"}};
        bool done = false;
        for (size_t i = 0; i < sizeof plain / sizeof plain[0] && !done; i++)
            if (!strcmp(name, plain[i][0])) {
                qkBufPrintf(&base, "{\"kind\": \"%s\"}", plain[i][1]);
                done = true;
            }
        bool param = s->builtin && n == 1 && name[0] >= 'A' && name[0] <= 'Z';
        for (int i = 0; i < s->paramCount; i++) param |= !strcmp(s->params[i], name);
        if (done) {
        } else if (!strcmp(name, "Number")) {
            qkBufPuts(&base, "{\"kind\": \"param\", \"name\": \"Number\", \"oneOf\": [{\"kind\": \"int\"}, {\"kind\": \"float\"}]}");
        } else if (!strcmp(name, "Loop") && shapeEat(s, "<")) {
            qkBufPuts(&base, "{\"kind\": \"loop\", \"element\": ");
            shapeUnion(s, &base);
            shapeEat(s, ">");
            qkBufByte(&base, '}');
        } else if (param) {
            qkBufPrintf(&base, "{\"kind\": \"param\", \"name\": \"%s\"}", name);
        } else {
            qkBufPrintf(&base, "{\"kind\": \"named\", \"name\": \"%s\"", name);
            if (shapeEat(s, "<")) {
                qkBufPuts(&base, ", \"args\": ");
                shapeList(s, &base, '>');
            }
            qkBufByte(&base, '}');
        }
    }
    for (;;) { /* T? T! T!ParseError */
        if (*s->p == '?' || *s->p == '!') {
            bool optional = *s->p++ == '?';
            QkBuf wrapped = {0};
            qkBufPrintf(&wrapped, "{\"kind\": \"%s\", \"element\": %s", optional ? "optional" : "result", base.data);
            if (!optional && shapeWord(s)) {
                qkBufPuts(&wrapped, ", \"error\": ");
                shapeOne(s, &wrapped);
            }
            qkBufByte(&wrapped, '}');
            qkBufFree(&base);
            base = wrapped;
        } else {
            break;
        }
    }
    qkBufAdd(out, base.data, base.len);
    qkBufFree(&base);
}

static void shapeUnion(Shaper* s, QkBuf* out) {
    QkBuf first = {0};
    shapeOne(s, &first);
    shapeSpaces(s);
    if (*s->p != '|') {
        qkBufAdd(out, first.data, first.len);
        qkBufFree(&first);
        return;
    }
    qkBufPrintf(out, "{\"kind\": \"oneOf\", \"options\": [%s", first.data);
    qkBufFree(&first);
    while (shapeEat(s, "|")) {
        qkBufPuts(out, ", ");
        shapeOne(s, out);
    }
    qkBufPuts(out, "]}");
}

/* ", \"key\": {shape}" for a type's text */
static void shapeField(QkBuf* out, const char* key, const char* text, const char* const* params, int paramCount, bool builtin) {
    Shaper s = {text ? text : "", params, paramCount, builtin};
    qkBufPrintf(out, ", \"%s\": ", key);
    if (!text || !*text) qkBufPuts(out, "{\"kind\": \"nothing\"}");
    else shapeUnion(&s, out);
}

/* A built-in's pins from its text ("source: Loop<T>, f: fn(T, Int) -> U"): name, shape, and whether it is a var
 * ("(a var)"), may be left out ("(may be left out)") or takes any number of values ("Any..."). */
static void builtinPins(QkBuf* out, const char* params) {
    qkBufPuts(out, ", \"pins\": [");
    Shaper s = {params, NULL, 0, true};
    for (int i = 0; *s.p; i++) {
        shapeSpaces(&s);
        size_t n = strcspn(s.p, ":");
        if (!s.p[n]) break;
        qkBufPuts(out, i ? ", {\"name\": " : "{\"name\": ");
        char name[64];
        snprintf(name, sizeof name, "%.*s", (int)(n < sizeof name ? n : sizeof name - 1), s.p);
        jsonText(out, name);
        s.p += n + 1;
        qkBufPuts(out, ", \"shape\": ");
        shapeUnion(&s, out);
        if (shapeEat(&s, "...")) qkBufPuts(out, ", \"variadic\": true");
        if (shapeEat(&s, "(a var)")) qkBufPuts(out, ", \"mutable\": true");
        if (shapeEat(&s, "(may be left out)")) qkBufPuts(out, ", \"optional\": true");
        qkBufPuts(out, "}");
        if (!shapeEat(&s, ",")) break;
    }
    qkBufPuts(out, "]");
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
            QkBuf t = {0};
            if (d->params[k].type) qkFormatTypeExpr(d->params[k].type, &t);
            shapeField(out, "shape", t.data, d->typeParams, d->typeParamCount, false);
            qkBufFree(&t);
            qkBufPuts(out, "}");
        }
        qkBufPuts(out, "]");
    }
    if (d->kind == D_FN) {
        QkBuf r = {0}; /* as written (Pair<B, A>), else as the checker found it */
        if (d->ret) qkFormatTypeExpr(d->ret, &r);
        else if (checked && d->retType && d->retType->kind != TY_VOID) qkTypeName(d->retType, &r);
        if ((checked && d->retType) || d->ret) {
            qkBufPuts(out, ", \"returns\": ");
            jsonText(out, r.data ? r.data : "");
            shapeField(out, "result", r.data, d->typeParams, d->typeParamCount, false);
        }
        qkBufFree(&r);
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
        builtinPins(&out, n->params);
        shapeField(&out, "result", n->returns, NULL, 0, true);
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
