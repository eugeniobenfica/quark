/* Quark — the embedding API (include/quark.h): what an engine or an editor uses. It wraps the compiler
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
    HostFn* hosts;
    int hostCount, hostCap;
    int64_t fuel;
    bool noAvx;
    QkDiagnostic* diags;
    int diagCount, diagCap;
    char lastError[512];
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

QkVm* qkVmNew(void) { return calloc(1, sizeof(QkVm)); }

void qkVmFree(QkVm* vm) {
    if (!vm) return;
    for (int i = 0; i < vm->hostCount; i++) free(vm->hosts[i].name);
    free(vm->hosts);
    free(vm->diags);
    free(vm->prelude);
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
    return true;
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
    if (!parseWithPrelude(vm, name, text, len, &prog) || !qkCheck(&prog, false)) {
        end();
        return NULL;
    }
    qkUseAvx = !vm->noAvx && qkCpuHasAvx();
    IrProgram ir;
    qkLower(&prog, &ir, vm->fuel > 0);
    qkOptimize(&ir);
    MachineCode mc;
    qkGenX64(&ir, &mc);
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
    m->vm = vm;
    m->name = dupText(name);
    m->fns = calloc((size_t)(ir.count ? ir.count : 1), sizeof(ModFn));
    for (int i = 0, k = 0; i < prog.count; i++) {
        Decl* d = prog.decls[i];
        if (d->kind != D_FN || d->isExtern) continue;
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
    bool ok = qkRunEntry(&m->loaded, m->loaded.code + f->at, (const uint64_t*)args, argSlots, out, f->results);
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
    {"lerp", "Math", "a: Float, b: Float, t: Float", "Float", "from a to b as t goes 0 to 1 (vectors too)"},
    {"Vec2", "Vectors", "x: Float, y: Float", "Vec2", "a 2D vector"},
    {"Vec3", "Vectors", "x: Float, y: Float, z: Float", "Vec3", "a 3D vector"},
    {"Vec4", "Vectors", "x: Float, y: Float, z: Float, w: Float", "Vec4", "a 4D vector (a color: r, g, b, a)"},
    {"length", "Vectors", "v: Vec", "Float", "a vector's length"},
    {"dot", "Vectors", "a: Vec, b: Vec", "Float", "dot product"},
    {"map", "Loops", "source: Loop<T>, f: fn(T, Int) -> U", "Loop<U>", "each value changed (f may take the index)"},
    {"filter", "Loops", "source: Loop<T>, keep: fn(T, Int) -> Bool", "Loop<T>", "only the values kept"},
    {"fold", "Loops", "source: Loop<T>, start: A, f: fn(A, T) -> A", "A", "all values into one, from start"},
    {"sum", "Loops", "source: Loop<Number>", "Number", "the total (vectors too)"},
    {"min", "Loops", "source: Loop<Number>", "Number", "the smallest (stops on an empty list)"},
    {"max", "Loops", "source: Loop<Number>", "Number", "the largest (stops on an empty list)"},
    {"any", "Loops", "source: Loop<T>, test: fn(T) -> Bool", "Bool", "true if one passes"},
    {"all", "Loops", "source: Loop<T>, test: fn(T) -> Bool", "Bool", "true if every one passes"},
    {"histogram", "Loops", "source: Loop<T>, bins: Int, bin: fn(T) -> Int", "[Int]", "how many values fall in each bin"},
    {"len", "Loops", "source: Loop<T>", "Int", "how many values"},
    {"toArray", "Loops", "source: Loop<T>", "[T]", "the values, kept in an array"},
    {"fill", "Arrays", "count: Int, value: T", "[T]", "an array of count copies"},
    {"push", "Arrays", "list: [T] (a var), value: T", "", "adds value at the end of a var array"},
    {"Some", "Optionals", "value: T", "T?", "a value that is there (None: none)"},
    {"toText", "Text", "value: Int | Float | Bool | Text | a case", "Text", "the value as text (\"{x}\" in a text does it too)"},
    {"len", "Text", "text: Text", "Int", "how many bytes"},
    {"print", "Out", "values: Any...", "", "effect: shows the values, separated by spaces"},
    {"now", "Out", "", "Float", "effect: seconds, for timing"},
};

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
    qkBufPuts(out, *first ? "\n    {\"name\": " : ",\n    {\"name\": ");
    *first = false;
    jsonText(out, d->name);
    const char* kind = d->kind == D_ENUM ? "enum" : d->kind == D_TYPE ? (d->isExtern ? "extern type" : "type")
                                                                      : (d->isExtern ? "extern fn" : "fn");
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
    qkBufPuts(&out, "],\n \"types\": [\"Int\", \"Float\", \"Bool\", \"Text\", \"Vec2\", \"Vec3\", \"Vec4\", \"[T]\", \"T?\"],\n"
                    " \"operators\": [\"+\", \"-\", \"*\", \"/\", \"%\", \"==\", \"!=\", \"<\", \"<=\", \">\", \">=\", \"and\", \"or\","
                    " \"not\", \"neg\", \"..\", \"??\", \"with\"],\n"
                    " \"statements\": [\"let\", \"var\", \"set\", \"do\", \"if\", \"match\", \"for\", \"while\", \"return\", \"break\","
                    " \"continue\"],\n"
                    " \"events\": [\"start(self)\", \"update(self, dt)\", \"hit(self, other)\", \"click(self)\"],\n"
                    " \"decls\": [");
    begin(vm);
    Program p;
    bool first = true;
    if (parseWithPrelude(vm, "program", text, len, &p)) {
        int errors = qkErrorCount;
        bool checked = qkCheck(&p, false) && qkErrorCount == errors;
        for (int i = 0; i < p.count; i++) declEntry(&out, p.decls[i], checked, &first);
    }
    end();
    qkBufPuts(&out, "]}\n");
    return takeText(&out);
}
