/* Quark — types: every expression gets one, inferred from its parts; only function parameters (and record
 * fields) are written. An Int goes where a Float is wanted; a loop (map, filter, a range) goes where an array
 * is wanted. A let holding a loop that is used once, in the same loop depth, is fused into that use. */
#include "qk.h"

#include <stdio.h>
#include <stdlib.h>

Type qkTypeError = {TY_ERROR}, qkTypeVoid = {TY_VOID}, qkTypeInt = {TY_INT}, qkTypeFloat = {TY_FLOAT},
     qkTypeBool = {TY_BOOL}, qkTypeText = {TY_TEXT}, qkTypeRange = {TY_RANGE, &qkTypeInt};
Type qkTypeNone = {TY_ENUM}, qkTypeEmpty = {TY_ARRAY}, qkTypeErr = {TY_ENUM}, qkTypeEmptyMap = {TY_MAP}, qkTypeNever = {TY_NEVER};

typedef struct Interned {
    Type* elem;
    Type* array;
    Type* iter;
    Type* option;
    Type* result;
    Type** maps; /* [K: elem] for each K */
    int mapCount, mapCap;
    struct Interned* next;
} Interned;
static Interned* interned;

static Interned* internFor(Type* elem) {
    for (Interned* i = interned; i; i = i->next)
        if (i->elem == elem) return i;
    Interned* i = qkNew(sizeof *i);
    i->elem = elem;
    i->array = qkNew(sizeof(Type));
    *i->array = (Type){TY_ARRAY, elem, NULL};
    i->iter = qkNew(sizeof(Type));
    *i->iter = (Type){TY_ITER, elem, NULL};
    i->next = interned;
    interned = i;
    return i;
}

Type* qkTypeArray(Type* elem) { return internFor(elem)->array; }

static Type** typedResults; /* T!E for an E of its own */
static int typedResultCount, typedResultCap;
static Type** fnTypes; /* (A, B) -> R: one type each */
static int fnTypeCount, fnTypeCap;
static Type** tupleTypes; /* (A, B): one record each */
static Type** entryTypes; /* Entry<K, V>: one record each */
static int entryTypeCount, entryTypeCap;
static int tupleTypeCount, tupleTypeCap;

/* With the compiler's memory (qkNewReset): the interned types lived there. */
void qkTypesReset(void) {
    interned = NULL;
    fnTypes = NULL;
    fnTypeCount = fnTypeCap = 0;
    tupleTypes = NULL;
    tupleTypeCount = tupleTypeCap = 0;
    typedResults = NULL;
    typedResultCount = typedResultCap = 0;
    entryTypes = NULL;
    entryTypeCount = entryTypeCap = 0;
}

/* (Int, Text): a record of its own, its fields 0, 1, ... (t.0), interned */
static Type* typeTuple(Type** elems, int n) {
    for (int i = 0; i < tupleTypeCount; i++) {
        const Decl* d = tupleTypes[i]->decl;
        bool same = d->paramCount == n;
        for (int k = 0; same && k < n; k++) same = d->params[k].binding->type == elems[k];
        if (same) return tupleTypes[i];
    }
    Decl* d = qkNew(sizeof *d);
    d->kind = D_TYPE;
    d->isTuple = true;
    d->state = 2;
    d->paramCount = d->paramCap = n;
    d->params = qkNew(sizeof(Param) * (size_t)n);
    QkBuf name = {0};
    for (int k = 0; k < n; k++) {
        char* fn = qkNew(8);
        snprintf(fn, 8, "%d", k);
        Binding* b = qkNew(sizeof *b);
        b->name = fn;
        b->type = elems[k];
        d->params[k] = (Param){fn, NULL, 0, 0, b, NULL, false};
        qkBufPuts(&name, k ? ", " : "(");
        qkTypeName(elems[k], &name);
    }
    qkBufPuts(&name, ")");
    d->name = qkDup(name.data, name.len);
    qkBufFree(&name);
    d->type = qkNew(sizeof(Type));
    d->type->kind = TY_RECORD;
    d->type->decl = d;
    QK_PUSH(tupleTypes, tupleTypeCount, tupleTypeCap, d->type);
    return d->type;
}

/* Entry<K, V> { key: K, value: V }: a map's entry, as loops go over a map (m.maxBy(e => e.value)), interned */
static Type* typeEntry(Type* key, Type* value) {
    for (int i = 0; i < entryTypeCount; i++) {
        const Decl* d = entryTypes[i]->decl;
        if (d->params[0].binding->type == key && d->params[1].binding->type == value) return entryTypes[i];
    }
    Decl* d = qkNew(sizeof *d);
    d->kind = D_TYPE;
    d->isEntry = true;
    d->state = 2;
    d->paramCount = d->paramCap = 2;
    d->params = qkNew(sizeof(Param) * 2);
    const char* names[2] = {"key", "value"};
    Type* types[2] = {key, value};
    QkBuf name = {0};
    qkBufPuts(&name, "Entry<");
    for (int k = 0; k < 2; k++) {
        Binding* b = qkNew(sizeof *b);
        b->name = names[k];
        b->type = types[k];
        d->params[k] = (Param){names[k], NULL, 0, 0, b, NULL, false};
        if (k) qkBufPuts(&name, ", ");
        qkTypeName(types[k], &name);
    }
    qkBufPuts(&name, ">");
    d->name = qkDup(name.data, name.len);
    qkBufFree(&name);
    d->type = qkNew(sizeof(Type));
    d->type->kind = TY_RECORD;
    d->type->decl = d;
    QK_PUSH(entryTypes, entryTypeCount, entryTypeCap, d->type);
    return d->type;
}

static Type* typeFn(Type** params, int n, Type* ret, bool effect) {
    for (int i = 0; i < fnTypeCount; i++) {
        Type* t = fnTypes[i];
        if (t->paramCount != n || t->elem != ret || t->effect != effect) continue;
        bool same = true;
        for (int k = 0; k < n; k++) same &= t->params[k] == params[k];
        if (same) return t;
    }
    Type* t = qkNew(sizeof(Type));
    t->kind = TY_FN;
    t->paramCount = n;
    t->params = qkNew(sizeof(Type*) * (size_t)(n ? n : 1));
    memcpy(t->params, params, sizeof(Type*) * (size_t)n);
    t->elem = ret;
    t->effect = effect;
    QK_PUSH(fnTypes, fnTypeCount, fnTypeCap, t);
    return t;
}

/* A lambda before it meets the function type it goes into (let f: (Int) -> Int = x => x * 2) */
static Type lambdaType = {TY_FN};
Type* qkTypeIter(Type* elem) { return internFor(elem)->iter; }

/* [K: V]: one type per key and value type */
Type* qkTypeMap(Type* key, Type* value) {
    Interned* in = internFor(value);
    for (int i = 0; i < in->mapCount; i++)
        if (in->maps[i]->key == key) return in->maps[i];
    Type* t = qkNew(sizeof(Type));
    *t = (Type){TY_MAP, value, NULL, key, NULL, 0, NULL};
    QK_PUSH(in->maps, in->mapCount, in->mapCap, t);
    return t;
}

/* T! is an enum of its own, like T?: Err(message: Text), Ok(value: T). The failure comes first, as None does,
 * so case 1 holds the value in both. */
Type* qkTypeResult(Type* elem) {
    Interned* in = internFor(elem);
    if (in->result) return in->result;
    Decl* d = qkNew(sizeof *d);
    d->kind = D_ENUM;
    QkBuf name = {0};
    qkTypeName(elem, &name);
    qkBufPuts(&name, "!");
    d->name = qkDup(name.data, name.len);
    qkBufFree(&name);
    d->cases = qkNew(sizeof(char*) * 2);
    d->cases[0] = "Err";
    d->cases[1] = "Ok";
    d->caseCount = d->caseCap = 2;
    d->caseFields = qkNew(sizeof(Param*) * 2);
    d->caseFieldCount = qkNew(sizeof(int) * 2);
    for (int k = 0; k < 2; k++) {
        d->caseFields[k] = qkNew(sizeof(Param));
        d->caseFields[k][0].name = k ? "value" : "message";
        d->caseFields[k][0].binding = qkNew(sizeof(Binding));
        d->caseFields[k][0].binding->name = d->caseFields[k][0].name;
        d->caseFields[k][0].binding->type = k ? elem : &qkTypeText;
        d->caseFieldCount[k] = 1;
    }
    d->state = 2;
    d->type = qkNew(sizeof(Type));
    *d->type = (Type){TY_ENUM, elem, d, NULL, NULL, 0, NULL};
    d->resultOf = elem;
    d->errorOf = &qkTypeText;
    in->result = d->type;
    return d->type;
}

Type* qkTypeResultOf(Type* elem, Type* error) {
    if (!error || error == &qkTypeText) return qkTypeResult(elem);
    for (int i = 0; i < typedResultCount; i++)
        if (typedResults[i]->decl->resultOf == elem && typedResults[i]->decl->errorOf == error) return typedResults[i];
    Decl* d = qkNew(sizeof *d);
    *d = *qkTypeResult(elem)->decl;
    QkBuf name = {0};
    qkTypeName(elem, &name);
    qkBufPuts(&name, "!");
    qkTypeName(error, &name);
    d->name = qkDup(name.data, name.len);
    qkBufFree(&name);
    d->caseFields = qkNew(sizeof(Param*) * 2);
    d->caseFieldCount = qkNew(sizeof(int) * 2);
    for (int k = 0; k < 2; k++) {
        d->caseFields[k] = qkNew(sizeof(Param));
        d->caseFields[k][0].name = k ? "value" : "error";
        d->caseFields[k][0].binding = qkNew(sizeof(Binding));
        d->caseFields[k][0].binding->name = d->caseFields[k][0].name;
        d->caseFields[k][0].binding->type = k ? elem : error;
        d->caseFieldCount[k] = 1;
    }
    d->errorOf = error;
    d->type = qkNew(sizeof(Type));
    *d->type = (Type){TY_ENUM, elem, d, NULL, NULL, 0, NULL};
    QK_PUSH(typedResults, typedResultCount, typedResultCap, d->type);
    return d->type;
}

static bool isOption(const Type* t) { return t->kind == TY_ENUM && t->decl && t->decl->optionOf; }
static bool isResult(const Type* t) { return t->kind == TY_ENUM && t->decl && t->decl->resultOf; }
Type* qkFallibleValue(const Type* t) {
    if (isOption(t)) return t->decl->optionOf;
    if (isResult(t)) return t->decl->resultOf;
    return NULL;
}

/* T? is an enum of its own: None, Some(value: T). */
Type* qkTypeOption(Type* elem) {
    Interned* in = internFor(elem);
    if (in->option) return in->option;
    Decl* d = qkNew(sizeof *d);
    d->kind = D_ENUM;
    QkBuf name = {0};
    qkTypeName(elem, &name);
    qkBufPuts(&name, "?");
    d->name = qkDup(name.data, name.len);
    qkBufFree(&name);
    d->cases = qkNew(sizeof(char*) * 2);
    d->cases[0] = "None";
    d->cases[1] = "Some";
    d->caseCount = d->caseCap = 2;
    d->caseFields = qkNew(sizeof(Param*) * 2);
    d->caseFieldCount = qkNew(sizeof(int) * 2);
    d->caseFields[1] = qkNew(sizeof(Param));
    d->caseFields[1][0].name = "value";
    d->caseFields[1][0].binding = qkNew(sizeof(Binding));
    d->caseFields[1][0].binding->name = "value";
    d->caseFields[1][0].binding->type = elem;
    d->caseFieldCount[1] = 1;
    d->state = 2;
    d->type = qkNew(sizeof(Type));
    *d->type = (Type){TY_ENUM, elem, d};
    d->optionOf = elem;
    in->option = d->type;
    return d->type;
}

bool qkHasData(const Type* t) {
    if (t->kind != TY_ENUM || !t->decl || !t->decl->caseFieldCount) return false;
    for (int k = 0; k < t->decl->caseCount; k++)
        if (t->decl->caseFieldCount[k]) return true;
    return false;
}

bool qkTypeSame(const Type* a, const Type* b) { return a == b || a->kind == TY_ERROR || b->kind == TY_ERROR; }

void qkTypeName(const Type* t, QkBuf* out) {
    switch (t->kind) {
    case TY_ERROR: qkBufPuts(out, t->typeVar ? t->typeVar : "?"); break;
    case TY_VOID: qkBufPuts(out, "nothing"); break;
    case TY_INT: qkBufPuts(out, "Int"); break;
    case TY_FLOAT: qkBufPuts(out, "Float"); break;
    case TY_BOOL: qkBufPuts(out, "Bool"); break;
    case TY_TEXT: qkBufPuts(out, "Text"); break;
    case TY_ARRAY:
        if (!t->elem) qkBufPuts(out, "[] (of no type yet)");
        else {
            qkBufPuts(out, "[");
            qkTypeName(t->elem, out);
            qkBufPuts(out, "]");
        }
        break;
    case TY_RECORD: case TY_ENUM: case TY_HANDLE:
        if (t == &qkTypeNone) qkBufPuts(out, "None (of no type yet)");
        else if (t == &qkTypeErr) qkBufPuts(out, "Err (of no type yet)");
        else qkBufPuts(out, t->decl->name);
        break;
    case TY_NEVER: qkBufPuts(out, "Never"); break;
    case TY_RANGE: qkBufPuts(out, "a range of Int"); break;
    case TY_ITER: qkBufPuts(out, "a loop of "); qkTypeName(t->elem, out); break;
    case TY_MAP:
        if (!t->key) {
            qkBufPuts(out, "[:] (of no type yet)");
            break;
        }
        qkBufPuts(out, "[");
        qkTypeName(t->key, out);
        qkBufPuts(out, ": ");
        qkTypeName(t->elem, out);
        qkBufPuts(out, "]");
        break;
    case TY_FN:
        if (t == &lambdaType) {
            qkBufPuts(out, "a lambda");
            break;
        }
        if (t->effect) qkBufPuts(out, "effect ");
        qkBufPuts(out, "(");
        for (int i = 0; i < t->paramCount; i++) {
            if (i) qkBufPuts(out, ", ");
            qkTypeName(t->params[i], out);
        }
        qkBufPuts(out, ") -> ");
        qkTypeName(t->elem, out);
        break;
    }
}

/* An enum: its case (1 slot), then every case's data, side by side (a slot never changes its kind). */
static int enumSlots(const Decl* d) {
    int n = 1;
    if (d && d->caseFieldCount)
        for (int k = 0; k < d->caseCount; k++)
            for (int f = 0; f < d->caseFieldCount[k]; f++) n += qkFieldSlots(&d->caseFields[k][f]);
    return n;
}

int qkFieldSlots(const Param* p) { return p->boxed ? 1 : qkTypeSlots(p->binding->type); }

int qkTypeSlots(const Type* t) {
    switch (t->kind) {
    case TY_ENUM: return enumSlots(t->decl);
    case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_HANDLE: case TY_MAP: return 1;
    case TY_FN: return 2; /* a function kept as a value: its address, and its captured values' (or 0) */
    case TY_TEXT: case TY_ARRAY: return 2;
    case TY_RECORD: {
        int n = 0;
        for (int i = 0; i < t->decl->paramCount; i++) n += qkFieldSlots(&t->decl->params[i]);
        return n;
    }
    default: return 0;
    }
}

void qkTypeSlotKinds(const Type* t, char* out) {
    switch (t->kind) {
    case TY_FLOAT: out[0] = 'F'; break;
    case TY_RECORD: {
        int at = 0;
        for (int i = 0; i < t->decl->paramCount; i++) {
            const Param* p = &t->decl->params[i];
            if (p->boxed) out[at] = 'I';
            else qkTypeSlotKinds(p->binding->type, out + at);
            at += qkFieldSlots(p);
        }
        break;
    }
    case TY_ENUM: {
        out[0] = 'I';
        int at = 1;
        const Decl* d = t->decl;
        if (d && d->caseFieldCount)
            for (int k = 0; k < d->caseCount; k++)
                for (int f = 0; f < d->caseFieldCount[k]; f++) {
                    const Param* p = &d->caseFields[k][f];
                    if (p->boxed) out[at] = 'I';
                    else qkTypeSlotKinds(p->binding->type, out + at);
                    at += qkFieldSlots(p);
                }
        break;
    }
    default:
        for (int i = 0; i < qkTypeSlots(t); i++) out[i] = 'I';
    }
}

bool qkIsIterable(const Type* t) { return t->kind == TY_ARRAY || t->kind == TY_RANGE || t->kind == TY_ITER; }
Type* qkElemType(const Type* t) { return t->elem ? t->elem : &qkTypeError; }

const char* const qkBuiltinNames[B_COUNT] = {
    NULL, "sqrt", "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "exp", "log", "pow", "abs", "floor", "ceil",
    "round", "toFloat", "toInt", "min", "max", "clamp", "lerp", "len", "fill", "map", "filter", "fold", "sum", "any",
    "all", "toArray", "print", "now", "readFile", "writeFile", "readLine", "args", "Some", "toText", "push", "split",
    "trim", "parseInt", "parseFloat",
    "Ok", "Err", "words", "lines", "contains", "indexOf", "startsWith", "endsWith", "replace", "upper", "lower",
    "join", "repeat", "chars", "sort", "sortBy", "reverse", "first", "last", "count", "minBy", "maxBy",
    "keys", "values", "remove", "index", "printErr", "exit", "env", "fileExists", "listDir", "time", "assert",
    "parallelMap", "random", "randomInt", "seedRandom", "spawn", "step", "yield", "wait", "take", "skip", "countBy", "mean",
    "panic", "get", "#interface", "#never", "#entries"};

/* ---- the checker --------------------------------------------------------------------------------- */

struct Checker {
    Program* prog;
    const QkSource* src;
    Binding** scope;
    int scopeCount, scopeCap;
    int fnScopeStart;
    Decl* fn;
    int loopDepth;   /* loops and lambda bodies: how often code runs */
    int lambdaDepth;
    int varReads;
    Binding** lets;  /* this function's lets, to decide which are fused */
    int letCount, letCap;
    bool inConst;    /* checking a constant or a default value: no names, no effects */
    int module;      /* the file whose code it checks: what it sees is its own and the pub of the files it uses */
    Type* hint;      /* the type a value goes into, when it is written (let xs: [Speaker] = [...]) */
    struct Checker* outer; /* checking a lambda kept as a value: the function it is written in (its names are out of reach) */
    Type* result;    /* the type the if or match being checked must give (a function's written result): None, Err(...) take it */
    Decl* typing;    /* whose type parameters a type may name (a template, or an instance) */
};

static void boxIfRecursive(Param* prm, const Type* owner);
typedef struct Checker Checker;
static Decl* findDecl(Checker* c, const char* name);
static Type* instanceType(Checker* c, Decl* d, Type** args);
static void enterDecl(Checker* c, const Decl* d);
/* A generic type's own type, before its types are known (Tree.Leaf of enum Tree<T>): it takes the Tree<Int> it
 * goes into, as None takes the optional. */
static bool isPlaceholder(const Type* t) {
    return (t->kind == TY_ENUM || t->kind == TY_RECORD) && t->decl && t->decl->isTemplate && t->decl->kind != D_FN;
}
static int instantiating; /* instances being made inside each other: a template that calls itself forever stops */

static Type* checkExpr(Checker* c, Expr* e, bool wantValue);

static void typeError(Checker* c, const Expr* e, const char* fmt, const Type* a, const Type* b) {
    if ((a && a->kind == TY_ERROR) || (b && b->kind == TY_ERROR)) return; /* already said */
    QkBuf na = {0}, nb = {0};
    if (a) qkTypeName(a, &na);
    if (b) qkTypeName(b, &nb);
    qkError(c->src, e->line, e->col, fmt, na.data ? na.data : "", nb.data ? nb.data : "");
    qkBufFree(&na);
    qkBufFree(&nb);
}

/* A map's key: Int, Text, Bool, or an enum's case without data. */
static bool keyable(const Type* t) {
    switch (t->kind) {
    case TY_INT: case TY_TEXT: case TY_BOOL: case TY_ERROR: return true;
    case TY_ENUM: return !qkHasData(t) && t != &qkTypeNone && t != &qkTypeErr;
    case TY_RECORD: { /* Vec2i { x: Int, y: Int }, (Int, Int): one or two Ints, Bools or cases */
        if (!t->decl || t->decl->paramCount < 1 || t->decl->paramCount > 2) return false;
        for (int f = 0; f < t->decl->paramCount; f++) {
            const Param* prm = &t->decl->params[f];
            if (!prm->binding) { /* not resolved yet (declared further down): by the name written */
                const char* n = prm->type ? prm->type->name : NULL;
                if (!n || prm->type->optional || prm->type->fallible || (strcmp(n, "Int") && strcmp(n, "Bool"))) return false;
                continue;
            }
            const Type* ft = prm->binding->type;
            if (prm->boxed || (ft->kind != TY_INT && ft->kind != TY_BOOL && ft->kind != TY_HANDLE && !(ft->kind == TY_ENUM && keyable(ft))))
                return false;
        }
        return true;
    }
    default: return false;
    }
}

static Type* resolveType(Checker* c, TypeExpr* te) {
    if (te->optional || te->fallible) {
        Type* inner = resolveType(c, te->elem);
        if (qkFallibleValue(inner)) {
            qkError(c->src, te->line, te->col, "an optional or a result of one: one ? or ! is enough");
            return inner;
        }
        if (inner->kind == TY_ERROR) return inner;
        if (te->fallible && te->errType) { /* T!ParseError */
            Type* err = resolveType(c, te->errType);
            if (err->kind == TY_ERROR) return err;
            return qkTypeResultOf(inner, err);
        }
        return te->fallible ? qkTypeResult(inner) : qkTypeOption(inner);
    }
    if (te->isTuple) { /* (A, B) */
        Type* elems[16];
        for (int i = 0; i < te->paramCount && i < 16; i++) {
            elems[i] = resolveType(c, te->params[i]);
            if (elems[i]->kind == TY_ERROR) return elems[i];
        }
        return typeTuple(elems, te->paramCount);
    }
    if (te->isFn) { /* (A, B) -> R: a function */
        Type* params[16];
        bool loose = false;
        for (int i = 0; i < te->paramCount && i < 16; i++) loose |= (params[i] = resolveType(c, te->params[i]))->kind == TY_ERROR;
        Type* ret = te->elem ? resolveType(c, te->elem) : &qkTypeVoid; /* an interface's fn log(...): nothing */
        if (loose || ret->kind == TY_ERROR) { /* a template's T inside: a type of its own, not interned */
            Type* t = qkNew(sizeof(Type));
            t->kind = TY_FN;
            t->paramCount = te->paramCount;
            t->params = qkNew(sizeof(Type*) * (size_t)(te->paramCount ? te->paramCount : 1));
            memcpy(t->params, params, sizeof(Type*) * (size_t)te->paramCount);
            t->elem = ret;
            t->effect = te->effect;
            return t;
        }
        return typeFn(params, te->paramCount, ret, te->effect);
    }
    if (!te->name && te->key) {
        Type* k = resolveType(c, te->key);
        Type* v = resolveType(c, te->elem);
        if (!keyable(k)) {
            QkBuf n = {0};
            qkTypeName(k, &n);
            qkError(c->src, te->line, te->col, "a map's key is an Int, a Text, a Bool, an enum's case or a record of two Ints, not %s", n.data);
            qkBufFree(&n);
            return &qkTypeError;
        }
        if (k->kind == TY_ERROR || v->kind == TY_ERROR) return k->kind == TY_ERROR ? k : v;
        return qkTypeMap(k, v);
    }
    if (!te->name) {
        Type* elem = resolveType(c, te->elem);
        return elem->kind == TY_ERROR ? elem : qkTypeArray(elem);
    }
    Decl* owner = c->typing;
    for (int i = 0; owner && owner->typeArgs && i < owner->typeParamCount; i++)
        if (!strcmp(owner->typeParams[i], te->name)) return owner->typeArgs[i]; /* T: what it stands for */
    if (!strcmp(te->name, "Int")) return &qkTypeInt;
    if (!strcmp(te->name, "Float")) return &qkTypeFloat;
    if (!strcmp(te->name, "Bool")) return &qkTypeBool;
    if (!strcmp(te->name, "Text")) return &qkTypeText;
    if (!strcmp(te->name, "Never")) return &qkTypeNever;
    Decl* d = findDecl(c, te->name);
    if (!d && !strcmp(te->name, "Entry") && te->argCount == 2) { /* a map's entry: Entry<Text, Int> */
        Type* k = resolveType(c, te->args[0]);
        Type* v = resolveType(c, te->args[1]);
        if (k->kind == TY_ERROR || v->kind == TY_ERROR) return k->kind == TY_ERROR ? k : v;
        return typeEntry(k, v);
    }
    if (d && d->kind != D_FN && d->kind != D_CONST) {
        if (d->typeParamCount != te->argCount) {
            if (d->typeParamCount)
                qkError(c->src, te->line, te->col, "%s takes %d type%s: %s<%s%s>", d->name, d->typeParamCount, d->typeParamCount == 1 ? "" : "s",
                        d->name, d->typeParams[0], d->typeParamCount > 1 ? ", ..." : "");
            else qkError(c->src, te->line, te->col, "%s takes no types", d->name);
            return &qkTypeError;
        }
        if (!te->argCount) return d->type;
        Type* args[16];
        for (int i = 0; i < te->argCount && i < 16; i++) {
            args[i] = resolveType(c, te->args[i]);
            if (args[i]->kind == TY_ERROR) return args[i]; /* inside a template: T is not known yet */
        }
        return instanceType(c, d, args);
    }
    qkError(c->src, te->line, te->col, "unknown type \"%s\"", te->name);
    return &qkTypeError;
}

Decl* qkFindDecl(Program* p, const char* name) {
    for (int i = 0; i < p->count; i++)
        if (p->decls[i]->kind != D_USE && !strcmp(p->decls[i]->name, name)) return p->decls[i];
    return NULL;
}

/* Does module `from` use module `to` (by its names: not through an alias, unless `aliased`)? */
static bool usesModuleAs(const Program* p, int from, int to, bool aliased) {
    if (from < 0 || from >= p->moduleCount) return false;
    for (int i = 0; i < p->modules[from].useCount; i++)
        if (p->modules[from].uses[i] == to && (aliased || !p->modules[from].aliases || !p->modules[from].aliases[i])) return true;
    return false;
}

static bool usesModule(const Program* p, int from, int to) { return usesModuleAs(p, from, to, false); }

/* What code in module m sees: its own declarations, the host's, and the pub ones of the files it uses (a type's
 * own functions, Vec2.plus, also through an alias: a + b works on a g.Vec2). */
static bool seenFrom(const Program* p, int m, const Decl* d) {
    if (d->kind == D_USE) return false;
    if (d->fromPrelude || d->module == m) return true;
    return d->isPub && usesModuleAs(p, m, d->module, d->owner != NULL);
}

/* use geometry as g: the module g stands for in module m, or -1 */
static int aliasModule(const Program* p, int m, const char* alias, size_t n) {
    if (m < 0 || m >= p->moduleCount || !p->modules[m].aliases) return -1;
    for (int i = 0; i < p->modules[m].useCount; i++) {
        const char* a = p->modules[m].aliases[i];
        if (a && strlen(a) == n && !strncmp(a, alias, n)) return p->modules[m].uses[i];
    }
    return -1;
}

static Decl* findDecl(Checker* c, const char* name) {
    const char* dot = strchr(name, '.');
    if (dot && name[0] >= 'a' && name[0] <= 'z') { /* g.area: a pub declaration of the module g stands for */
        int m = aliasModule(c->prog, c->module, name, (size_t)(dot - name));
        if (m >= 0)
            for (int i = 0; i < c->prog->count; i++) {
                Decl* d = c->prog->decls[i];
                if (d->module == m && d->isPub && d->kind != D_USE && !strcmp(d->name, dot + 1)) return d;
            }
    }
    for (int i = 0; i < c->prog->count; i++) {
        Decl* d = c->prog->decls[i];
        if (!strcmp(d->name, name) && seenFrom(c->prog, c->module, d)) return d;
    }
    return NULL;
}

/* Shape, g.Shape: a name whose last part is a type's */
static bool typeLike(const char* name) {
    const char* last = strrchr(name, '.');
    last = last ? last + 1 : name;
    return last[0] >= 'A' && last[0] <= 'Z';
}

/* The checker set to the file d comes from: its errors there, its names in reach. */
static void enterDecl(Checker* c, const Decl* d) {
    c->module = d->module;
    c->src = d->src ? d->src : &c->prog->src;
}

static Binding* lookup(Checker* c, const char* name) {
    for (int i = c->scopeCount - 1; i >= 0; i--)
        if (!strcmp(c->scope[i]->name, name)) return c->scope[i];
    return NULL;
}

/* A template's parameter that takes a function is used as a value (kept, captured, given back): from now on
 * it is given a function value, not a lambda put in. */
static void keepsFunction(Binding* b) {
    Decl* d = b->paramOf;
    if (!d || !d->isTemplate) return;
    if (!d->valueParams) d->valueParams = qkNew(sizeof(bool) * (size_t)(d->paramCount ? d->paramCount : 1));
    d->valueParams[b->paramIndex] = true;
}

/* A name, in reach here, or around the lambda kept as a value being checked: then it is captured (its value,
 * copied where the lambda is made) and the lambda's own binding for it is given. */
static Binding* lookupOrCapture(Checker* c, const char* name) {
    Binding* b = lookup(c, name);
    if (b || !c->outer || !c->fn || !c->fn->lifted) return b;
    Decl* d = c->fn;
    for (int i = 0; i < d->captureCount; i++)
        if (!strcmp(d->captures[i].inner->name, name)) return d->captures[i].inner;
    Binding* ob = lookupOrCapture(c->outer, name);
    if (ob && ob->fnParam) keepsFunction(ob); /* the template keeps the function it is given: a value */
    if (!ob || (ob->fnParam && !(ob->paramOf && ob->paramOf->isTemplate)) || ob->lambda) return NULL;
    ob->uses++;
    ob->iterUses += 1000; /* stored, never fused into the lambda */
    Binding* in = qkNew(sizeof *in);
    *in = (Binding){0};
    in->name = ob->name;
    in->type = ob->type;
    in->line = ob->line;
    in->col = ob->col;
    in->capturedFrom = ob;
    struct Capture cap = {ob, in};
    QK_PUSH(d->captures, d->captureCount, d->captureCap, cap);
    return in;
}

static Binding* declare(Checker* c, const char* name, Type* type, bool mutable, int line, int col) {
    Binding* old = lookup(c, name);
    if (old && !old->isParam) {
        qkError(c->src, line, col, "\"%s\" is already defined here: give it another name", name);
    } else if (old) {
        qkError(c->src, line, col, "\"%s\" is a parameter: give it another name", name);
    }
    Binding* b = qkNew(sizeof *b);
    b->name = name;
    b->type = type;
    b->mutable = mutable;
    b->depth = c->loopDepth;
    b->line = line;
    b->col = col;
    QK_PUSH(c->scope, c->scopeCount, c->scopeCap, b);
    return b;
}

/* "did you mean" over every name in reach */
static void unknownName(Checker* c, const Expr* e) {
    const char* names[512];
    int n = 0;
    for (int i = 0; i < c->scopeCount && n < 400; i++) names[n++] = c->scope[i]->name;
    for (int i = 0; i < c->prog->count && n < 480; i++)
        if (seenFrom(c->prog, c->module, c->prog->decls[i])) names[n++] = c->prog->decls[i]->name;
    for (int i = 1; i < B_COUNT && n < 512; i++) names[n++] = qkBuiltinNames[i];
    for (struct Checker* o = c->outer; o; o = o->outer)
        if (lookup(o, e->name)) {
            qkError(c->src, e->line, e->col, "a function is not a value to keep here: %s was given to the function around this lambda, so call it there",
                    e->name);
            return;
        }
    const char* close = qkClosest(e->name, names, n);
    if (close) qkError(c->src, e->line, e->col, "unknown name \"%s\": did you mean \"%s\"?", e->name, close);
    else qkError(c->src, e->line, e->col, "unknown name \"%s\"", e->name);
}

/* Wraps *slot in an implicit builtin call (a conversion the reader does not see). */
static void wrapImplicit(Expr** slot, int builtin, Type* type) {
    Expr* inner = *slot;
    Expr* w = qkNewExpr(E_CALL, inner->line, inner->col);
    w->name = qkBuiltinNames[builtin];
    w->builtin = builtin;
    w->implicit = true;
    w->args = qkNew(sizeof(Expr*));
    w->argNames = qkNew(sizeof(char*));
    w->args[0] = inner;
    w->argCount = w->argCap = 1;
    w->type = type;
    *slot = w;
}

static void checkFnBody(Checker* c, Decl* d);
static void markIterUse(Expr* e);
static bool coerce(Checker* c, Expr** slot, Type* want, const char* where);

/* A lambda kept as a value (let f: (Int) -> Int = x => x * 2): a function of its own, which uses its parameters,
 * constants and functions, never the names around it (nothing is captured). */
static bool liftLambdaAs(Checker* c, Expr* lam, Type* want, bool inferEffect);
static bool liftLambda(Checker* c, Expr* lam, Type* want) { return liftLambdaAs(c, lam, want, false); }

static bool liftLambdaAs(Checker* c, Expr* lam, Type* want, bool inferEffect) {
    if (lam->paramCount != want->paramCount) {
        qkError(c->src, lam->line, lam->col, "this lambda takes %d value%s, but %d go into it here", lam->paramCount,
                lam->paramCount == 1 ? "" : "s", want->paramCount);
        return false;
    }
    Decl* d = qkNew(sizeof *d);
    d->kind = D_FN;
    d->lifted = true;
    char* name = qkNew(32);
    snprintf(name, 32, "lambda at line %d", lam->line);
    d->name = name;
    d->line = lam->line;
    d->col = lam->col;
    d->module = c->module;
    d->src = c->src;
    d->isEffect = want->effect;
    d->inferEffect = inferEffect;
    d->paramCount = d->paramCap = lam->paramCount + 1;
    d->params = qkNew(sizeof(Param) * (size_t)(lam->paramCount + 1));
    d->env = qkNew(sizeof(Binding));
    d->env->name = "#env";
    d->env->type = &qkTypeInt;
    d->env->isParam = true;
    d->params[lam->paramCount] = (Param){"#env", NULL, lam->line, lam->col, d->env, NULL, false};
    for (int i = 0; i < lam->paramCount; i++) {
        Binding* b = qkNew(sizeof *b);
        b->name = lam->params[i].name;
        b->type = want->params[i];
        b->isParam = true;
        b->line = lam->params[i].line;
        b->col = lam->params[i].col;
        lam->params[i].binding = b;
        d->params[i] = (Param){lam->params[i].name, NULL, lam->params[i].line, lam->params[i].col, b, NULL, false};
    }
    d->body = lam->body;
    d->retType = want->elem; /* NULL: what the body gives */
    d->index = c->prog->count;
    if ((c->typing && c->typing->isTemplate) || (c->fn && c->fn->isTemplate)) d->isTemplate = true; /* never lowered */
    QK_PUSH(c->prog->decls, c->prog->count, c->prog->cap, d);
    Checker lc = {0};
    lc.prog = c->prog;
    lc.outer = c;
    checkFnBody(&lc, d);
    lam->decl = d;
    lam->type = want->elem ? want : typeFn(want->params, want->paramCount, d->retType ? d->retType : &qkTypeError, want->effect || d->isEffect);
    return true;
}

/* What parallelMap's function gives back: plain values (numbers, Bools, cases, records of them), so each thread's
 * memory can go when it is done. */
static bool plainValue(const Type* t) {
    switch (t->kind) {
    case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_HANDLE: return true;
    case TY_ENUM:
        for (int k = 0; qkHasData(t) && k < t->decl->caseCount; k++)
            for (int f = 0; f < t->decl->caseFieldCount[k]; f++)
                if (t->decl->caseFields[k][f].boxed || !plainValue(t->decl->caseFields[k][f].binding->type)) return false;
        return t != &qkTypeNone && t != &qkTypeErr;
    case TY_RECORD:
        for (int f = 0; f < t->decl->paramCount; f++)
            if (t->decl->params[f].boxed || !plainValue(t->decl->params[f].binding->type)) return false;
        return true;
    default: return false;
    }
}

/* parallelMap(xs, f): f on every element, the elements shared out to the processor's threads. f is a function
 * (a name, or a lambda that uses only its parameter: each thread runs it on its own). */
static Type* checkParallelMap(Checker* c, Expr* e) {
    if (e->argCount != 2) {
        qkError(c->src, e->line, e->col, "parallelMap takes an array and a function: xs.parallelMap(f)");
        return &qkTypeError;
    }
    Type* t = checkExpr(c, e->args[0], true);
    if (!qkIsIterable(t)) {
        if (t->kind != TY_ERROR) typeError(c, e->args[0], "parallelMap goes over an array, a range or a loop, not %s", t, NULL);
        return &qkTypeError;
    }
    markIterUse(e->args[0]);
    if (t->kind != TY_ARRAY) coerce(c, &e->args[0], qkTypeArray(t->elem), "parallelMap");
    Type* elem = t->elem;
    Type* ft;
    if (e->args[1]->kind == E_LAMBDA) {
        Type proto = {TY_FN};
        proto.params = &elem;
        proto.paramCount = 1;
        if (!liftLambda(c, e->args[1], &proto)) return &qkTypeError;
        ft = e->args[1]->type;
    } else {
        ft = checkExpr(c, e->args[1], true);
    }
    if (ft->kind == TY_ERROR) return ft;
    if (ft->kind != TY_FN || ft->paramCount != 1 || !qkTypeSame(ft->params[0], elem)) {
        QkBuf want = {0};
        qkTypeName(elem, &want);
        typeError(c, e->args[1], "parallelMap needs a function of one %s here, not %s", NULL, ft);
        qkBufFree(&want);
        return &qkTypeError;
    }
    if (ft->effect) {
        qkError(c->src, e->args[1]->line, e->args[1]->col, "parallelMap runs pure functions (no effects): its threads change nothing");
        return &qkTypeError;
    }
    if (!plainValue(ft->elem)) {
        typeError(c, e->args[1], "parallelMap gives numbers, Bools, cases or records of them, not %s", ft->elem, NULL);
        return &qkTypeError;
    }
    e->builtin = B_PARALLELMAP;
    return qkTypeArray(ft->elem);
}

/* A function's name where a value goes: the function itself, as a value */
static Type* fnValueType(Checker* c, Decl* d, const Expr* at);

/* An interface: a record whose fields are all functions (type Speaker { speak: () -> Text }). Any type with a
 * function of its own for each field (fn Dog.speak(d: Dog) -> Text) goes where one is wanted: the record of
 * closures over the value, so a [Speaker] holds dogs and cats. */
static bool isInterface(const Type* t) { return (t->kind == TY_RECORD) && t->decl && t->decl->isInterface; }

static Decl* typeFunction(Checker* c, const Type* t, const char* name);
static Type* fnReturnType(Checker* c, Decl* d, const Expr* at);

static bool hasOperators(const Type* t);
static bool effectOf(Checker* c, Decl* d);
static bool toInterface(Checker* c, Expr** slot, Type* have, Type* want) {
    Decl* w = want->decl;
    QkBuf hn = {0}, wn = {0};
    qkTypeName(have, &hn);
    qkTypeName(want, &wn);
    for (int f = 0; f < w->paramCount; f++) { /* each field: the type's own function, of the same type */
        Type* ft = w->params[f].binding->type;
        Decl* m = typeFunction(c, have, w->params[f].name);
        bool fits = m && m->paramCount == ft->paramCount + 1 && (!effectOf(c, m) || ft->effect);
        for (int i = 0; fits && i < ft->paramCount; i++) fits = m->params[i + 1].binding->type == ft->params[i];
        if (fits) fits = fnReturnType(c, m, *slot) == ft->elem;
        if (!fits) {
            QkBuf sig = {0};
            qkTypeName(ft, &sig);
            qkError(c->src, (*slot)->line, (*slot)->col, "%s cannot be a %s: it needs fn %s.%s(self, ...) of the type %s", hn.data, wn.data,
                    hn.data, w->params[f].name, sig.data);
            qkBufFree(&sig);
            qkBufFree(&hn);
            qkBufFree(&wn);
            return false;
        }
    }
    qkBufFree(&hn);
    qkBufFree(&wn);
    Expr* inner = *slot;
    Binding* self = qkNew(sizeof *self);
    self->name = "#self";
    self->type = have;
    self->line = inner->line;
    self->col = inner->col;
    QK_PUSH(c->scope, c->scopeCount, c->scopeCap, self);
    Expr* wrap = qkNewExpr(E_CALL, inner->line, inner->col);
    wrap->name = "#interface";
    wrap->builtin = B_INTERFACE;
    wrap->implicit = true;
    wrap->type = want;
    wrap->binding = self;
    wrap->argCount = wrap->argCap = 1 + w->paramCount;
    wrap->args = qkNew(sizeof(Expr*) * (size_t)wrap->argCount);
    wrap->argNames = qkNew(sizeof(char*) * (size_t)wrap->argCount);
    wrap->args[0] = inner;
    bool ok = true;
    for (int f = 0; f < w->paramCount; f++) { /* (a, b) => #self.name(a, b), kept as a value */
        Type* ft = w->params[f].binding->type;
        Expr* lam = qkNewExpr(E_LAMBDA, inner->line, inner->col);
        lam->paramCount = ft->paramCount;
        lam->params = qkNew(sizeof(Param) * (size_t)(ft->paramCount + 1));
        Expr* call = qkNewExpr(E_CALL, inner->line, inner->col);
        call->name = w->params[f].name;
        call->method = true;
        call->argCount = call->argCap = ft->paramCount + 1;
        call->args = qkNew(sizeof(Expr*) * (size_t)call->argCount);
        call->argNames = qkNew(sizeof(char*) * (size_t)call->argCount);
        call->args[0] = qkNewExpr(E_NAME, inner->line, inner->col);
        call->args[0]->name = "#self";
        for (int i = 0; i < ft->paramCount; i++) {
            char* pn = qkNew(8);
            snprintf(pn, 8, "#a%d", i);
            lam->params[i] = (Param){pn, NULL, inner->line, inner->col, NULL, NULL, false};
            call->args[i + 1] = qkNewExpr(E_NAME, inner->line, inner->col);
            call->args[i + 1]->name = pn;
        }
        lam->body = call;
        ok &= liftLambda(c, lam, ft);
        wrap->args[1 + f] = lam;
    }
    c->scopeCount--;
    *slot = wrap;
    return ok;
}

/* Makes *slot fit `want`, converting when Quark allows it. */
static bool coerce(Checker* c, Expr** slot, Type* want, const char* where) {
    Type* have = (*slot)->type;
    if (qkTypeSame(have, want)) return true;
    if (have->kind == TY_NEVER) { /* exit(1), panic("...") where a value goes: it never gets there */
        if (want->kind != TY_VOID && want->kind != TY_NEVER) wrapImplicit(slot, B_NEVER, want);
        return true;
    }
    if (have == &qkTypeNone && want->kind == TY_ENUM && want->decl && want->decl->optionOf) { /* None takes the type */
        Expr* e = *slot;
        e->type = want;
        e->decl = want->decl;
        return true;
    }
    if (have == &lambdaType && want->kind == TY_FN && (*slot)->kind == E_LAMBDA) return liftLambda(c, *slot, want);
    if (have->kind == TY_FN && want->kind == TY_FN && have != &lambdaType && !have->effect && want->effect &&
        typeFn(have->params, have->paramCount, have->elem, true) == want) { /* a pure function where an effect one goes */
        (*slot)->type = want;
        return true;
    }
    if (isPlaceholder(have) && want->decl && want->decl->templateOf == have->decl) { /* Tree.Leaf takes the Tree<T> */
        (*slot)->type = want;
        (*slot)->decl = want->decl;
        return true;
    }
    if (have == &qkTypeEmpty && want->kind == TY_ARRAY) { /* [] takes the type */
        (*slot)->type = want;
        return true;
    }
    if (have == &qkTypeEmptyMap && want->kind == TY_MAP) { /* [:] takes the type */
        (*slot)->type = want;
        return true;
    }
    if (have == &qkTypeErr && isResult(want)) { /* Err("...") takes the T! it goes into, and holds its error type */
        Expr* err = *slot;
        while (err->kind == E_CALL && err->implicit) err = err->args[0];
        if (err->kind == E_CALL && err->builtin == B_ERR && err->argCount == 1 &&
            !coerce(c, &err->args[0], want->decl->errorOf, "Err's value"))
            return false;
        (*slot)->type = want;
        return true;
    }
    bool haveFallible = qkFallibleValue(have) != NULL;
    if (isOption(want) && !haveFallible && have != &qkTypeNone && have != &qkTypeErr) {
        /* a T (or an Int for a Float?) where a T? goes: Some(it) */
        if (!coerce(c, slot, want->decl->optionOf, where)) return false;
        wrapImplicit(slot, B_SOME, want);
        return true;
    }
    if (isResult(want) && !haveFallible && have != &qkTypeNone && have != &qkTypeErr) {
        /* a T where a T! goes: Ok(it) */
        if (!coerce(c, slot, want->decl->resultOf, where)) return false;
        wrapImplicit(slot, B_OK, want);
        return true;
    }
    if (have->kind == TY_INT && want->kind == TY_FLOAT) {
        wrapImplicit(slot, B_TOFLOAT, &qkTypeFloat);
        return true;
    }
    if ((have->kind == TY_RANGE || have->kind == TY_ITER) && want->kind == TY_ARRAY && qkTypeSame(have->elem, want->elem)) {
        wrapImplicit(slot, B_TOARRAY, want);
        return true;
    }
    if (want->kind == TY_RECORD && have != want && hasOperators(have) && isInterface(want)) return toInterface(c, slot, have, want);
    QkBuf msg = {0};
    qkBufPrintf(&msg, "%s: expected %%s, found %%s", where);
    typeError(c, *slot, msg.data, want, have);
    qkBufFree(&msg);
    return false;
}

/* The source of a loop, seen through implicit wrappers; marks a name used as a loop source. */
static void markIterUse(Expr* e) {
    while (e->kind == E_CALL && e->implicit) e = e->args[0];
    if (e->kind == E_NAME && e->binding) e->binding->iterUses++;
}

static bool isNumeric(const Type* t) { return t->kind == TY_INT || t->kind == TY_FLOAT; }

/* Int with Float: both become Float. Returns the common type or NULL. */
static Type* unifyNumeric(Checker* c, Expr** a, Expr** b) {
    Type* ta = (*a)->type;
    Type* tb = (*b)->type;
    if (ta == tb) return ta;
    if (ta->kind == TY_INT && tb->kind == TY_FLOAT) {
        coerce(c, a, &qkTypeFloat, "");
        return &qkTypeFloat;
    }
    if (ta->kind == TY_FLOAT && tb->kind == TY_INT) {
        coerce(c, b, &qkTypeFloat, "");
        return &qkTypeFloat;
    }
    return NULL;
}

/* ---- functions of a type, and operators --------------------------------------------------------- */

/* A type the program declares (a record, an enum, a host handle) can have functions of its own: fn Vec2.plus.
 * They are called as v.f(...) (or f(v, ...)), and its operators go through them: a + b is a's type's plus. */
static bool hasOperators(const Type* t) {
    if (t->kind == TY_RECORD || t->kind == TY_HANDLE) return true;
    return t->kind == TY_ENUM && t != &qkTypeNone && t != &qkTypeErr && t->decl && !qkFallibleValue(t);
}

static Decl* typeFunction(Checker* c, const Type* t, const char* name) {
    if (!hasOperators(t)) return NULL;
    QkBuf full = {0};
    qkBufPrintf(&full, "%s.%s", t->decl->templateOf ? t->decl->templateOf->name : t->decl->name, name);
    Decl* d = findDecl(c, full.data);
    const Decl* owner = t->decl->templateOf ? t->decl->templateOf : t->decl;
    for (int i = 0; !d && i < c->prog->count; i++) { /* a type's functions go with its values: from its own file too (a generic of std on a program's type) */
        Decl* o = c->prog->decls[i];
        if (o->module == owner->module && o->kind == D_FN && !strcmp(o->name, full.data)) d = o;
    }
    qkBufFree(&full);
    return d && d->kind == D_FN ? d : NULL;
}

/* Some type has a function of this name: a call to it may mean that one. */
static bool anyTypeFunction(Checker* c, const char* name) {
    for (int i = 0; i < c->prog->count; i++) {
        Decl* d = c->prog->decls[i];
        if (d->owner && !strcmp(d->name + strlen(d->owner) + 1, name)) return true;
    }
    return false;
}

static const char* operatorWord(int op) {
    switch (op) {
    case T_PLUS: return "plus";
    case T_MINUS: return "minus";
    case T_STAR: return "times";
    case T_SLASH: return "div";
    case T_PERCENT: return "rem";
    default: return "negate";
    }
}

/* What == compares, field by field: numbers, Bools, texts, cases without data, handles, and records of them. */
static bool equatable(const Type* t) {
    switch (t->kind) {
    case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_TEXT: case TY_HANDLE: case TY_ERROR: return true;
    case TY_ENUM: /* cases with data too, when the data compares (an optional of a number, a result...) */
        if (t == &qkTypeNone || t == &qkTypeErr) return false;
        for (int k = 0; qkHasData(t) && k < t->decl->caseCount; k++)
            for (int f = 0; f < t->decl->caseFieldCount[k]; f++)
                if (t->decl->caseFields[k][f].boxed || !equatable(t->decl->caseFields[k][f].binding->type)) return false;
        return true;
    case TY_RECORD:
        for (int f = 0; f < t->decl->paramCount; f++)
            if (t->decl->params[f].boxed || !equatable(t->decl->params[f].binding->type)) return false;
        return true;
    default: return false;
    }
}

static void needEffect(Checker* c, const Expr* e, const char* what);
static void usesEffectsOf(Checker* c, const Expr* e, Decl* d, int place);
static Type* fnReturnType(Checker* c, Decl* d, const Expr* at);

/* a + b (or -a) on a type of the program: a call to its function (e->decl, e->ordered); the operands are
 * checked already. In text and in graphs it stays an operator. */
static Type* checkOperator(Checker* c, Expr* e, bool scalarFirst) {
    bool unary = e->kind == E_UNARY;
    Expr* first = scalarFirst ? e->b : e->a; /* 2 * v: v's times, given v then 2 */
    Expr* second = scalarFirst ? e->a : e->b;
    Type* t = first->type;
    const char* word = operatorWord(unary ? 0 : e->op);
    const char* symbol = unary ? "-" : qkTokName(e->op);
    Decl* d = typeFunction(c, t, word);
    if (!d) {
        QkBuf n = {0};
        qkTypeName(t, &n);
        if (unary) qkError(c->src, e->line, e->col, "%s has no -: write fn %s.negate(self) -> %s", n.data, n.data, n.data);
        else qkError(c->src, e->line, e->col, "%s has no %s: write fn %s.%s(self, b: ...) -> ...", n.data, symbol, n.data, word);
        qkBufFree(&n);
        return &qkTypeError;
    }
    int want = unary ? 1 : 2;
    if (d->paramCount != want) {
        qkError(c->src, e->line, e->col, "%s takes %d value%s to be the operator %s", d->name, want, want == 1 ? "" : "s", symbol);
        return &qkTypeError;
    }
    usesEffectsOf(c, e, d, 0);
    Expr** ordered = qkNew(sizeof(Expr*) * 2);
    ordered[0] = first;
    if (!unary) {
        ordered[1] = second;
        if (ordered[1]->type->kind == TY_RANGE || ordered[1]->type->kind == TY_ITER) markIterUse(ordered[1]);
        if (!coerce(c, &ordered[1], d->params[1].binding->type, d->name)) return &qkTypeError;
    }
    e->decl = d;
    e->ordered = ordered;
    e->orderedCount = want;
    return fnReturnType(c, d, e);
}

/* ---- effects ------------------------------------------------------------------------------------- */

/* A call that changes the world (print, now, the host's effect fns, or a function that makes one) makes the
 * function it is in change the world too: effects are inferred, never written on a function with a body. A
 * lambda given to map, filter... and a constant never have any, so they can be fused, reordered, cached, and
 * run on several threads. Where nothing can be inferred (extern effect fn, effect (A) -> R) it is written. */
enum { EFFECT_IN_BODY, EFFECT_IN_LAMBDA, EFFECT_IN_CONST, EFFECT_IN_KEPT, EFFECT_AS_VALUE };

static int effectPlace(const Checker* c) {
    if (c->lambdaDepth) return EFFECT_IN_LAMBDA;
    if (c->inConst || !c->fn) return EFFECT_IN_CONST;
    if (c->fn->lifted && !c->fn->isEffect && !c->fn->inferEffect) return EFFECT_IN_KEPT;
    return EFFECT_IN_BODY;
}

static void effectError(const QkSource* src, const Expr* e, int place, const char* what) {
    switch (place) {
    case EFFECT_IN_LAMBDA: qkError(src, e->line, e->col, "%s has effects: a lambda cannot have any (map, filter... are pure)", what); break;
    case EFFECT_IN_CONST: qkError(src, e->line, e->col, "%s has effects: a constant cannot have any", what); break;
    case EFFECT_IN_KEPT:
        qkError(src, e->line, e->col, "%s has effects: this lambda's type says it has none (write effect (A) -> R)", what);
        break;
    default:
        qkError(src, e->line, e->col, "%s has effects, found only after its type was given as pure (a function that calls itself through it)", what);
    }
}

static void needEffect(Checker* c, const Expr* e, const char* what) {
    int place = effectPlace(c);
    if (place == EFFECT_IN_BODY) c->fn->isEffect = true;
    else effectError(c->src, e, place, what);
}

/* Uses of functions not known (yet) to change the world: one of them may still turn out to, through a function
 * that calls itself. Settled once everything is checked. */
typedef struct EffectUse {
    Decl* user;
    Decl* used;
    const Expr* at;
    const QkSource* src;
    int place;
} EffectUse;
static EffectUse* effectUses;
static int effectUseCount, effectUseCap;

/* d's body, checked now (in a checker of its own) if it was not yet: what it gives, and whether it has effects */
static void checkFnBody(Checker* c, Decl* d);
static void checkNow(Checker* c, Decl* d) {
    if (d->isExtern || d->state != 0 || d->isTemplate || d->lifted) return;
    Checker inner = *c;
    inner.scope = NULL;
    inner.scopeCount = inner.scopeCap = 0;
    inner.lets = NULL;
    inner.letCount = inner.letCap = 0;
    inner.inConst = false;
    inner.hint = NULL;
    inner.outer = NULL;
    checkFnBody(&inner, d);
}

static bool effectOf(Checker* c, Decl* d) {
    checkNow(c, d);
    return d->isEffect;
}

/* e uses d (calls it, through an operator too, or takes it as a value): d's effects are the user's */
static void usesEffectsOf(Checker* c, const Expr* e, Decl* d, int place) {
    if (effectOf(c, d)) {
        if (place == EFFECT_AS_VALUE) return; /* its type says so */
        needEffect(c, e, d->name);
        return;
    }
    if (d->isExtern) return;
    EffectUse u = {c->fn, d, e, c->src, place == EFFECT_AS_VALUE ? place : effectPlace(c)};
    QK_PUSH(effectUses, effectUseCount, effectUseCap, u);
}

static void settleEffects(void) {
    for (bool changed = true; changed;) {
        changed = false;
        for (int i = 0; i < effectUseCount; i++) {
            EffectUse* u = &effectUses[i];
            if (u->place == EFFECT_IN_BODY && u->used->isEffect && u->user && !u->user->isEffect && !u->user->lifted) {
                u->user->isEffect = true;
                changed = true;
            }
        }
    }
    for (int i = 0; i < effectUseCount; i++) {
        EffectUse* u = &effectUses[i];
        if (!u->used->isEffect) continue;
        if (u->place != EFFECT_IN_BODY) effectError(u->src, u->at, u->place, u->used->name);
        else if (u->user && u->user->lifted && !u->user->isEffect) effectError(u->src, u->at, EFFECT_AS_VALUE, u->used->name);
    }
    effectUseCount = 0;
}

/* ---- lambdas ------------------------------------------------------------------------------------- */

static int builtinNamed(const char* name);

/* Checks a function argument: a lambda, or a function's name (made into one). Returns its result type. */
static Type* checkLambda(Checker* c, Expr** slot, Type** paramTypes, int maxParams, int minParams, const char* builtin) {
    Expr* e = *slot;
    Binding* fb = e->kind == E_NAME ? lookupOrCapture(c, e->name) : NULL;
    bool kept = fb && fb->type->kind == TY_FN && !fb->lambda && !fb->fnParam; /* a function kept as a value */
    if (fb && fb->type->kind == TY_FN && !kept) { /* a function this one was given: the caller's lambda */
        Type* ft = fb->type;
        if (ft->paramCount < minParams || ft->paramCount > maxParams) {
            qkError(c->src, e->line, e->col, "%s needs a function of %d value%s here; %s takes %d", builtin, minParams,
                    minParams == 1 ? "" : "s", e->name, ft->paramCount);
            return &qkTypeError;
        }
        for (int i = 0; i < ft->paramCount; i++)
            if (!qkTypeSame(ft->params[i], paramTypes[i]))
                typeError(c, e, "this function takes %s, but %s is given here", ft->params[i], paramTypes[i]);
        e->binding = fb;
        fb->uses++;
        if (!fb->lambda) return ft->elem; /* a template, checked loosely */
        *slot = fb->lambda;
        return ft->elem;
    }
    if (e->kind == E_NAME) {
        Decl* d = lookup(c, e->name) ? NULL : findDecl(c, e->name);
        /* a function's name, or a built-in's (len, abs, upper...: as many values as are given here) */
        int arity = kept ? fb->type->paramCount
                    : d && d->kind == D_FN ? d->paramCount : !d && !lookup(c, e->name) && builtinNamed(e->name) ? minParams : -1;
        if (arity >= 0) {
            if (arity < minParams || arity > maxParams) {
                qkError(c->src, e->line, e->col, "%s needs a function of %d value%s here; %s takes %d", builtin, minParams,
                        minParams == 1 ? "" : "s", e->name, arity);
                return &qkTypeError;
            }
            Expr* lam = qkNewExpr(E_LAMBDA, e->line, e->col);
            lam->paramCount = arity;
            lam->params = qkNew(sizeof(Param) * (size_t)(arity ? arity : 1));
            Expr* call = qkNewExpr(E_CALL, e->line, e->col);
            call->name = e->name;
            for (int i = 0; i < arity; i++) {
                char* pn = qkNew(8);
                snprintf(pn, 8, "#%d", i);
                lam->params[i] = (Param){pn, NULL, e->line, e->col, NULL};
                Expr* ref = qkNewExpr(E_NAME, e->line, e->col);
                ref->name = pn;
                if (call->argCount == call->argCap) {
                    call->argCap = arity;
                    call->args = qkNew(sizeof(Expr*) * (size_t)arity);
                    call->argNames = qkNew(sizeof(char*) * (size_t)arity);
                }
                call->args[call->argCount++] = ref;
            }
            lam->body = call;
            lam->sugar = e;
            *slot = e = lam;
        }
    }
    if (e->kind != E_LAMBDA) {
        qkError(c->src, e->line, e->col, "%s needs a function here: a lambda (x => ...) or a function's name", builtin);
        checkExpr(c, e, true);
        return &qkTypeError;
    }
    if (e->paramCount < minParams || e->paramCount > maxParams) {
        if (minParams == maxParams)
            qkError(c->src, e->line, e->col, "this lambda takes %d value%s in %s", minParams, minParams == 1 ? "" : "s", builtin);
        else
            qkError(c->src, e->line, e->col, "this lambda takes %d or %d values in %s", minParams, maxParams, builtin);
        return &qkTypeError;
    }
    int mark = c->scopeCount;
    c->loopDepth++;
    c->lambdaDepth++;
    for (int i = 0; i < e->paramCount; i++) {
        Binding* b = qkNew(sizeof *b);
        b->name = e->params[i].name;
        b->type = paramTypes[i];
        b->isParam = true;
        b->line = e->params[i].line;
        b->col = e->params[i].col;
        b->depth = c->loopDepth;
        e->params[i].binding = b;
        QK_PUSH(c->scope, c->scopeCount, c->scopeCap, b);
    }
    Type* t = checkExpr(c, e->body, true);
    c->scopeCount = mark;
    c->loopDepth--;
    c->lambdaDepth--;
    e->type = t;
    return t;
}

/* ---- calls --------------------------------------------------------------------------------------- */

static int builtinNamed(const char* name) {
    for (int i = 1; i < B_COUNT; i++)
        if (!strcmp(qkBuiltinNames[i], name)) return i;
    return 0;
}

static bool argCount(Checker* c, Expr* e, int lo, int hi) {
    if (e->argCount >= lo && e->argCount <= hi) return true; /* the names were checked by namedBuiltinArgs */
    if (lo == hi) qkError(c->src, e->line, e->col, "%s takes %d value%s, not %d", e->name, lo, lo == 1 ? "" : "s", e->argCount);
    else qkError(c->src, e->line, e->col, "%s takes %d to %d values, not %d", e->name, lo, hi, e->argCount);
    return false;
}

static Type* checkSource(Checker* c, Expr* e, int i) {
    Type* t = checkExpr(c, e->args[i], true);
    if (t->kind == TY_MAP && t->key) { /* a map: its entries, Entry(key, value), in the order the keys came */
        wrapImplicit(&e->args[i], B_ENTRIES, qkTypeIter(typeEntry(t->key, t->elem)));
        return e->args[i]->type->elem;
    }
    if (!qkIsIterable(t)) {
        typeError(c, e->args[i], "expected an array, a map, a range or a loop, found %s", t, NULL);
        return &qkTypeError;
    }
    markIterUse(e->args[i]);
    return t->elem;
}

/* what toText and "{x}" take */
static bool textable(const Type* t) {
    return t->kind == TY_INT || t->kind == TY_FLOAT || t->kind == TY_BOOL || t->kind == TY_TEXT || t->kind == TY_ERROR ||
           (t->kind == TY_ENUM && t != &qkTypeNone && t != &qkTypeErr && !qkFallibleValue(t)); /* a case: its name */
}

static bool printable(const Type* t) {
    switch (t->kind) {
    case TY_ARRAY: return printable(t->elem);
    case TY_MAP: return t->key && printable(t->key) && printable(t->elem);
    case TY_RANGE: case TY_ITER: case TY_VOID: case TY_ERROR: case TY_FN: return false;
    default: return true;
    }
}

/* What sort and < order: numbers, texts, Bools, cases without data, and types with fn T.compare(a, b) -> Int. */
static Decl* typeFunction(Checker* c, const Type* t, const char* name);
static bool orderable(Checker* c, const Type* t) {
    switch (t->kind) {
    case TY_INT: case TY_FLOAT: case TY_TEXT: case TY_BOOL: case TY_ERROR: return true;
    case TY_ENUM: case TY_RECORD: case TY_HANDLE: {
        if (t->kind == TY_ENUM && t->decl && !qkHasData(t) && !qkFallibleValue(t) && !typeFunction(c, t, "compare")) return true;
        Decl* d = typeFunction(c, t, "compare");
        return d && d->paramCount == 2;
    }
    default: return false;
    }
}

/* split(t, separator: ","): a built-in's values may be named, in their order (a name says what the value is) */
static bool namedBuiltinArgs(Checker* c, Expr* e) {
    for (int i = 0; i < e->argCount; i++) {
        if (!e->argNames || !e->argNames[i]) continue;
        QkBuf want = {0};
        if (!qkBuiltinParam(e->name, i, e->argNames[i], &want)) {
            if (want.len) qkError(c->src, e->args[i]->line, e->args[i]->col, "%s's value here is %s, not \"%s\" (named values go in their order)", e->name, want.data, e->argNames[i]);
            else qkError(c->src, e->args[i]->line, e->args[i]->col, "%s has no value \"%s\"", e->name, e->argNames[i]);
            qkBufFree(&want);
            return false;
        }
        qkBufFree(&want);
    }
    return true;
}

static Type* checkBuiltin(Checker* c, Expr* e, int b) {
    e->builtin = b;
    if (!e->implicit && !namedBuiltinArgs(c, e)) return &qkTypeError;
    if ((b == B_SORT || b == B_SORTBY) && e->argCount >= 2 && e->argNames[e->argCount - 1] &&
        !strcmp(e->argNames[e->argCount - 1], "descending")) { /* xs.sortBy(f, descending: true) */
        e->argCount--;
        Type* r = checkBuiltin(c, e, b);
        e->argCount++;
        checkExpr(c, e->args[e->argCount - 1], true);
        coerce(c, &e->args[e->argCount - 1], &qkTypeBool, "descending");
        return r;
    }
    switch (b) {
    case B_TAKE: case B_SKIP: { /* the first n, or all but them: fused into the loop; past the end is the end */
        if (!argCount(c, e, 2, 2)) return &qkTypeError;
        Type* elem = checkSource(c, e, 0);
        checkExpr(c, e->args[1], true);
        coerce(c, &e->args[1], &qkTypeInt, e->name);
        return elem->kind == TY_ERROR ? elem : qkTypeIter(elem);
    }
    case B_MEAN: {
        if (!argCount(c, e, 1, 1)) return &qkTypeError;
        Type* t = checkSource(c, e, 0);
        if (t->kind != TY_ERROR && !isNumeric(t)) typeError(c, e->args[0], "mean: expected numbers, found a list of %s", t, NULL);
        return &qkTypeFloat;
    }
    case B_COUNTBY: { /* how many values give each key: [key: count], in the order the keys came */
        if (!argCount(c, e, 2, 2)) return &qkTypeError;
        Type* elem = checkSource(c, e, 0);
        Type* params[1] = {elem};
        Type* r = checkLambda(c, &e->args[1], params, 1, 1, "countBy");
        if (r->kind == TY_ERROR || elem->kind == TY_ERROR) return &qkTypeError;
        if (!keyable(r)) {
            typeError(c, e->args[1], "countBy: the lambda gives %s, which cannot be a map's key", r, NULL);
            return &qkTypeError;
        }
        return qkTypeMap(r, &qkTypeInt);
    }
    case B_GET: { /* xs.get(i): the value at i, or None (no panic) */
        if (!argCount(c, e, 2, 2)) return &qkTypeError;
        Type* elem = checkSource(c, e, 0);
        if (elem->kind == TY_ERROR) return elem;
        coerce(c, &e->args[0], qkTypeArray(elem), "get");
        checkExpr(c, e->args[1], true);
        coerce(c, &e->args[1], &qkTypeInt, "get's index");
        return qkTypeOption(elem);
    }
    case B_PANIC: /* stops the program (or the host's call) with this message: it never gives a value */
        if (!argCount(c, e, 1, 1)) return &qkTypeError;
        checkExpr(c, e->args[0], true);
        coerce(c, &e->args[0], &qkTypeText, "panic's message");
        return &qkTypeNever;
    case B_SQRT: case B_SIN: case B_COS: case B_TAN: case B_ASIN: case B_ACOS: case B_ATAN: case B_EXP: case B_LOG:
        if (!argCount(c, e, 1, 1)) return &qkTypeError;
        checkExpr(c, e->args[0], true);
        coerce(c, &e->args[0], &qkTypeFloat, e->name);
        return &qkTypeFloat;
    case B_ATAN2: case B_POW:
        if (!argCount(c, e, 2, 2)) return &qkTypeError;
        for (int i = 0; i < 2; i++) {
            checkExpr(c, e->args[i], true);
            coerce(c, &e->args[i], &qkTypeFloat, e->name);
        }
        return &qkTypeFloat;
    case B_ABS: {
        if (!argCount(c, e, 1, 1)) return &qkTypeError;
        Type* t = checkExpr(c, e->args[0], true);
        if (!isNumeric(t)) typeError(c, e->args[0], "abs: expected Int or Float, found %s", t, NULL);
        return t;
    }
    case B_FLOOR: case B_CEIL: case B_ROUND: {
        if (!argCount(c, e, 1, 1)) return &qkTypeError;
        Type* t = checkExpr(c, e->args[0], true);
        if (t->kind == TY_INT) return &qkTypeInt;
        coerce(c, &e->args[0], &qkTypeFloat, e->name);
        return &qkTypeInt;
    }
    case B_TOFLOAT:
        if (!argCount(c, e, 1, 1)) return &qkTypeError;
        checkExpr(c, e->args[0], true);
        if (e->args[0]->type->kind == TY_FLOAT) return &qkTypeFloat;
        coerce(c, &e->args[0], &qkTypeInt, "toFloat");
        return &qkTypeFloat;
    case B_TOINT:
        if (!argCount(c, e, 1, 1)) return &qkTypeError;
        checkExpr(c, e->args[0], true);
        if (e->args[0]->type->kind == TY_INT) return &qkTypeInt;
        coerce(c, &e->args[0], &qkTypeFloat, "toInt");
        return &qkTypeInt;
    case B_MIN: case B_MAX: {
        if (!argCount(c, e, 1, 2)) return &qkTypeError;
        if (e->argCount == 1) {
            Type* t = checkSource(c, e, 0);
            if (t->kind != TY_ERROR && !isNumeric(t)) typeError(c, e->args[0], "%s of a list: expected numbers, found %s", NULL, t);
            return t;
        }
        checkExpr(c, e->args[0], true);
        checkExpr(c, e->args[1], true);
        Type* t = unifyNumeric(c, &e->args[0], &e->args[1]);
        if (!t || !isNumeric(t)) {
            typeError(c, e, "min / max: expected two numbers, found %s and %s", e->args[0]->type, e->args[1]->type);
            return &qkTypeError;
        }
        return t;
    }
    case B_CLAMP: {
        if (!argCount(c, e, 3, 3)) return &qkTypeError;
        for (int i = 0; i < 3; i++) checkExpr(c, e->args[i], true);
        bool anyFloat = false;
        for (int i = 0; i < 3; i++) {
            if (!isNumeric(e->args[i]->type)) {
                typeError(c, e->args[i], "clamp: expected a number, found %s", e->args[i]->type, NULL);
                return &qkTypeError;
            }
            anyFloat |= e->args[i]->type->kind == TY_FLOAT;
        }
        if (!anyFloat) return &qkTypeInt;
        for (int i = 0; i < 3; i++) coerce(c, &e->args[i], &qkTypeFloat, "clamp");
        return &qkTypeFloat;
    }
    case B_LERP:
        if (!argCount(c, e, 3, 3)) return &qkTypeError;
        for (int i = 0; i < 3; i++) {
            checkExpr(c, e->args[i], true);
            coerce(c, &e->args[i], &qkTypeFloat, "lerp");
        }
        return &qkTypeFloat;
    case B_LEN: {
        if (!argCount(c, e, 1, 1)) return &qkTypeError;
        Type* t = checkExpr(c, e->args[0], true);
        if (t->kind == TY_TEXT || t->kind == TY_MAP) return &qkTypeInt; /* bytes; keys */
        if (!qkIsIterable(t)) {
            if (t->kind != TY_ERROR) typeError(c, e->args[0], "len: expected a text, an array, a range or a loop, found %s", t, NULL);
            return &qkTypeInt;
        }
        markIterUse(e->args[0]);
        return &qkTypeInt;
    }
    case B_FILL: {
        if (!argCount(c, e, 2, 2)) return &qkTypeError;
        checkExpr(c, e->args[0], true);
        coerce(c, &e->args[0], &qkTypeInt, "fill's count");
        Type* t = checkExpr(c, e->args[1], true);
        if (t->kind == TY_VOID || t->kind == TY_RANGE || t->kind == TY_ITER) {
            typeError(c, e->args[1], "fill: %s cannot be stored", t, NULL);
            return &qkTypeError;
        }
        return qkTypeArray(t);
    }
    case B_MAP: case B_FILTER: case B_ANY: case B_ALL: {
        if (!argCount(c, e, 2, 2)) return &qkTypeError;
        Type* elem = checkSource(c, e, 0);
        Type* params[2] = {elem, &qkTypeInt};
        bool indexed = b == B_MAP || b == B_FILTER;
        Type* r = checkLambda(c, &e->args[1], params, indexed ? 2 : 1, 1, e->name);
        if (b == B_MAP) {
            if (r->kind == TY_VOID || r->kind == TY_RANGE || r->kind == TY_ITER) {
                typeError(c, e->args[1], "map: each value becomes %s, which cannot be stored", r, NULL);
                return &qkTypeError;
            }
            return qkTypeIter(r);
        }
        if (r->kind != TY_BOOL && r->kind != TY_ERROR) typeError(c, e->args[1], "%s: the lambda must give a Bool, not %s", r, NULL);
        return b == B_FILTER ? qkTypeIter(elem) : &qkTypeBool;
    }
    case B_FOLD: {
        if (!argCount(c, e, 3, 3)) return &qkTypeError;
        Type* elem = checkSource(c, e, 0);
        Type* acc = checkExpr(c, e->args[1], true);
        Type* params[2] = {acc, elem};
        Type* r = checkLambda(c, &e->args[2], params, 2, 2, "fold");
        if (r->kind != TY_ERROR && !qkTypeSame(r, acc)) {
            if (r->kind == TY_INT && acc->kind == TY_FLOAT) return acc; /* converted where it is lowered */
            typeError(c, e->args[2], "fold: the lambda gives %s, but the total is %s", r, acc);
        }
        return acc;
    }
    case B_SUM: {
        if (!argCount(c, e, 1, 1)) return &qkTypeError;
        Type* t = checkSource(c, e, 0);
        if (t->kind != TY_ERROR && !isNumeric(t)) typeError(c, e->args[0], "sum: expected numbers, found a list of %s (fold adds anything)", t, NULL);
        return t;
    }
    case B_TOARRAY: {
        if (!argCount(c, e, 1, 1)) return &qkTypeError;
        return qkTypeArray(checkSource(c, e, 0));
    }
    case B_PRINT:
        needEffect(c, e, "print");
        for (int i = 0; i < e->argCount; i++) {
            Type* t = checkExpr(c, e->args[i], true);
            if (t->kind == TY_RANGE || t->kind == TY_ITER) {
                markIterUse(e->args[i]);
                coerce(c, &e->args[i], qkTypeArray(t->elem), "print");
            } else if (!printable(t)) {
                typeError(c, e->args[i], "print cannot show %s", t, NULL);
            }
        }
        return &qkTypeVoid;
    case B_NOW:
        needEffect(c, e, "now");
        argCount(c, e, 0, 0);
        return &qkTypeFloat;
    case B_SPAWN: { /* spawn(f): a task that runs f a step at a time; its number */
        needEffect(c, e, "spawn");
        if (!argCount(c, e, 1, 1)) return &qkTypeError;
        Type proto = {TY_FN};
        proto.effect = true;
        Type* ft;
        if (e->args[0]->kind == E_LAMBDA) {
            proto.elem = NULL;
            if (!liftLambda(c, e->args[0], &proto)) return &qkTypeError;
            ft = e->args[0]->type;
        } else {
            ft = checkExpr(c, e->args[0], true);
        }
        if (ft->kind == TY_ERROR) return ft;
        if (ft->kind != TY_FN || ft->paramCount) {
            typeError(c, e->args[0], "spawn runs a function of no values, () => ..., not %s", ft, NULL);
            return &qkTypeError;
        }
        return &qkTypeInt;
    }
    case B_STEP: /* step(task): runs it until it gives its turn back; false once it is done */
        needEffect(c, e, "step");
        if (!argCount(c, e, 1, 1)) return &qkTypeError;
        checkExpr(c, e->args[0], true);
        coerce(c, &e->args[0], &qkTypeInt, "step");
        return &qkTypeBool;
    case B_YIELD: /* the turn back to whoever stepped the task (nothing, outside a task) */
        needEffect(c, e, "yield");
        argCount(c, e, 0, 0);
        return &qkTypeVoid;
    case B_WAIT: /* gives the turn back until so many seconds went by (in a task); sleeps, outside one */
        needEffect(c, e, "wait");
        if (!argCount(c, e, 1, 1)) return &qkTypeError;
        checkExpr(c, e->args[0], true);
        coerce(c, &e->args[0], &qkTypeFloat, "wait");
        return &qkTypeVoid;
    case B_RANDOM: /* a Float in [0, 1): the program's own generator (seeded from the clock, or by seedRandom) */
        needEffect(c, e, "random");
        argCount(c, e, 0, 0);
        return &qkTypeFloat;
    case B_RANDOMINT: /* an Int in [lo, hi) */
    case B_SEEDRANDOM: {
        needEffect(c, e, e->name);
        int n = b == B_RANDOMINT ? 2 : 1;
        if (!argCount(c, e, n, n)) return &qkTypeError;
        for (int i = 0; i < n; i++) {
            checkExpr(c, e->args[i], true);
            coerce(c, &e->args[i], &qkTypeInt, e->name);
        }
        return b == B_RANDOMINT ? &qkTypeInt : &qkTypeVoid;
    }
    case B_READFILE: case B_WRITEFILE: case B_READLINE: case B_ARGS: { /* the outside world */
        static const int counts[] = {1, 2, 0, 0};
        int n = counts[b - B_READFILE];
        needEffect(c, e, e->name);
        if (!argCount(c, e, n, n)) return &qkTypeError;
        for (int i = 0; i < n; i++) {
            checkExpr(c, e->args[i], true);
            coerce(c, &e->args[i], &qkTypeText, e->name);
        }
        if (b == B_WRITEFILE) return &qkTypeBool;
        if (b == B_ARGS) return qkTypeArray(&qkTypeText);
        if (b == B_READFILE) return qkTypeResult(&qkTypeText); /* Err: why it could not be read */
        return qkTypeOption(&qkTypeText); /* None: the end of the input */
    }
    case B_SPLIT: case B_TRIM: case B_PARSEINT: case B_PARSEFLOAT: { /* text, pure */
        int n = b == B_SPLIT ? 2 : 1;
        if (!argCount(c, e, n, n)) return &qkTypeError;
        for (int i = 0; i < n; i++) {
            checkExpr(c, e->args[i], true);
            coerce(c, &e->args[i], &qkTypeText, e->name);
        }
        if (b == B_SPLIT) return qkTypeArray(&qkTypeText);
        if (b == B_TRIM) return &qkTypeText;
        return qkTypeOption(b == B_PARSEINT ? &qkTypeInt : &qkTypeFloat); /* None: not a number */
    }
    case B_SOME: {
        if (!argCount(c, e, 1, 1)) return &qkTypeError;
        Type* t = checkExpr(c, e->args[0], true);
        if (t->kind == TY_RANGE || t->kind == TY_ITER) {
            markIterUse(e->args[0]);
            coerce(c, &e->args[0], qkTypeArray(t->elem), "Some");
            t = e->args[0]->type;
        }
        if (t->kind == TY_VOID || t == &qkTypeNone || t == &qkTypeEmpty) {
            typeError(c, e->args[0], "Some needs a value of a known type, not %s", t, NULL);
            return &qkTypeError;
        }
        if (t->kind == TY_ERROR) return t;
        if (t->kind == TY_ENUM && t->decl && t->decl->optionOf) {
            typeError(c, e->args[0], "Some of %s: it is optional already", t, NULL);
            return &qkTypeError;
        }
        return qkTypeOption(t);
    }
    case B_TOTEXT: {
        if (!argCount(c, e, 1, 1)) return &qkTypeError;
        Type* t = checkExpr(c, e->args[0], true);
        if (!textable(t)) typeError(c, e->args[0], "toText: %s has no text form (Int, Float, Bool, Text and enum cases do)", t, NULL);
        return &qkTypeText;
    }
    case B_OK: {
        if (!argCount(c, e, 1, 1)) return &qkTypeError;
        Type* t = checkExpr(c, e->args[0], true);
        if (t->kind == TY_RANGE || t->kind == TY_ITER) {
            markIterUse(e->args[0]);
            coerce(c, &e->args[0], qkTypeArray(t->elem), "Ok");
            t = e->args[0]->type;
        }
        if (t->kind == TY_ERROR) return t;
        if (t->kind == TY_VOID || t == &qkTypeNone || t == &qkTypeEmpty || t == &qkTypeErr || t == &qkTypeEmptyMap || qkFallibleValue(t)) {
            typeError(c, e->args[0], "Ok needs a plain value of a known type, not %s", t, NULL);
            return &qkTypeError;
        }
        return qkTypeResult(t);
    }
    case B_ERR: { /* Err("why"), or Err(ParseError.Empty) in a T!ParseError */
        if (!argCount(c, e, 1, 1)) return &qkTypeError;
        Type* t = checkExpr(c, e->args[0], true);
        if (t->kind == TY_RANGE || t->kind == TY_ITER) {
            markIterUse(e->args[0]);
            coerce(c, &e->args[0], qkTypeArray(t->elem), "Err");
        }
        return &qkTypeErr; /* takes the T! it goes into: its error type is checked there */
    }
    case B_WORDS: case B_LINES: case B_CHARS: case B_UPPER: case B_LOWER:
        if (!argCount(c, e, 1, 1)) return &qkTypeError;
        checkExpr(c, e->args[0], true);
        coerce(c, &e->args[0], &qkTypeText, e->name);
        return b == B_UPPER || b == B_LOWER ? &qkTypeText : qkTypeArray(&qkTypeText);
    case B_STARTSWITH: case B_ENDSWITH: case B_REPLACE: {
        int n = b == B_REPLACE ? 3 : 2;
        if (!argCount(c, e, n, n)) return &qkTypeError;
        for (int i = 0; i < n; i++) {
            checkExpr(c, e->args[i], true);
            coerce(c, &e->args[i], &qkTypeText, e->name);
        }
        return b == B_REPLACE ? &qkTypeText : &qkTypeBool;
    }
    case B_REPEAT:
        if (!argCount(c, e, 2, 2)) return &qkTypeError;
        checkExpr(c, e->args[0], true);
        coerce(c, &e->args[0], &qkTypeText, "repeat");
        checkExpr(c, e->args[1], true);
        coerce(c, &e->args[1], &qkTypeInt, "repeat's count");
        return &qkTypeText;
    case B_JOIN: {
        if (!argCount(c, e, 2, 2)) return &qkTypeError;
        Type* t = checkSource(c, e, 0);
        if (t->kind != TY_TEXT && t->kind != TY_ERROR) typeError(c, e->args[0], "join puts texts together, not %s (map them to text first)", t, NULL);
        checkExpr(c, e->args[1], true);
        coerce(c, &e->args[1], &qkTypeText, "join's separator");
        return &qkTypeText;
    }
    case B_CONTAINS: case B_INDEXOF: {
        if (!argCount(c, e, 2, 2)) return &qkTypeError;
        Type* t = checkExpr(c, e->args[0], true);
        Type* found = b == B_CONTAINS ? &qkTypeBool : qkTypeOption(&qkTypeInt);
        if (t->kind == TY_TEXT) {
            checkExpr(c, e->args[1], true);
            coerce(c, &e->args[1], &qkTypeText, e->name);
            return found;
        }
        if (t->kind == TY_MAP && b == B_CONTAINS) {
            checkExpr(c, e->args[1], true);
            coerce(c, &e->args[1], t->key, "the key");
            return found;
        }
        if (!qkIsIterable(t)) {
            if (t->kind != TY_ERROR) typeError(c, e->args[0], "this looks in a text, an array or a map, not in %s", t, NULL);
            return &qkTypeError;
        }
        markIterUse(e->args[0]);
        if (!equatable(t->elem)) typeError(c, e->args[0], "the values of %s cannot be compared with ==", t, NULL);
        checkExpr(c, e->args[1], true);
        coerce(c, &e->args[1], t->elem, e->name);
        return found;
    }
    case B_SORT: case B_REVERSE: {
        if (!argCount(c, e, 1, 1)) return &qkTypeError;
        Type* t = checkSource(c, e, 0);
        if (t->kind == TY_ERROR) return t;
        if (b == B_SORT && !orderable(c, t))
            typeError(c, e->args[0], "sort orders numbers, texts and cases; for %s write fn T.compare(a: T, b: T) -> Int, or use sortBy", t, NULL);
        coerce(c, &e->args[0], qkTypeArray(t), e->name); /* it works on an array */
        return qkTypeArray(t);
    }
    case B_SORTBY: case B_MINBY: case B_MAXBY: case B_COUNTOF: case B_FIRST: case B_LAST: {
        bool optionalFn = b == B_FIRST || b == B_LAST;
        if (b == B_COUNTOF && e->argCount == 1) { /* count(): how many, as len() */
            checkSource(c, e, 0);
            e->builtin = B_LEN;
            return &qkTypeInt;
        }
        if (!argCount(c, e, optionalFn ? 1 : 2, 2)) return &qkTypeError;
        Type* elem = checkSource(c, e, 0);
        if (e->argCount == 2) {
            Type* params[1] = {elem};
            Type* r = checkLambda(c, &e->args[1], params, 1, 1, e->name);
            if (r->kind != TY_ERROR) {
                if ((b == B_COUNTOF || optionalFn) && r->kind != TY_BOOL)
                    typeError(c, e->args[1], "the lambda must give a Bool, not %s", r, NULL);
                if ((b == B_SORTBY || b == B_MINBY || b == B_MAXBY) && !orderable(c, r))
                    typeError(c, e->args[1], "the lambda gives %s, which has no order (numbers, texts and cases do)", r, NULL);
            }
        }
        if (elem->kind == TY_ERROR) return elem;
        if (b == B_SORTBY) {
            coerce(c, &e->args[0], qkTypeArray(elem), e->name);
            return qkTypeArray(elem);
        }
        if (b == B_LAST && e->argCount == 1) coerce(c, &e->args[0], qkTypeArray(elem), e->name);
        if (b == B_COUNTOF) return &qkTypeInt;
        return optionalFn ? qkTypeOption(elem) : elem;
    }
    case B_KEYS: case B_VALUES: {
        if (!argCount(c, e, 1, 1)) return &qkTypeError;
        Type* t = checkExpr(c, e->args[0], true);
        if (t->kind != TY_MAP) {
            if (t->kind != TY_ERROR) typeError(c, e->args[0], "expected a map, found %s", t, NULL);
            return &qkTypeError;
        }
        return qkTypeArray(b == B_KEYS ? t->key : t->elem);
    }
    case B_REMOVE: { /* m.remove(key): m a var map, one key fewer */
        if (!argCount(c, e, 2, 2)) return &qkTypeError;
        Type* t = checkExpr(c, e->args[0], true);
        Expr* target = e->args[0];
        if (target->kind != E_NAME || !target->binding || !target->binding->mutable) {
            qkError(c->src, target->line, target->col, "remove takes a key out of a var map: var m: [Text: Int] = [:] ... m.remove(\"a\")");
            return &qkTypeError;
        }
        if (t->kind != TY_MAP) {
            if (t->kind != TY_ERROR) typeError(c, target, "remove takes a key out of a map, not %s", t, NULL);
            return &qkTypeError;
        }
        checkExpr(c, e->args[1], true);
        coerce(c, &e->args[1], t->key, "the key");
        return &qkTypeVoid;
    }
    case B_INDEX: {
        if (!argCount(c, e, 1, 1)) return &qkTypeError;
        Type* t = checkExpr(c, e->args[0], true);
        if (t->kind != TY_ENUM || t == &qkTypeNone || t == &qkTypeErr) {
            if (t->kind != TY_ERROR) typeError(c, e->args[0], "index gives an enum case's place (0, 1...), not %s's", t, NULL);
            return &qkTypeError;
        }
        return &qkTypeInt;
    }
    case B_PRINTERR:
        needEffect(c, e, "printErr");
        for (int i = 0; i < e->argCount; i++) {
            Type* t = checkExpr(c, e->args[i], true);
            if (t->kind == TY_RANGE || t->kind == TY_ITER) {
                markIterUse(e->args[i]);
                coerce(c, &e->args[i], qkTypeArray(t->elem), "printErr");
            } else if (!printable(t)) {
                typeError(c, e->args[i], "printErr cannot show %s", t, NULL);
            }
        }
        return &qkTypeVoid;
    case B_EXIT:
        needEffect(c, e, "exit");
        if (!argCount(c, e, 1, 1)) return &qkTypeError;
        checkExpr(c, e->args[0], true);
        coerce(c, &e->args[0], &qkTypeInt, "exit's code");
        return &qkTypeNever;
    case B_ENV: case B_FILEEXISTS: case B_LISTDIR:
        needEffect(c, e, e->name);
        if (!argCount(c, e, 1, 1)) return &qkTypeError;
        checkExpr(c, e->args[0], true);
        coerce(c, &e->args[0], &qkTypeText, e->name);
        if (b == B_ENV) return qkTypeOption(&qkTypeText);
        if (b == B_FILEEXISTS) return &qkTypeBool;
        return qkTypeResult(qkTypeArray(&qkTypeText));
    case B_TIME:
        needEffect(c, e, "time");
        argCount(c, e, 0, 0);
        return &qkTypeFloat;
    case B_ASSERT: /* assert(condition) or assert(condition, "why"): stops the program (or the test) when false */
        if (!argCount(c, e, 1, 2)) return &qkTypeError;
        checkExpr(c, e->args[0], true);
        coerce(c, &e->args[0], &qkTypeBool, "assert");
        if (e->argCount == 2) {
            checkExpr(c, e->args[1], true);
            coerce(c, &e->args[1], &qkTypeText, "assert's message");
        }
        return &qkTypeVoid;
    case B_PUSH: { /* xs.push(value): xs a var array, one longer */
        if (!argCount(c, e, 2, 2)) return &qkTypeError;
        Type* t = checkExpr(c, e->args[0], true);
        Expr* target = e->args[0];
        if (target->kind != E_NAME || !target->binding || !target->binding->mutable) {
            qkError(c->src, target->line, target->col, "push adds to a var array: var xs: [Int] = [] ... xs.push(1)");
            return &qkTypeError;
        }
        if (t->kind != TY_ARRAY) {
            if (t->kind != TY_ERROR) typeError(c, target, "push adds to an array, not %s", t, NULL);
            return &qkTypeError;
        }
        Type* v = checkExpr(c, e->args[1], true);
        if (v->kind == TY_RANGE || v->kind == TY_ITER) markIterUse(e->args[1]);
        coerce(c, &e->args[1], t->elem, "push");
        return &qkTypeVoid;
    }
    }
    return &qkTypeError;
}

static Type* fnReturnType(Checker* c, Decl* d, const Expr* at);

/* A constant's type, its value checked once, where no local name is in reach and nothing has effects. Its
 * value is computed where it is used (it is pure). */
static Type* checkPure(Checker* c, Expr** slot, TypeExpr* written, Type* want, const char* name) {
    Checker inner = {0};
    inner.prog = c->prog;
    inner.src = c->src;
    inner.inConst = true;
    inner.module = c->module;
    Type* t = checkExpr(&inner, *slot, true);
    if (written) want = resolveType(&inner, written);
    if (want) {
        coerce(&inner, slot, want, name);
        t = want;
    }
    if (t->kind == TY_RANGE || t->kind == TY_ITER) {
        markIterUse(*slot);
        coerce(&inner, slot, qkTypeArray(t->elem), name);
        t = (*slot)->type;
    }
    if (t == &qkTypeNone || t == &qkTypeEmpty || t == &qkTypeErr || t == &qkTypeEmptyMap) {
        qkError(c->src, (*slot)->line, (*slot)->col, "%s needs its type written: let %s: T = ...", name, name);
        t = &qkTypeError;
    }
    if (t->kind == TY_VOID) {
        qkError(c->src, (*slot)->line, (*slot)->col, "%s is given nothing", name);
        t = &qkTypeError;
    }
    return t;
}

static Type* constType(Checker* c, Decl* d, const Expr* at) {
    if (d->state == 2) return d->retType ? d->retType : &qkTypeError;
    if (d->state == 1) {
        qkError(c->src, at->line, at->col, "the constant %s needs itself", d->name);
        return &qkTypeError;
    }
    d->state = 1;
    Checker home = *c; /* where the constant is written */
    enterDecl(&home, d);
    d->retType = checkPure(&home, &d->body, d->ret, NULL, d->name);
    d->state = 2;
    return d->retType;
}

/* A call's arguments onto the parameters: by position, then by name (f(1, size: 2)), the rest from their
 * defaults. e->ordered gets one value per parameter, converted to its type; e->args keep what was written. */
static bool bindArgs(Checker* c, Expr* e, Param* params, int count, const char* what) {
    Expr** ordered = qkNew(sizeof(Expr*) * (size_t)(count ? count : 1));
    bool ok = true, named = false;
    for (int i = 0; i < e->argCount; i++) {
        Type* t = checkExpr(c, e->args[i], true);
        if (t->kind == TY_RANGE || t->kind == TY_ITER) markIterUse(e->args[i]);
        int p = i;
        if (e->argNames && e->argNames[i]) {
            named = true;
            p = -1;
            for (int k = 0; k < count; k++)
                if (!strcmp(params[k].name, e->argNames[i])) p = k;
            if (p < 0) {
                const char* names[64];
                int n = 0;
                for (int k = 0; k < count && n < 64; k++) names[n++] = params[k].name;
                const char* close = qkClosest(e->argNames[i], names, n);
                qkError(c->src, e->args[i]->line, e->args[i]->col, "%s has no parameter \"%s\"%s%s%s", what, e->argNames[i],
                        close ? ": did you mean \"" : "", close ? close : "", close ? "\"?" : "");
                ok = false;
                continue;
            }
        } else if (named) {
            qkError(c->src, e->args[i]->line, e->args[i]->col, "after a named value, every value is named");
            ok = false;
            continue;
        }
        if (p >= count) {
            qkError(c->src, e->line, e->col, "%s takes %d value%s, given %d", what, count, count == 1 ? "" : "s", e->argCount);
            return false;
        }
        if (ordered[p]) {
            qkError(c->src, e->args[i]->line, e->args[i]->col, "\"%s\" is given twice", params[p].name);
            ok = false;
            continue;
        }
        ordered[p] = e->args[i];
        coerce(c, &ordered[p], params[p].binding->type, params[p].name);
    }
    for (int p = 0; p < count; p++)
        if (!ordered[p]) {
            if (params[p].fallback) ordered[p] = params[p].fallback;
            else {
                qkError(c->src, e->line, e->col, "%s needs \"%s\" (%d value%s, given %d)", what, params[p].name, count, count == 1 ? "" : "s",
                        e->argCount);
                ok = false;
            }
        }
    e->ordered = ordered;
    e->orderedCount = count;
    return ok;
}

/* ---- templates: generic functions, and functions that take functions ----------------------------- */

static void checkFnBody(Checker* c, Decl* d);
static Type* checkPure(Checker* c, Expr** slot, TypeExpr* written, Type* want, const char* name);

static int typeParamIndex(const Decl* d, const char* name) {
    for (int i = 0; name && i < d->typeParamCount; i++)
        if (!strcmp(d->typeParams[i], name)) return i;
    return -1;
}

/* What a call's value says about the template's type parameters: [T] given [Int] binds T to Int. */
static void unify(const Decl* d, const TypeExpr* te, Type* actual, Type** bound) {
    if (!actual || actual->kind == TY_ERROR || actual == &qkTypeNone || actual == &qkTypeErr) return;
    if (te->optional || te->fallible) {
        Type* inner = qkFallibleValue(actual);
        unify(d, te->elem, inner ? inner : actual, bound);
        return;
    }
    if (te->isFn) { /* (T) -> R given (Int) -> Text */
        if (actual->kind != TY_FN || actual->paramCount != te->paramCount) return;
        for (int i = 0; i < te->paramCount; i++) unify(d, te->params[i], actual->params[i], bound);
        if (actual->elem) unify(d, te->elem, actual->elem, bound);
        return;
    }
    if (!te->name && te->key) {
        if (actual->kind == TY_MAP && actual->key) {
            unify(d, te->key, actual->key, bound);
            unify(d, te->elem, actual->elem, bound);
        }
        return;
    }
    if (!te->name) {
        if (qkIsIterable(actual) && actual->elem) unify(d, te->elem, actual->elem, bound);
        return;
    }
    if (te->argCount) { /* Pair<T, Int> given Pair<Text, Int>: T is Text */
        const Decl* ad = (actual->kind == TY_RECORD || actual->kind == TY_ENUM) ? actual->decl : NULL;
        if (ad && ad->templateOf && !strcmp(ad->templateOf->name, te->name))
            for (int i = 0; i < te->argCount && i < ad->typeParamCount; i++) unify(d, te->args[i], ad->typeArgs[i], bound);
        return;
    }
    int k = typeParamIndex(d, te->name);
    if (k < 0) return;
    if (actual->kind == TY_RANGE || actual->kind == TY_ITER) actual = qkTypeArray(actual->elem);
    if (actual == &qkTypeEmpty || actual == &qkTypeEmptyMap || actual->kind == TY_VOID) return;
    if (!bound[k] || (bound[k]->kind == TY_INT && actual->kind == TY_FLOAT)) bound[k] = actual;
}

static bool typeExprBound(const Decl* d, const TypeExpr* te, Type** bound, const char** missing) {
    if (!te) return true;
    if (te->isFn) {
        for (int i = 0; i < te->paramCount; i++)
            if (!typeExprBound(d, te->params[i], bound, missing)) return false;
        return typeExprBound(d, te->elem, bound, missing);
    }
    if (te->name) {
        int k = typeParamIndex(d, te->name);
        if (k >= 0 && !bound[k]) {
            *missing = te->name;
            return false;
        }
        for (int i = 0; i < te->argCount; i++)
            if (!typeExprBound(d, te->args[i], bound, missing)) return false;
        return true;
    }
    return typeExprBound(d, te->key, bound, missing) && typeExprBound(d, te->elem, bound, missing);
}

/* A copy of the template, with its types (and its lambdas) in place, checked like any function. */
static Decl* instantiate(Checker* c, Decl* d, Type** bound, Expr** lambdas, bool takesFn) {
    Decl* inst = qkCloneDecl(d->pristine);
    inst->templateOf = d;
    inst->isTemplate = false;
    inst->typeArgs = qkNew(sizeof(Type*) * (size_t)(d->typeParamCount ? d->typeParamCount : 1));
    memcpy(inst->typeArgs, bound, sizeof(Type*) * (size_t)d->typeParamCount);
    inst->inlineOnly = takesFn;
    QkBuf name = {0};
    qkBufPuts(&name, d->name);
    for (int i = 0; i < d->typeParamCount; i++) {
        qkBufPuts(&name, i ? ", " : "<");
        qkTypeName(bound[i], &name);
    }
    if (d->typeParamCount) qkBufPuts(&name, ">");
    if (takesFn) qkBufPrintf(&name, "#%d", d->instanceCount + 1);
    inst->name = qkDup(name.data, name.len);
    qkBufFree(&name);
    inst->index = c->prog->count;
    QK_PUSH(c->prog->decls, c->prog->count, c->prog->cap, inst);
    QK_PUSH(d->instances, d->instanceCount, d->instanceCap, inst);
    Checker ic = {0};
    ic.prog = c->prog;
    enterDecl(&ic, inst);
    ic.typing = inst;
    for (int f = 0; f < inst->paramCount; f++) {
        Param* prm = &inst->params[f];
        Binding* b = qkNew(sizeof *b);
        b->name = prm->name;
        b->type = resolveType(&ic, prm->type);
        b->isParam = true;
        if (b->type->kind == TY_FN) b->lambda = lambdas[f];
        prm->binding = b;
    }
    for (int f = 0; f < inst->paramCount; f++)
        if (inst->params[f].fallback) checkPure(&ic, &inst->params[f].fallback, NULL, inst->params[f].binding->type, inst->params[f].name);
    instantiating++;
    checkFnBody(&ic, inst);
    instantiating--;
    return inst;
}

/* A call to a template: its types from the values given, its lambdas checked where they are written; then the
 * instance for those types (made once), or for those lambdas (made for this call, and always inlined). */
static Type* checkTemplateCall(Checker* c, Expr* e, Decl* d) {
    if (d->state == 0) { /* checked loosely first: which of its functions it keeps */
        Checker tc = {0};
        tc.prog = c->prog;
        checkFnBody(&tc, d);
    }
    int count = d->paramCount;
    Expr** given = qkNew(sizeof(Expr*) * (size_t)(count ? count : 1));
    bool named = false;
    for (int i = 0; i < e->argCount; i++) {
        int p = i;
        if (e->argNames[i]) {
            named = true;
            p = -1;
            for (int k = 0; k < count; k++)
                if (!strcmp(d->params[k].name, e->argNames[i])) p = k;
            if (p < 0) {
                qkError(c->src, e->args[i]->line, e->args[i]->col, "%s has no parameter \"%s\"", d->name, e->argNames[i]);
                return &qkTypeError;
            }
        } else if (named) {
            qkError(c->src, e->args[i]->line, e->args[i]->col, "after a named value, every value is named");
            return &qkTypeError;
        }
        if (p >= count) {
            qkError(c->src, e->line, e->col, "%s takes %d value%s, given %d", d->name, count, count == 1 ? "" : "s", e->argCount);
            return &qkTypeError;
        }
        if (given[p]) {
            qkError(c->src, e->args[i]->line, e->args[i]->col, "\"%s\" is given twice", d->params[p].name);
            return &qkTypeError;
        }
        given[p] = e->args[i];
    }
    bool loose = c->typing && c->typing->isTemplate; /* inside a template being checked: no instance yet */
    Type** bound = qkNew(sizeof(Type*) * (size_t)(d->typeParamCount ? d->typeParamCount : 1));
    bool takesFn = false;
    for (int p = 0; p < count; p++) {
        if (d->params[p].type->isFn && !(d->valueParams && d->valueParams[p])) {
            takesFn = true;
            continue;
        }
        if (d->params[p].type->isFn) continue; /* a function value: below, once the other types are known */
        if (!given[p]) continue;
        Type* t = checkExpr(c, given[p], true);
        if (t->kind == TY_RANGE || t->kind == TY_ITER) markIterUse(given[p]);
        if (t->kind == TY_ERROR) loose = true;
        unify(d, d->params[p].type, t, bound);
    }
    Expr** lambdas = qkNew(sizeof(Expr*) * (size_t)(count ? count : 1));
    for (int p = 0; p < count; p++) {
        TypeExpr* fte = d->params[p].type;
        if (!fte->isFn) continue;
        if (given[p] && d->valueParams && d->valueParams[p]) { /* kept by the template: a function value, a closure */
            Type* params[16];
            const char* missing = NULL;
            bool known = true;
            for (int i = 0; i < fte->paramCount; i++) known &= typeExprBound(d, fte->params[i], bound, &missing);
            if (!known || loose) {
                if (!loose) qkError(c->src, given[p]->line, given[p]->col, "%s: what %s is cannot be told from the other values", d->name, missing);
                return &qkTypeError;
            }
            Decl view = *d;
            view.typeArgs = bound;
            Checker vc = *c;
            vc.typing = &view;
            for (int i = 0; i < fte->paramCount && i < 16; i++) params[i] = resolveType(&vc, fte->params[i]);
            Type proto = {TY_FN};
            proto.params = params;
            proto.paramCount = fte->paramCount;
            proto.effect = fte->effect;
            Type* t;
            if (given[p]->kind == E_LAMBDA) {
                if (!liftLambda(c, given[p], &proto)) return &qkTypeError;
                t = given[p]->type;
            } else {
                t = checkExpr(c, given[p], true);
            }
            if (t->kind == TY_ERROR) return t;
            unify(d, fte, t, bound);
            lambdas[p] = NULL;
            continue;
        }
        if (!given[p]) {
            qkError(c->src, e->line, e->col, "%s needs a function for \"%s\": a lambda or a function's name", d->name, d->params[p].name);
            return &qkTypeError;
        }
        Type* params[16];
        const char* missing = NULL;
        bool known = true;
        for (int i = 0; i < fte->paramCount; i++) known &= typeExprBound(d, fte->params[i], bound, &missing);
        Decl view = *d;
        view.typeArgs = bound;
        Checker vc = *c;
        vc.typing = &view;
        for (int i = 0; i < fte->paramCount && i < 16; i++) params[i] = known && !loose ? resolveType(&vc, fte->params[i]) : &qkTypeError;
        if (!known && !loose) {
            qkError(c->src, given[p]->line, given[p]->col, "%s: what %s is cannot be told from the other values", d->name, missing);
            return &qkTypeError;
        }
        Binding* fb = given[p]->kind == E_NAME ? lookup(c, given[p]->name) : NULL;
        Type* r;
        if (fb && fb->type->kind == TY_FN && fb->lambda) { /* passed on: the caller's own lambda */
            lambdas[p] = fb->lambda;
            r = fb->type->elem;
            fb->uses++;
        } else {
            r = checkLambda(c, &given[p], params, fte->paramCount, fte->paramCount, d->name);
            lambdas[p] = given[p];
        }
        if (r->kind == TY_ERROR || lambdas[p]->kind != E_LAMBDA) loose = true;
        else unify(d, fte->elem, r, bound);
    }
    if (loose) return &qkTypeError;
    for (int i = 0; i < d->typeParamCount; i++)
        if (!bound[i]) {
            qkError(c->src, e->line, e->col, "%s: what %s is cannot be told from the values given", d->name, d->typeParams[i]);
            return &qkTypeError;
        }
    if (instantiating > 24) {
        qkError(c->src, e->line, e->col, "%s keeps making new versions of itself (a function that takes a function, or new types, calling itself)", d->name);
        return &qkTypeError;
    }
    Decl* inst = NULL;
    for (int i = 0; !takesFn && i < d->instanceCount && !inst; i++) {
        bool same = true;
        for (int k = 0; k < d->typeParamCount; k++) same &= d->instances[i]->typeArgs[k] == bound[k];
        if (same) inst = d->instances[i];
    }
    if (!inst) inst = instantiate(c, d, bound, lambdas, takesFn);
    /* a lambda's value becomes what the function type says (an Int where a Float goes) */
    for (int p = 0; p < count; p++) {
        Type* ft = inst->params[p].binding->type;
        if (ft->kind != TY_FN || !lambdas[p] || lambdas[p]->kind != E_LAMBDA || !lambdas[p]->body) continue;
        if (lambdas[p]->type && lambdas[p]->type != ft->elem && given[p] == lambdas[p]) {
            coerce(c, &lambdas[p]->body, ft->elem, "the function's result");
            lambdas[p]->type = ft->elem;
        }
    }
    e->decl = inst;
    usesEffectsOf(c, e, inst, 0);
    Expr** ordered = qkNew(sizeof(Expr*) * (size_t)(count ? count : 1));
    for (int p = 0; p < count; p++) {
        if (inst->params[p].binding->type->kind == TY_FN && lambdas[p]) {
            ordered[p] = lambdas[p];
        } else if (given[p]) {
            ordered[p] = given[p];
            coerce(c, &ordered[p], inst->params[p].binding->type, inst->params[p].name);
        } else if (inst->params[p].fallback) {
            ordered[p] = inst->params[p].fallback;
        } else {
            qkError(c->src, e->line, e->col, "%s needs \"%s\"", d->name, d->params[p].name);
            return &qkTypeError;
        }
    }
    e->ordered = ordered;
    e->orderedCount = count;
    return fnReturnType(c, inst, e);
}

/* f(x) where f is a function this one was given */
static Type* checkFnParamCall(Checker* c, Expr* e, Binding* fb) {
    Type* ft = fb->type;
    fb->uses++;
    e->binding = fb;
    if (fb->mutable) c->varReads++;
    if (ft->effect) needEffect(c, e, e->name);
    if (e->argCount != ft->paramCount) {
        qkError(c->src, e->line, e->col, "%s takes %d value%s, not %d", e->name, ft->paramCount, ft->paramCount == 1 ? "" : "s", e->argCount);
        return &qkTypeError;
    }
    for (int i = 0; i < e->argCount; i++) {
        if (e->argNames[i]) qkError(c->src, e->args[i]->line, e->args[i]->col, "%s takes no named values", e->name);
        Type* t = checkExpr(c, e->args[i], true);
        if (t->kind == TY_RANGE || t->kind == TY_ITER) markIterUse(e->args[i]);
        coerce(c, &e->args[i], ft->params[i], e->name);
    }
    e->ordered = e->args;
    e->orderedCount = e->argCount;
    return ft->elem;
}

/* A generic type's instance for these types (made once): its fields resolved with T standing for each. */
static Type* instanceType(Checker* c, Decl* d, Type** args) {
    for (int i = 0; i < d->instanceCount; i++) {
        bool same = true;
        for (int k = 0; k < d->typeParamCount; k++) same &= d->instances[i]->typeArgs[k] == args[k];
        if (same) return d->instances[i]->type;
    }
    Decl* inst = qkCloneDecl(d->pristine ? d->pristine : d);
    inst->templateOf = d;
    inst->isTemplate = false;
    inst->typeArgs = qkNew(sizeof(Type*) * (size_t)d->typeParamCount);
    memcpy(inst->typeArgs, args, sizeof(Type*) * (size_t)d->typeParamCount);
    QkBuf name = {0};
    qkBufPuts(&name, d->name);
    for (int i = 0; i < d->typeParamCount; i++) {
        qkBufPuts(&name, i ? ", " : "<");
        qkTypeName(args[i], &name);
    }
    qkBufPuts(&name, ">");
    inst->name = qkDup(name.data, name.len);
    qkBufFree(&name);
    inst->type = qkNew(sizeof(Type));
    inst->type->kind = d->kind == D_ENUM ? TY_ENUM : TY_RECORD;
    inst->type->decl = inst;
    inst->state = 2;
    inst->index = c->prog->count;
    QK_PUSH(c->prog->decls, c->prog->count, c->prog->cap, inst);
    QK_PUSH(d->instances, d->instanceCount, d->instanceCap, inst); /* before its fields: List<T> holds a List<T> */
    Checker ic = {0};
    ic.prog = c->prog;
    enterDecl(&ic, inst);
    ic.typing = inst;
    Param* fields[65];
    int counts[65], groups = 0;
    if (inst->kind == D_TYPE) {
        fields[groups] = inst->params;
        counts[groups++] = inst->paramCount;
    } else {
        for (int k = 0; inst->caseFieldCount && k < inst->caseCount; k++) {
            fields[groups] = inst->caseFields[k];
            counts[groups++] = inst->caseFieldCount[k];
        }
    }
    for (int g = 0; g < groups; g++)
        for (int f = 0; f < counts[g]; f++) {
            Binding* b = qkNew(sizeof *b);
            b->name = fields[g][f].name;
            b->type = resolveType(&ic, fields[g][f].type);
            fields[g][f].binding = b;
        }
    for (int g = 0; g < groups; g++)
        for (int f = 0; f < counts[g]; f++) boxIfRecursive(&fields[g][f], inst->type);
    return inst->type;
}

/* A call that makes a generic record or case: its types from the values given (each checked once here). */
static Decl* inferInstance(Checker* c, Expr* e, Decl* d, Param* fields, int count) {
    Type* bound[16] = {0};
    for (int i = 0; i < e->argCount; i++) {
        int f = i;
        if (e->argNames && e->argNames[i]) {
            f = -1;
            for (int k = 0; k < count; k++)
                if (!strcmp(fields[k].name, e->argNames[i])) f = k;
        }
        Type* t = checkExpr(c, e->args[i], true);
        e->args[i]->typed = true;
        if (t->kind == TY_ERROR) return NULL; /* said already, or a template's T: its instances will tell */
        if (f >= 0 && f < count) unify(d, fields[f].type, t, bound);
    }
    for (int k = 0; k < d->typeParamCount; k++)
        if (!bound[k]) {
            qkError(c->src, e->line, e->col, "%s: what %s is cannot be told from the values: write the type where it goes, let x: %s<...> = ...",
                    d->name, d->typeParams[k], d->name);
            return NULL;
        }
    return instanceType(c, d, bound)->decl;
}

static Type* fnValueType(Checker* c, Decl* d, const Expr* at) {
    if (d->isExtern) {
        qkError(c->src, at->line, at->col, "%s is the host's: keep a function of the program instead, fn f(...) = %s(...)", d->name, d->name);
        return &qkTypeError;
    }
    if (d->isTemplate || d->isTest) {
        qkError(c->src, at->line, at->col, "%s is generic, or takes a function: call it, %s(...)", d->name, d->name);
        return &qkTypeError;
    }
    Type* params[16];
    for (int i = 0; i < d->paramCount && i < 16; i++) params[i] = d->params[i].binding->type;
    Type* ret = fnReturnType(c, d, at);
    if (ret->kind == TY_ERROR) return ret;
    usesEffectsOf(c, at, d, EFFECT_AS_VALUE);
    return typeFn(params, d->paramCount, ret, d->isEffect);
}

/* ---- capabilities: what a program may reach ------------------------------------------------------ */

const char* qkCapabilities;

static bool listed(const char* name) {
    const char* p = qkCapabilities;
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t') p++;
        const char* start = p;
        while (*p && *p != ' ' && *p != ',' && *p != '\n' && *p != '\t') p++;
        size_t n = (size_t)(p - start);
        if (!n) continue;
        if (n == 1 && *start == '*') return true;
        if (start[n - 1] == '*' && !strncmp(name, start, n - 1)) return true; /* a prefix: Object.*, net* */
        if (strlen(name) == n && !strncmp(name, start, n)) return true;
    }
    return false;
}

bool qkCapable(const char* name) { return !qkCapabilities || listed(name); }

/* The group a built-in that reaches outside the program belongs to, or NULL */
static const char* builtinGroup(int b) {
    switch (b) {
    case B_PRINT: case B_PRINTERR: return "print";
    case B_READFILE: case B_WRITEFILE: case B_FILEEXISTS: case B_LISTDIR: return "files";
    case B_ENV: return "env";
    case B_ARGS: return "args";
    case B_EXIT: return "exit";
    case B_READLINE: return "stdin";
    case B_NOW: case B_TIME: return "clock";
    case B_RANDOM: case B_RANDOMINT: case B_SEEDRANDOM: return "random";
    case B_SPAWN: case B_STEP: case B_YIELD: case B_WAIT: return "tasks";
    default: return NULL;
    }
}

static bool capable(Checker* c, const Expr* e, const char* name, const char* group) {
    if (qkCapable(group ? group : name) || (group && qkCapable(name))) return true;
    qkError(c->src, e->line, e->col, "%s is not given to this program (the host allows: %s)%s%s", name, qkCapabilities[0] ? qkCapabilities : "nothing",
            group ? "; it is in the group " : "", group ? group : "");
    return false;
}

/* a record's field that holds a function: button.onPress(3) calls it */
static bool fnField(const Type* t, const char* name) {
    for (int f = 0; t->kind == TY_RECORD && f < t->decl->paramCount; f++)
        if (!strcmp(t->decl->params[f].name, name) && t->decl->params[f].binding->type->kind == TY_FN) return true;
    return false;
}

static Type* checkCall(Checker* c, Expr* e) {
    if (!strcmp(e->name, "(,)")) { /* (a, b): a tuple */
        Type* elems[16];
        for (int i = 0; i < e->argCount && i < 16; i++) {
            elems[i] = checkExpr(c, e->args[i], true);
            if (elems[i]->kind == TY_RANGE || elems[i]->kind == TY_ITER) {
                markIterUse(e->args[i]);
                coerce(c, &e->args[i], qkTypeArray(elems[i]->elem), "a tuple's value");
                elems[i] = e->args[i]->type;
            }
            if (elems[i] == &qkTypeNone || elems[i] == &qkTypeEmpty || elems[i] == &qkTypeEmptyMap || elems[i] == &qkTypeErr ||
                elems[i]->kind == TY_VOID || elems[i] == &lambdaType) {
                typeError(c, e->args[i], "a tuple's value needs a type of its own, not %s", elems[i], NULL);
                return &qkTypeError;
            }
            if (elems[i]->kind == TY_ERROR) return elems[i];
        }
        Type* t = typeTuple(elems, e->argCount);
        e->decl = t->decl;
        e->ordered = e->args;
        e->orderedCount = e->argCount;
        return t;
    }
    Binding* fb = e->qualifier ? NULL : lookupOrCapture(c, e->name);
    if (!strcmp(e->name, "()") && e->argCount) { /* pick(true)(5): the function a value gives, called */
        Type* ft = checkExpr(c, e->args[0], true);
        if (ft->kind == TY_ERROR) return ft;
        if (ft->kind != TY_FN || ft == &lambdaType) {
            typeError(c, e->args[0], "only a function can be called, not %s", ft, NULL);
            return &qkTypeError;
        }
        e->callee = e->args[0];
        if (e->argCount - 1 != ft->paramCount) {
            qkError(c->src, e->line, e->col, "this function takes %d value%s, not %d", ft->paramCount, ft->paramCount == 1 ? "" : "s", e->argCount - 1);
            return &qkTypeError;
        }
        if (ft->effect) needEffect(c, e, "this function");
        for (int i = 1; i < e->argCount; i++) {
            Type* t = checkExpr(c, e->args[i], true);
            if (t->kind == TY_RANGE || t->kind == TY_ITER) markIterUse(e->args[i]);
            coerce(c, &e->args[i], ft->params[i - 1], "the function");
        }
        e->ordered = e->args + 1;
        e->orderedCount = e->argCount - 1;
        return ft->elem;
    }
    if (fb && fb->type->kind == TY_FN) return checkFnParamCall(c, e, fb);
    Decl* d = findDecl(c, e->name);
    if (e->qualifier) d = NULL; /* a case: below */
    if (d && d->kind == D_TYPE && d->isExtern) {
        qkError(c->src, e->line, e->col, "%s comes from the host: it is given to the program, never made by it", d->name);
        return &qkTypeError;
    }
    if (d && d->kind == D_TYPE && d->isTemplate) { /* Pair(1, "a"): Pair<Int, Text>, from the values */
        d = inferInstance(c, e, d, d->params, d->paramCount);
        if (!d) return &qkTypeError;
    }
    if (d && d->kind == D_TYPE) { /* a record: its fields by position, then by name */
        e->decl = d;
        if (!bindArgs(c, e, d->params, d->paramCount, d->name)) return &qkTypeError;
        return d->type;
    }
    if (e->qualifier) { /* Shape.Circle(2): a case with data */
        Decl* en = findDecl(c, e->qualifier);
        if (!en || en->kind != D_ENUM) {
            qkError(c->src, e->line, e->col, "%s is not an enum", e->qualifier);
            return &qkTypeError;
        }
        int k = -1;
        for (int i = 0; i < en->caseCount; i++)
            if (!strcmp(en->cases[i], e->name)) k = i;
        if (k < 0) {
            const char* close = qkClosest(e->name, en->cases, en->caseCount);
            qkError(c->src, e->line, e->col, "%s has no case %s%s%s%s", en->name, e->name, close ? ": did you mean " : "", close ? close : "",
                    close ? "?" : "");
            return &qkTypeError;
        }
        int n = en->caseFieldCount ? en->caseFieldCount[k] : 0;
        if (!n) {
            qkError(c->src, e->line, e->col, "%s.%s carries no data: write %s.%s", en->name, e->name, en->name, e->name);
            return &qkTypeError;
        }
        if (en->isTemplate) { /* Tree.Node(1, ...): Tree<Int>, from the values */
            en = inferInstance(c, e, en, en->caseFields[k], n);
            if (!en) return &qkTypeError;
        }
        e->decl = en;
        e->caseIndex = k;
        if (!bindArgs(c, e, en->caseFields[k], n, e->name)) return &qkTypeError;
        return en->type;
    }
    bool ownFn = false;
    bool generic = e->method && c->typing && (c->typing->isTemplate || c->typing->templateOf) && !builtinNamed(e->name) && !d;
    if (!e->qualifier && e->argCount && !e->argNames[0] && e->args[0]->kind != E_LAMBDA && (anyTypeFunction(c, e->name) || generic)) {
        /* v.length(): the receiver's type's own function comes first */
        Type* t = checkExpr(c, e->args[0], true);
        e->args[0]->typed = true;
        Decl* own = typeFunction(c, t, e->name);
        if (own) d = own, ownFn = true; /* j.text(): the type's own function, even with a local named text */
        else if (t->kind == TY_ERROR && (t->typeVar || (c->typing && c->typing->isTemplate)) && !d) { /* x.speak() on a template's T: checked in each instance */
            for (int i = 1; i < e->argCount; i++)
                if (e->args[i]->kind != E_LAMBDA) checkExpr(c, e->args[i], true);
            return &qkTypeError;
        } else if (!d && c->typing && c->typing->templateOf && t->kind != TY_ERROR && !fnField(t, e->name) && !builtinNamed(e->name)) { /* an instance: say whose T it is (a built-in, xs.push, is not) */
            QkBuf n = {0};
            qkTypeName(t, &n);
            qkError(c->src, e->line, e->col, "%s has no function %s: %s needs fn %s.%s(...) for it", n.data, e->name, c->typing->name, n.data, e->name);
            qkBufFree(&n);
            return &qkTypeError;
        }
    }
    if (d && d->kind == D_FN && d->isTemplate && (ownFn || !lookup(c, e->name))) return checkTemplateCall(c, e, d);
    if (d && d->kind == D_FN && d->isTest) {
        qkError(c->src, e->line, e->col, "a test is run by quark test, not called");
        return &qkTypeError;
    }
    if (d && d->kind == D_FN && (ownFn || !lookup(c, e->name))) {
        if (d->isExtern && !capable(c, e, d->name, NULL)) return &qkTypeError;
        e->decl = d;
        usesEffectsOf(c, e, d, 0);
        bindArgs(c, e, d->params, d->paramCount, d->name);
        return fnReturnType(c, d, e);
    }
    if (d && d->kind == D_ENUM) {
        qkError(c->src, e->line, e->col, "%s is an enum: pick a case, %s.%s", d->name, d->name, d->caseCount ? d->cases[0] : "Case");
        return &qkTypeError;
    }
    if (d && d->kind == D_CONST) {
        qkError(c->src, e->line, e->col, "%s is a constant, not a function", d->name);
        return &qkTypeError;
    }
    if (e->method && e->argCount && !d) { /* button.onPress(3): a function kept in a field */
        Type* rt = checkExpr(c, e->args[0], true);
        e->args[0]->typed = true;
        for (int f = 0; rt->kind == TY_RECORD && f < rt->decl->paramCount; f++) {
            Param* prm = &rt->decl->params[f];
            if (strcmp(prm->name, e->name) || prm->binding->type->kind != TY_FN) continue;
            Expr* field = qkNewExpr(E_FIELD, e->line, e->col);
            field->a = e->args[0];
            field->name = e->name;
            Type* ft = checkExpr(c, field, true);
            e->callee = field;
            if (e->argCount - 1 != ft->paramCount) {
                qkError(c->src, e->line, e->col, "%s takes %d value%s, not %d", e->name, ft->paramCount, ft->paramCount == 1 ? "" : "s", e->argCount - 1);
                return &qkTypeError;
            }
            if (ft->effect) needEffect(c, e, e->name);
            for (int i = 1; i < e->argCount; i++) {
                Type* t = checkExpr(c, e->args[i], true);
                if (t->kind == TY_RANGE || t->kind == TY_ITER) markIterUse(e->args[i]);
                coerce(c, &e->args[i], ft->params[i - 1], e->name);
            }
            e->ordered = e->args + 1;
            e->orderedCount = e->argCount - 1;
            return ft->elem;
        }
    }
    int b = builtinNamed(e->name);
    if (b == B_PARALLELMAP) return checkParallelMap(c, e);
    if (b && builtinGroup(b) && !capable(c, e, e->name, builtinGroup(b))) return &qkTypeError;
    if (b) return checkBuiltin(c, e, b);
    if (lookup(c, e->name)) {
        qkError(c->src, e->line, e->col, "\"%s\" is a value, not a function", e->name);
        return &qkTypeError;
    }
    unknownName(c, e);
    for (int i = 0; i < e->argCount; i++)
        if (e->args[i]->kind != E_LAMBDA) checkExpr(c, e->args[i], true);
    return &qkTypeError;
}

/* ---- blocks and statements ----------------------------------------------------------------------- */

static void checkStmt(Checker* c, Stmt* s);

/* The last statement's value is the block's, when one is wanted. */
static Type* checkBlock(Checker* c, Block* b, bool wantValue) {
    int mark = c->scopeCount;
    Type* t = &qkTypeVoid;
    for (int i = 0; i < b->count; i++) {
        Stmt* s = b->stmts[i];
        if (i == b->count - 1 && wantValue && s->kind == S_EXPR) {
            t = checkExpr(c, s->value, true);
        } else {
            checkStmt(c, s);
            if (i == b->count - 1 && s->kind == S_RETURN) t = NULL; /* never falls through */
        }
    }
    c->scopeCount = mark;
    return t ? t : &qkTypeVoid;
}

static Expr** blockResult(Expr* e) {
    if (e->kind != E_BLOCK) return NULL;
    Block* b = e->block;
    if (!b->count || b->stmts[b->count - 1]->kind != S_EXPR) return NULL;
    return &b->stmts[b->count - 1]->value;
}

/* Branch values must agree; an Int and a Float become Floats. */
static Type* unifyBranches(Checker* c, Expr*** slots, int n, Expr* at) {
    int nevers = 0;
    for (int i = 0; i < n; i++) nevers += slots[i] && (*slots[i])->type->kind == TY_NEVER;
    if (nevers) { /* a branch that never gives a value (exit, panic) takes the others' type */
        if (nevers == n) return &qkTypeNever;
        Expr*** rest = qkNew(sizeof(Expr**) * (size_t)n);
        int m = 0;
        for (int i = 0; i < n; i++)
            if (!slots[i] || (*slots[i])->type->kind != TY_NEVER) rest[m++] = slots[i];
        Type* r = unifyBranches(c, rest, m, at);
        for (int i = 0; i < n; i++)
            if (slots[i] && (*slots[i])->type->kind == TY_NEVER) coerce(c, slots[i], r, "this branch");
        return r;
    }
    Type* t = NULL;
    bool anyFloat = false, allNumeric = true;
    for (int i = 0; i < n; i++) {
        Type* bt = slots[i] ? (*slots[i])->type : &qkTypeVoid;
        if (bt->kind == TY_ERROR) return &qkTypeError;
        anyFloat |= bt->kind == TY_FLOAT;
        allNumeric &= isNumeric(bt);
        if (!t) t = bt;
    }
    if (allNumeric && anyFloat) {
        for (int i = 0; i < n; i++) coerce(c, slots[i], &qkTypeFloat, "this branch");
        return &qkTypeFloat;
    }
    /* an optional among the branches (or a typed array among []): every branch becomes one */
    Type* target = NULL;
    for (int i = 0; i < n && !target; i++) {
        Type* bt = slots[i] ? (*slots[i])->type : &qkTypeVoid;
        if (qkFallibleValue(bt)) target = bt;
    }
    for (int i = 0; i < n && !target; i++) {
        Type* bt = slots[i] ? (*slots[i])->type : &qkTypeVoid;
        if (bt->kind == TY_ARRAY && bt != &qkTypeEmpty) target = bt;
    }
    bool anyNone = false, anyEmpty = false, anyErr = false;
    for (int i = 0; i < n; i++) {
        Type* bt = slots[i] ? (*slots[i])->type : &qkTypeVoid;
        anyNone |= bt == &qkTypeNone;
        anyEmpty |= bt == &qkTypeEmpty || bt == &qkTypeEmptyMap;
        anyErr |= bt == &qkTypeErr;
    }
    for (int i = 0; i < n && !target; i++) { /* Tree.Leaf among Tree<Int>s */
        Type* bt = slots[i] ? (*slots[i])->type : &qkTypeVoid;
        for (int j = 0; j < n && isPlaceholder(bt) && !target; j++) {
            Type* other = slots[j] ? (*slots[j])->type : &qkTypeVoid;
            if (other->decl && other->decl->templateOf == bt->decl) target = other;
        }
    }
    bool anyPlaceholder = false;
    for (int i = 0; i < n; i++) anyPlaceholder |= slots[i] && isPlaceholder((*slots[i])->type);
    for (int i = 0; i < n && !target; i++) { /* a typed map among [:] */
        Type* bt = slots[i] ? (*slots[i])->type : &qkTypeVoid;
        if (bt->kind == TY_MAP && bt != &qkTypeEmptyMap) target = bt;
    }
    if ((anyNone || anyErr) && !target && c->result && (anyErr ? isResult(c->result) : isOption(c->result))) target = c->result;
    if ((anyNone || anyErr) && !target) { /* None and a T: a T?; Err(...) and a T: a T! */
        for (int i = 0; i < n && !target; i++) {
            Type* bt = slots[i] ? (*slots[i])->type : &qkTypeVoid;
            if (bt != &qkTypeNone && bt != &qkTypeErr && bt->kind != TY_VOID) target = anyErr ? qkTypeResult(bt) : qkTypeOption(bt);
        }
    }
    if (target && (anyNone || anyEmpty || anyErr || anyPlaceholder || target->kind == TY_ENUM)) {
        for (int i = 0; i < n; i++)
            if (slots[i] && !coerce(c, slots[i], target, "this branch")) return &qkTypeError;
        return target;
    }
    for (int i = 0; i < n; i++) {
        Type* bt = slots[i] ? (*slots[i])->type : &qkTypeVoid;
        if (!qkTypeSame(bt, t)) {
            typeError(c, slots[i] ? *slots[i] : at, "the branches give different types: %s and %s", t, bt);
            return &qkTypeError;
        }
    }
    if (t->kind == TY_RANGE || t->kind == TY_ITER) {
        Type* arr = qkTypeArray(t->elem);
        for (int i = 0; i < n; i++) coerce(c, slots[i], arr, "this branch");
        return arr;
    }
    return t;
}

static Expr** branchSlot(Expr** body) {
    Expr** r = blockResult(*body);
    return r ? r : (*body)->kind == E_BLOCK ? NULL : body;
}

static Type* checkIf(Checker* c, Expr* e, bool wantValue) {
    Type* result = c->result; /* for this if's branches, not what is inside them */
    c->result = NULL;
    Type* cond = checkExpr(c, e->a, true);
    if (cond->kind != TY_BOOL && cond->kind != TY_ERROR) typeError(c, e->a, "the condition must be a Bool, not %s", cond, NULL);
    Type* thenT = checkBlock(c, e->thenBlock, wantValue);
    if (!e->elseExpr) {
        if (wantValue && thenT->kind != TY_VOID) {
            qkError(c->src, e->line, e->col, "an if that gives a value needs an else");
            return &qkTypeError;
        }
        return &qkTypeVoid;
    }
    Type* elseT = checkExpr(c, e->elseExpr, wantValue);
    if (!wantValue) return &qkTypeVoid;
    if (thenT->kind == TY_VOID && elseT->kind == TY_VOID) return &qkTypeVoid;
    Expr thenHolder = {0};
    thenHolder.kind = E_BLOCK;
    thenHolder.block = e->thenBlock;
    Expr* thenPtr = &thenHolder;
    Expr** slots[2] = {blockResult(thenPtr), branchSlot(&e->elseExpr)};
    if (!slots[0] || !slots[1]) {
        qkError(c->src, e->line, e->col, "both branches must end with a value");
        return &qkTypeError;
    }
    c->result = result;
    Type* r = unifyBranches(c, slots, 2, e);
    c->result = NULL;
    return r;
}

/* Some(x), Circle(r, _): the case of enum t the pattern names (a->caseIndex), its names declared; false when it
 * is not one of t's cases. */
/* A pattern on a value of type t: Some(x), Circle(r, _), Point(x, 0), (q, r), 3, "quit", with patterns inside;
 * its names declared. False when it cannot be one of t's. */
static bool bindPattern(Checker* c, Arm* a, Type* t, const Expr* at) {
    if (t->kind == TY_ERROR) return false;
    if (a->textPattern) {
        if (t->kind == TY_TEXT) return true;
        typeError(c, a->textPattern, "a text pattern, but the value is %s", t, NULL);
        return false;
    }
    if (a->intPattern || a->isRange) {
        if (t->kind == TY_INT || t->kind == TY_FLOAT) return true; /* Box(w, 1): a Float field too */
        typeError(c, a->intPattern ? a->intPattern : a->intHigh, "an Int pattern, but the value is %s", t, NULL);
        return false;
    }
    if (!a->pattern) return true; /* _ */
    Decl* d = t->decl;
    Param* fields;
    int fieldCount;
    if (t->kind == TY_RECORD) { /* Point(x, y), (q, r): the record itself */
        const char* name = d->isEntry ? "Entry" : d->templateOf ? d->templateOf->name : d->name;
        bool tuple = !strcmp(a->pattern, "(,)");
        if (tuple != d->isTuple || (!tuple && strcmp(a->pattern, name))) {
            QkBuf n = {0};
            qkTypeName(t, &n);
            qkError(c->src, at->line, at->col, "%s is not a pattern of %s", tuple ? "a tuple" : a->pattern, n.data);
            qkBufFree(&n);
            return false;
        }
        a->caseIndex = 0;
        fields = d->params;
        fieldCount = d->paramCount;
    } else if (t->kind == TY_ENUM && t != &qkTypeNone && t != &qkTypeErr) {
        a->caseIndex = -1;
        for (int k = 0; k < d->caseCount; k++)
            if (!strcmp(d->cases[k], a->pattern)) a->caseIndex = k;
        if (a->caseIndex < 0) {
            const char* close = qkClosest(a->pattern, d->cases, d->caseCount);
            qkError(c->src, at->line, at->col, "%s has no case %s%s%s%s", d->name, a->pattern, close ? ": did you mean " : "",
                    close ? close : "", close ? "?" : "");
            return false;
        }
        fields = d->caseFields ? d->caseFields[a->caseIndex] : NULL;
        fieldCount = d->caseFieldCount ? d->caseFieldCount[a->caseIndex] : 0;
    } else {
        QkBuf n = {0};
        qkTypeName(t, &n);
        qkError(c->src, at->line, at->col, "%s is a pattern of a case or a record, but the value is %s", a->pattern, n.data);
        qkBufFree(&n);
        return false;
    }
    if ((a->bindCount || t->kind == TY_RECORD) && a->bindCount != fieldCount) {
        qkError(c->src, at->line, at->col, "%s carries %d value%s: name each (or _)", a->pattern, fieldCount, fieldCount == 1 ? "" : "s");
        return false;
    }
    a->bindings = qkNew(sizeof(Binding*) * (size_t)(a->bindCount ? a->bindCount : 1));
    for (int f = 0; f < a->bindCount; f++) {
        Type* ft = fields[f].binding->type;
        if (a->subs && a->subs[f]) {
            if (!bindPattern(c, a->subs[f], ft, at)) return false;
            continue;
        }
        if (!strcmp(a->binds[f], "_")) continue;
        a->bindings[f] = declare(c, a->binds[f], ft, false, at->line, at->col);
    }
    return true;
}

/* Can a pattern fail on a value of its type? Names, _, records and one-case enums of patterns that cannot. */
static bool refutable(const Arm* a, const Type* t) {
    if (a->textPattern || a->intPattern || a->isRange) return true;
    if (!a->pattern) return false;
    if (t->kind == TY_ENUM && t->decl->caseCount > 1) return true;
    const Decl* d = t->decl;
    for (int f = 0; a->subs && f < a->bindCount; f++) {
        if (!a->subs[f]) continue;
        const Type* ft = t->kind == TY_RECORD ? d->params[f].binding->type : d->caseFields[a->caseIndex][f].binding->type;
        if (refutable(a->subs[f], ft)) return true;
    }
    return false;
}

/* ---- exhaustiveness: do the arms cover every value? A matrix of patterns (rows: arms; NULL: anything), its
 * first column taken apart case by case (Maranget's way, as OCaml and Rust do). */

static bool wildcardPattern(const Arm* a) { return !a || (!a->pattern && !a->intPattern && !a->isRange && !a->textPattern); }

static const Type* fieldType(const Type* t, int k, int f) {
    return t->kind == TY_RECORD ? t->decl->params[f].binding->type : t->decl->caseFields[k][f].binding->type;
}

static int fieldsOf(const Type* t, int k) {
    if (t->kind == TY_RECORD) return t->decl->paramCount;
    return t->decl->caseFieldCount ? t->decl->caseFieldCount[k] : 0;
}

static bool covers(Arm*** rows, int rowCount, const Type** cols, int colCount, int depth) {
    if (!rowCount) return false;
    if (!colCount || depth > 40) return true;
    const Type* t = cols[0];
    bool split = false; /* does the first column take values apart (cases, records)? */
    for (int r = 0; r < rowCount; r++) split |= rows[r][0] && rows[r][0]->pattern;
    bool enumSplit = split && t->kind == TY_ENUM;
    int ways = enumSplit ? t->decl->caseCount : 1;
    if (!split) { /* anything, or literals: only the rows that take anything cover the rest */
        Arm*** keep = qkNew(sizeof(Arm**) * (size_t)rowCount);
        int n = 0;
        for (int r = 0; r < rowCount; r++)
            if (wildcardPattern(rows[r][0])) keep[n++] = rows[r] + 1;
        return covers(keep, n, cols + 1, colCount - 1, depth + 1);
    }
    for (int k = 0; k < ways; k++) { /* each case (or the record): its fields, then the other columns */
        int fields = fieldsOf(t, k);
        int width = fields + colCount - 1;
        const Type** sub = qkNew(sizeof(Type*) * (size_t)(width ? width : 1));
        for (int f = 0; f < fields; f++) sub[f] = fieldType(t, k, f);
        for (int c = 1; c < colCount; c++) sub[fields + c - 1] = cols[c];
        Arm*** next = qkNew(sizeof(Arm**) * (size_t)rowCount);
        int n = 0;
        for (int r = 0; r < rowCount; r++) {
            Arm* a = rows[r][0];
            if (!wildcardPattern(a) && (!a->pattern || (enumSplit && a->caseIndex != k))) continue;
            Arm** row = qkNew(sizeof(Arm*) * (size_t)(width ? width : 1));
            for (int f = 0; f < fields; f++) row[f] = a && a->subs && f < a->bindCount ? a->subs[f] : NULL;
            for (int c = 1; c < colCount; c++) row[fields + c - 1] = rows[r][c];
            next[n++] = row;
        }
        if (!covers(next, n, sub, width, depth + 1)) return false;
    }
    return true;
}

/* Does a case's arm need its data's patterns to match too (Some(Circle(r))): then it does not cover the case */
static bool subsRefutable(const Arm* a, const Type* t) {
    if (!a->subs || t->kind != TY_ENUM) return false;
    for (int f = 0; f < a->bindCount; f++)
        if (a->subs[f] && refutable(a->subs[f], t->decl->caseFields[a->caseIndex][f].binding->type)) return true;
    return false;
}

static Type* checkMatch(Checker* c, Expr* e, bool wantValue) {
    Type* result = c->result;
    c->result = NULL;
    Type* t = checkExpr(c, e->a, true);
    bool onText = t->kind == TY_TEXT;
    bool onRecord = t->kind == TY_RECORD;
    if ((t->kind != TY_ENUM && t->kind != TY_INT && !onText && !onRecord) || t == &qkTypeNone || t == &qkTypeErr) {
        if (t->kind != TY_ERROR)
            typeError(c, e->a, "match works on an enum, an optional, a result, a record, an Int or a Text, not %s", t, NULL);
        return &qkTypeError;
    }
    bool wildcard = false;
    bool seen[256] = {0};
    for (int i = 0; i < e->armCount; i++) {
        Arm* a = &e->arms[i];
        int mark = c->scopeCount;
        bool ok = true;
        if (!a->pattern && !a->intPattern && !a->isRange && !a->textPattern) {
            if (!a->guard) {
                wildcard = true;
                if (i != e->armCount - 1) qkError(c->src, a->body->line, a->body->col, "_ catches everything: it goes last");
            }
        } else if (a->pattern) {
            if (t->kind != TY_ENUM && !onRecord) {
                QkBuf n = {0};
                qkTypeName(t, &n);
                qkError(c->src, a->body->line, a->body->col, "%s is a case, but the match is on %s", a->pattern, n.data);
                qkBufFree(&n);
                ok = false;
            } else if (bindPattern(c, a, t, a->body)) {
                if (onRecord) {
                    if (!a->guard && !refutable(a, t)) wildcard = true; /* Point(x, y) catches every point */
                } else if (a->caseIndex < 256 && !a->guard && !subsRefutable(a, t)) { /* a guarded arm may let its case through */
                    if (seen[a->caseIndex]) qkError(c->src, a->body->line, a->body->col, "%s is matched twice", a->pattern);
                    seen[a->caseIndex] = true;
                }
            } else {
                ok = false;
            }
        } else if (a->textPattern) {
            if (!onText) {
                typeError(c, a->textPattern, "a text pattern, but the match is on %s", t, NULL);
                ok = false;
            }
        } else if (t->kind != TY_INT) {
            Expr* at = a->intPattern ? a->intPattern : a->intHigh;
            typeError(c, at, "an Int pattern, but the match is on %s", t, NULL);
            ok = false;
        } else if (a->isRange && a->intPattern && a->intHigh && a->intPattern->i >= a->intHigh->i) {
            qkError(c->src, a->intPattern->line, a->intPattern->col, "this range is empty (its end is not included): write a smaller start");
        }
        if (ok && a->guard) {
            Type* g = checkExpr(c, a->guard, true);
            if (g->kind != TY_BOOL && g->kind != TY_ERROR) typeError(c, a->guard, "an arm's if takes a Bool, not %s", g, NULL);
        }
        if (ok) checkExpr(c, a->body, wantValue);
        c->scopeCount = mark;
    }
    if (!wildcard && (onRecord || t->kind == TY_ENUM)) { /* patterns inside patterns: is every value covered? */
        bool nested = onRecord;
        for (int i = 0; i < e->armCount; i++) nested |= e->arms[i].subs != NULL;
        if (nested) {
            Arm*** rows = qkNew(sizeof(Arm**) * (size_t)(e->armCount ? e->armCount : 1));
            int n = 0;
            bool fine = true;
            for (int i = 0; i < e->armCount; i++) {
                Arm* a = &e->arms[i];
                if (a->guard) continue;
                fine &= !a->pattern || a->caseIndex >= 0;
                rows[n] = qkNew(sizeof(Arm*));
                rows[n++][0] = a;
            }
            const Type* col = t;
            if (fine && covers(rows, n, &col, 1, 0)) wildcard = true;
            else if (fine) {
                qkError(c->src, e->line, e->col, "this match may miss some values: cover them, or add a last arm _ => ...");
                wildcard = true; /* said */
            }
        }
    }
    if (!wildcard) {
        if (onRecord) qkError(c->src, e->line, e->col, "this match may miss some values: add a last arm _ => ...");
        else if (t->kind == TY_INT) qkError(c->src, e->line, e->col, "a match on an Int needs a last arm _ => ...");
        else if (onText) qkError(c->src, e->line, e->col, "a match on a Text needs a last arm _ => ...");
        else
            for (int k = 0; k < t->decl->caseCount && k < 256; k++)
                if (!seen[k]) {
                    qkError(c->src, e->line, e->col, "this match misses %s.%s (or add _ => ...)", t->decl->name, t->decl->cases[k]);
                    break;
                }
    }
    if (!wantValue) return &qkTypeVoid;
    Expr*** slots = qkNew(sizeof(Expr**) * (size_t)(e->armCount ? e->armCount : 1));
    bool allVoid = true;
    for (int i = 0; i < e->armCount; i++) {
        slots[i] = branchSlot(&e->arms[i].body);
        if (slots[i] && (*slots[i])->type->kind != TY_VOID) allVoid = false;
    }
    if (allVoid) return &qkTypeVoid;
    for (int i = 0; i < e->armCount; i++)
        if (!slots[i]) {
            qkError(c->src, e->arms[i].body->line, e->arms[i].body->col, "every arm must give a value");
            return &qkTypeError;
        }
    c->result = result;
    Type* r = unifyBranches(c, slots, e->armCount, e);
    c->result = NULL;
    return r;
}

/* The binding a place (x, x[i], x.f, x[i].f) writes to, or NULL when it is not a place. */
static Binding* placeRoot(Expr* e, bool* direct) {
    *direct = true;
    if (e->kind == E_NAME) return e->binding;
    if (e->kind == E_FIELD) {
        Binding* b = placeRoot(e->a, direct);
        return b;
    }
    if (e->kind == E_INDEX) {
        if (e->a->kind != E_NAME) *direct = false;
        return e->a->kind == E_NAME ? e->a->binding : NULL;
    }
    return NULL;
}

/* Does a block always leave (return, break, continue, exit, panic)? Then what follows it never runs after it. */
static bool leaves(const Block* b) {
    if (!b->count) return false;
    const Stmt* s = b->stmts[b->count - 1];
    if (s->kind == S_RETURN || s->kind == S_BREAK || s->kind == S_CONTINUE) return true;
    if (s->kind == S_EXPR && s->value->type && s->value->type->kind == TY_NEVER) return true; /* exit, panic */
    if (s->kind == S_EXPR && s->value->kind == E_IF && s->value->elseExpr) {
        const Expr* e = s->value;
        bool all = leaves(e->thenBlock);
        while (all && e->elseExpr && e->elseExpr->kind == E_IF) {
            e = e->elseExpr;
            all = leaves(e->thenBlock) && e->elseExpr;
        }
        return all && e->elseExpr && e->elseExpr->kind == E_BLOCK && leaves(e->elseExpr->block);
    }
    return false;
}

/* let Some(x) = value else { return }: the names hold the case's data from here on */
static void checkLetElse(Checker* c, Stmt* s) {
    Type* t = checkExpr(c, s->value, true);
    Arm* a = s->pattern;
    bool ok = (t->kind == TY_ENUM || t->kind == TY_RECORD) && t != &qkTypeNone && t != &qkTypeErr;
    if (!ok && t->kind != TY_ERROR)
        typeError(c, s->value, "let Case(...) = value takes apart an enum, an optional, a result, a record or a tuple, not %s", t, NULL);
    if (s->elseBody) {
        checkBlock(c, s->elseBody, false);
        if (!leaves(s->elseBody))
            qkError(c->src, s->line, s->col, "the else of let %s(...) must leave: end it with return, break, continue or exit",
                    strcmp(a->pattern, "(,)") ? a->pattern : "");
    }
    if (ok && bindPattern(c, a, t, s->value)) {
        if (!s->elseBody && refutable(a, t))
            qkError(c->src, s->line, s->col, "this pattern may not match: say what happens then, let ... = value else { return }");
        return;
    }
    a->bindings = qkNew(sizeof(Binding*) * (size_t)(a->bindCount ? a->bindCount : 1)); /* unknown: no more errors */
    for (int f = 0; f < a->bindCount; f++)
        if (strcmp(a->binds[f], "_")) a->bindings[f] = declare(c, a->binds[f], &qkTypeError, false, s->line, s->col);
}

static void checkStmt(Checker* c, Stmt* s) {
    switch (s->kind) {
    case S_LET: case S_VAR: {
        if (s->pattern) {
            checkLetElse(c, s);
            break;
        }
        int readsBefore = c->varReads;
        if (s->typeAnn) c->hint = resolveType(c, s->typeAnn);
        Type* t = checkExpr(c, s->value, true);
        c->hint = NULL;
        if (s->typeAnn) {
            Type* want = resolveType(c, s->typeAnn);
            coerce(c, &s->value, want, s->name);
            t = want;
        }
        if (t->kind == TY_VOID) {
            qkError(c->src, s->value->line, s->value->col, "this gives nothing to keep in \"%s\"", s->name);
            t = &qkTypeError;
        }
        if (isPlaceholder(t)) {
            qkError(c->src, s->value->line, s->value->col, "%s needs its type written: %s %s: %s<...> = ...", t->decl->name,
                    s->kind == S_LET ? "let" : "var", s->name, t->decl->name);
            t = &qkTypeError;
        }
        if (t == &qkTypeNone || t == &qkTypeEmpty || t == &qkTypeErr || t == &qkTypeEmptyMap) {
            const char* what = t == &qkTypeNone ? "None" : t == &qkTypeEmpty ? "[]" : t == &qkTypeErr ? "Err(...)" : "[:]";
            const char* example = t == &qkTypeNone ? "Int?" : t == &qkTypeEmpty ? "[Int]" : t == &qkTypeErr ? "Int!" : "[Text: Int]";
            qkError(c->src, s->value->line, s->value->col, "%s needs its type written: %s %s: %s = ...", what, s->kind == S_LET ? "let" : "var",
                    s->name, example);
            t = &qkTypeError;
        }
        if (t == &lambdaType && s->value->kind == E_LAMBDA) { /* let f = (x: Int) => ...: its type from its parameters' */
            Expr* lam = s->value;
            bool typed = lam->paramCount < 16;
            for (int i = 0; i < lam->paramCount && typed; i++) typed = lam->params[i].type != NULL;
            if (typed) {
                Type* params[16];
                for (int i = 0; i < lam->paramCount; i++) params[i] = resolveType(c, lam->params[i].type);
                Type proto = {TY_FN};
                proto.params = params;
                proto.paramCount = lam->paramCount;
                t = liftLambdaAs(c, lam, &proto, true) ? lam->type : &qkTypeError; /* its effects: its body's */
            }
        }
        if (t == &lambdaType) {
            qkError(c->src, s->value->line, s->value->col, "a lambda kept in \"%s\" needs its type written: %s %s: (Int) -> Int = x => ...", s->name,
                    s->kind == S_LET ? "let" : "var", s->name);
            t = &qkTypeError;
        }
        bool loop = t->kind == TY_RANGE || t->kind == TY_ITER;
        if (loop && s->kind == S_VAR) {
            markIterUse(s->value);
            coerce(c, &s->value, qkTypeArray(t->elem), s->name);
            t = s->value->type;
        }
        Binding* b = declare(c, s->name, loop ? qkTypeArray(t->elem) : t, s->kind == S_VAR, s->line, s->col + 4); /* at the name */
        b->init = s->value;
        b->readsVar = c->varReads != readsBefore;
        s->binding = b;
        if (loop) QK_PUSH(c->lets, c->letCount, c->letCap, b);
        break;
    }
    case S_ASSIGN: {
        Type* target = checkExpr(c, s->target, true);
        if (s->target->kind == E_INDEX && s->target->b->kind == E_RANGE) {
            qkError(c->src, s->target->line, s->target->col, "a slice is a copy: set the elements one by one, xs[i] = ...");
            break;
        }
        if (s->target->kind == E_INDEX && s->target->a->type && s->target->a->type->kind == TY_MAP) { /* m[k] = v */
            target = s->target->a->type->elem;
            if (s->op != T_ASSIGN) {
                qkError(c->src, s->line, s->col, "m[key] gives an optional: write m[key] = (m[key] ?? 0) + 1");
                break;
            }
        }
        bool direct;
        Binding* root = placeRoot(s->target, &direct);
        if (!root) {
            qkError(c->src, s->target->line, s->target->col, "only a var, an element of a var array, or a field of a var can be set");
        } else if (!root->mutable) {
            qkError(c->src, s->target->line, s->target->col, "\"%s\" is a let (it never changes); declare it with var to change it", root->name);
        } else if (!direct) {
            qkError(c->src, s->target->line, s->target->col, "set an element through the array's own name: arr[i] = ...");
        }
        for (Expr* t = s->target; t->kind == E_FIELD || t->kind == E_INDEX; t = t->a) /* through a box: a new value */
            if (t->kind == E_FIELD && t->a->type && t->a->type->kind == TY_RECORD && t->a->type->decl->params[t->caseIndex].boxed &&
                (t != s->target || s->op != T_ASSIGN)) {
                qkError(c->src, t->line, t->col, "%s holds its own type: set it whole (x.%s = ...), or build a new one with with", t->name,
                        t->name);
                break;
            }
        Type* value = checkExpr(c, s->value, true);
        if (s->op != T_ASSIGN) {
            if (hasOperators(target)) { /* v += w: v = v + w, through the type's plus */
                Expr* op = qkNewExpr(E_BINARY, s->line, s->col);
                op->op = s->op == T_PLUSEQ ? T_PLUS : s->op == T_MINUSEQ ? T_MINUS : s->op == T_STAREQ ? T_STAR : T_SLASH;
                op->a = s->target;
                op->b = s->value;
                op->implicit = true;
                Type* r = op->type = checkOperator(c, op, false);
                if (r->kind != TY_ERROR && !qkTypeSame(r, target)) typeError(c, s->value, "the new value: expected %s, found %s", target, r);
                s->opValue = op;
                break;
            }
            if (!isNumeric(target)) typeError(c, s->target, "%s cannot be changed with +=, -=, *= or /=", target, NULL);
            if (target->kind == TY_INT && value->kind == TY_FLOAT) {
                typeError(c, s->value, "an Int changed by a Float: write toInt(...) or make it a Float", NULL, NULL);
                break;
            }
        }
        if (value->kind == TY_RANGE || value->kind == TY_ITER) markIterUse(s->value);
        coerce(c, &s->value, target, "the new value");
        break;
    }
    case S_EXPR: {
        Expr* e = s->value;
        checkExpr(c, e, false);
        if (e->kind != E_CALL && e->kind != E_IF && e->kind != E_MATCH && e->kind != E_BLOCK && e->kind != E_TRY && e->type->kind != TY_ERROR)
            qkError(c->src, e->line, e->col, "this value is not used: keep it with let, or remove it");
        break;
    }
    case S_FOR: {
        Type* t = checkExpr(c, s->iter, true);
        bool overMap = t->kind == TY_MAP && t->key; /* for key, value in m */
        if (!qkIsIterable(t) && !overMap) {
            if (t->kind != TY_ERROR) typeError(c, s->iter, "for goes over an array, a range, a loop or a map, not %s", t, NULL);
            t = &qkTypeError;
        }
        markIterUse(s->iter);
        int mark = c->scopeCount;
        c->loopDepth++;
        s->binding = declare(c, s->name, t->kind == TY_ERROR ? t : overMap ? t->key : t->elem, false, s->line, s->col + 4);
        /* for i in 0..len(xs), xs a let or a parameter: xs[i] needs no bounds check */
        Expr* it = s->iter;
        if (it->kind == E_RANGE && it->a->kind == E_INT && it->a->i >= 0 && it->b->kind == E_CALL && it->b->builtin == B_LEN &&
            it->b->argCount == 1) {
            Expr* arr = it->b->args[0];
            if (arr->kind == E_NAME && arr->binding && !arr->binding->mutable && arr->binding->type->kind == TY_ARRAY)
                s->binding->safeIn = arr->binding;
        }
        if (s->indexName) s->indexBinding = declare(c, s->indexName, overMap ? t->elem : &qkTypeInt, false, s->line, s->col + 6 + (int)strlen(s->name));
        checkBlock(c, s->body, false);
        c->loopDepth--;
        c->scopeCount = mark;
        break;
    }
    case S_WHILE: {
        c->loopDepth++;
        Type* t = checkExpr(c, s->value, true);
        if (t->kind != TY_BOOL && t->kind != TY_ERROR) typeError(c, s->value, "the condition must be a Bool, not %s", t, NULL);
        checkBlock(c, s->body, false);
        c->loopDepth--;
        break;
    }
    case S_RETURN: {
        if (c->lambdaDepth) {
            qkError(c->src, s->line, s->col, "return inside a lambda: a lambda is one expression");
            break;
        }
        Type* want = c->fn->retType;
        if (!want && !s->value && c->fn->body->kind == E_BLOCK) { /* return, alone: the function gives nothing */
            c->fn->retType = &qkTypeVoid;
            c->fn->returnsEarly = true;
            break;
        }
        if (!want) {
            qkError(c->src, s->line, s->col, "a function that returns early needs its type written: fn %s(...) -> Type", c->fn->name);
            if (s->value) checkExpr(c, s->value, true);
            break;
        }
        if (!s->value) {
            if (want->kind != TY_VOID) qkError(c->src, s->line, s->col, "%s must return a value", c->fn->name);
            break;
        }
        Type* t = checkExpr(c, s->value, true);
        if (t->kind == TY_RANGE || t->kind == TY_ITER) markIterUse(s->value);
        coerce(c, &s->value, want, "return");
        break;
    }
    case S_BREAK: case S_CONTINUE:
        if (c->lambdaDepth) qkError(c->src, s->line, s->col, "break and continue cannot leave a lambda");
        else if (!c->loopDepth) qkError(c->src, s->line, s->col, "%s outside a loop", s->kind == S_BREAK ? "break" : "continue");
        break;
    }
}

/* ---- expressions --------------------------------------------------------------------------------- */

static Type* checkBinary(Checker* c, Expr* e) {
    Type* a = checkExpr(c, e->a, true);
    Type* b = checkExpr(c, e->b, true);
    if (a->kind == TY_ERROR || b->kind == TY_ERROR) return &qkTypeError;
    if (e->op == T_QQ) { /* a ?? b: what is in a, or b */
        Type* inner = qkFallibleValue(a);
        if (!inner) {
            typeError(c, e->a, "?? takes an optional (T?) or a result (T!) on its left, not %s", a, NULL);
            return &qkTypeError;
        }
        if ((isOption(a) && (b == &qkTypeNone || isOption(b))) || (isResult(a) && (b == &qkTypeErr || isResult(b)))) {
            coerce(c, &e->b, a, "??"); /* a ?? b ?? c: still optional */
            return a;
        }
        coerce(c, &e->b, inner, "??");
        return inner;
    }
    if (a->kind == TY_TEXT && b->kind == TY_TEXT) { /* joined, compared byte by byte */
        if (e->op == T_PLUS) return &qkTypeText;
        if (e->op >= T_EQ && e->op <= T_GE) return &qkTypeBool;
        typeError(c, e, "texts are joined with + and compared, not %s and %s", a, b);
        return &qkTypeError;
    }
    if (e->op == T_PLUS && (a->kind == TY_ARRAY || b->kind == TY_ARRAY)) { /* xs + ys: a new array, one after the other */
        if (a == &qkTypeEmpty && b->kind == TY_ARRAY) coerce(c, &e->a, b, "+");
        else if (b == &qkTypeEmpty && a->kind == TY_ARRAY) coerce(c, &e->b, a, "+");
        else if (a->kind == TY_ARRAY && (b->kind == TY_RANGE || b->kind == TY_ITER)) markIterUse(e->b), coerce(c, &e->b, a, "+");
        else if (b->kind == TY_ARRAY && (a->kind == TY_RANGE || a->kind == TY_ITER)) markIterUse(e->a), coerce(c, &e->a, b, "+");
        else if (a->kind == TY_ARRAY && b->kind == TY_ARRAY && a != b && a->elem && b->elem) {
            typeError(c, e, "+ joins two arrays of one type, not %s and %s", a, b);
            return &qkTypeError;
        }
        if (e->a->type == &qkTypeEmpty) {
            qkError(c->src, e->line, e->col, "[] + []: write the type, let xs: [Int] = []");
            return &qkTypeError;
        }
        if (e->a->type->kind == TY_ARRAY && e->a->type == e->b->type) return e->a->type;
        typeError(c, e, "+ joins two arrays of one type, not %s and %s", e->a->type, e->b->type);
        return &qkTypeError;
    }
    if ((a->kind == TY_TEXT) != (b->kind == TY_TEXT) && e->op == T_PLUS) {
        typeError(c, e, "+ joins two texts: make the other one text with \"{x}\" or toText(x) (%s and %s)", a, b);
        return &qkTypeError;
    }
    if (e->op == T_AMP || e->op == T_PIPE || e->op == T_CARET || e->op == T_SHL || e->op == T_SHR) { /* an Int's bits */
        if ((a->kind != TY_INT && a->kind != TY_ERROR) || (b->kind != TY_INT && b->kind != TY_ERROR)) {
            typeError(c, e, "& | ^ << >> work on the bits of Ints, not %s and %s", a, b);
            return &qkTypeError;
        }
        return &qkTypeInt;
    }
    bool arithmetic = e->op == T_PLUS || e->op == T_MINUS || e->op == T_STAR || e->op == T_SLASH || e->op == T_PERCENT;
    if (arithmetic && hasOperators(a)) return checkOperator(c, e, false);
    if (e->op == T_STAR && isNumeric(a) && hasOperators(b)) return checkOperator(c, e, true); /* 2 * v is v * 2 */
    if (arithmetic && hasOperators(b)) {
        typeError(c, e, "put the %s first: the left value's type gives the operator", b, NULL);
        return &qkTypeError;
    }
    switch (e->op) {
    case T_AND: case T_OR:
        if (a->kind != TY_BOOL || b->kind != TY_BOOL) {
            typeError(c, e, "and / or join Bools, not %s and %s", a, b);
            return &qkTypeError;
        }
        return &qkTypeBool;
    case T_PLUS: case T_MINUS: case T_STAR: case T_SLASH: case T_PERCENT:
        if (isNumeric(a) && isNumeric(b)) {
            if (e->op == T_PERCENT && (a->kind != TY_INT || b->kind != TY_INT)) {
                typeError(c, e, "%% works on Ints, not %s and %s", a, b);
                return &qkTypeError;
            }
            return unifyNumeric(c, &e->a, &e->b);
        }
        typeError(c, e, "arithmetic needs numbers, not %s and %s", a, b);
        return &qkTypeError;
    case T_EQ: case T_NE:
        if (isNumeric(a) && isNumeric(b)) {
            unifyNumeric(c, &e->a, &e->b);
            return &qkTypeBool;
        }
        if (isPlaceholder(b) && !isPlaceholder(a) && coerce(c, &e->b, a, "==")) b = a; /* t == Tree.Leaf */
        if (isPlaceholder(a) && !isPlaceholder(b) && coerce(c, &e->a, b, "==")) a = b;
        if (a != b && qkFallibleValue(a) && !qkFallibleValue(b) && coerce(c, &e->b, a, "==")) b = a; /* x == None, x == 3 */
        else if (a != b && qkFallibleValue(b) && !qkFallibleValue(a) && coerce(c, &e->a, b, "==")) a = b;
        if (a == b && equatable(a)) return &qkTypeBool; /* records field by field, cases with their data */
        if (a == b && qkHasData(a)) {
            typeError(c, e, "%s carries data: compare it with match", a, NULL);
            return &qkTypeError;
        }
        if (a == b && a->kind == TY_RECORD) {
            typeError(c, e, "%s holds something == cannot compare (an array, or data a match takes apart)", a, NULL);
            return &qkTypeError;
        }
        typeError(c, e, "== compares numbers, texts, Bools, enum cases or records of one type, not %s and %s", a, b);
        return &qkTypeError;
    default: /* < <= > >= */
        if (isNumeric(a) && isNumeric(b)) {
            unifyNumeric(c, &e->a, &e->b);
            return &qkTypeBool;
        }
        if (a == b && a->kind == TY_BOOL) return &qkTypeBool;
        if (a == b && (a->kind == TY_RECORD || a->kind == TY_ENUM || a->kind == TY_HANDLE)) {
            Decl* d = typeFunction(c, a, "compare"); /* fn T.compare(a: T, b: T) -> Int: below 0, 0, above 0 */
            if (d && d->paramCount == 2 && d->isTemplate) { /* a generic type's compare: its instance, as a call would make */
                Expr* call = qkNew(sizeof *call);
                call->kind = E_CALL;
                call->line = e->line;
                call->col = e->col;
                call->name = d->name;
                call->args = qkNew(sizeof(Expr*) * 2);
                call->args[0] = e->a;
                call->args[1] = e->b;
                call->argNames = qkNew(sizeof(const char*) * 2);
                call->argCount = 2;
                e->a->typed = e->b->typed = true;
                Type* r = checkTemplateCall(c, call, d);
                if (r->kind == TY_ERROR) return r;
                e->decl = call->decl;
                e->ordered = call->ordered;
                e->orderedCount = 2;
                return &qkTypeBool;
            }
            if (d && d->paramCount == 2) {
                Type* r = fnReturnType(c, d, e);
                if (r->kind != TY_INT && r->kind != TY_ERROR) {
                    typeError(c, e, "%s.compare must give an Int (below 0, 0 or above 0), not %s", a, r);
                    return &qkTypeError;
                }
                usesEffectsOf(c, e, d, 0);
                Expr** ordered = qkNew(sizeof(Expr*) * 2);
                ordered[0] = e->a;
                ordered[1] = e->b;
                coerce(c, &ordered[1], d->params[1].binding->type, d->name);
                e->decl = d;
                e->ordered = ordered;
                e->orderedCount = 2;
                return &qkTypeBool;
            }
            if (a->kind == TY_ENUM && !qkHasData(a) && !qkFallibleValue(a)) return &qkTypeBool; /* in the order of its cases */
            typeError(c, e, "%s has no order: write fn %s.compare(a, b) -> Int", a, a);
            return &qkTypeError;
        }
        typeError(c, e, "< and > compare numbers, texts and cases, not %s and %s", a, b);
        return &qkTypeError;
    }
}

static Type* checkExprInner(Checker* c, Expr* e, bool wantValue) {
    switch (e->kind) {
    case E_INT: return &qkTypeInt;
    case E_FLOAT: return &qkTypeFloat;
    case E_TEXT: return &qkTypeText;
    case E_BOOL: return &qkTypeBool;
    case E_CASE: return e->decl->type;
    case E_NAME: {
        Binding* b = lookupOrCapture(c, e->name);
        if (b && b->fnParam && b->paramOf && b->paramOf->isTemplate) keepsFunction(b); /* kept: a value from now on */
        else if (b && (b->fnParam || b->lambda)) {
            qkError(c->src, e->line, e->col, "a function is not a value to keep here: %s was given to this function, so call it or pass it on",
                    e->name);
            return &qkTypeError;
        }
        if (b) {
            e->binding = b;
            b->uses++;
            if (b->mutable) c->varReads++;
            if (b->depth != c->loopDepth) b->iterUses += 1000; /* used deeper in loops: never fused */
            return b->type;
        }
        Decl* d = findDecl(c, e->name);
        if (d && d->kind == D_CONST) { /* a constant: its value, wherever it is used */
            e->decl = d;
            return constType(c, d, e);
        }
        if (!d && !strcmp(e->name, "None")) return &qkTypeNone; /* takes the T? it goes into */
        if (d && d->kind == D_FN) return fnValueType(c, (e->decl = d), e);
        else if (d) qkError(c->src, e->line, e->col, "%s is a type, not a value", e->name);
        else if (!strcmp(e->name, "Some")) qkError(c->src, e->line, e->col, "Some holds a value: Some(x)");
        else unknownName(c, e);
        return &qkTypeError;
    }
    case E_WITH: { /* a copy with some fields changed */
        Type* t = checkExpr(c, e->a, true);
        if (t->kind != TY_RECORD) {
            if (t->kind != TY_ERROR) typeError(c, e->a, "with changes fields of a record, not %s", t, NULL);
            return &qkTypeError;
        }
        for (int i = 0; i < e->argCount; i++) {
            checkExpr(c, e->args[i], true);
            const char* f = e->argNames[i];
            Type* ft = NULL;
            for (int k = 0; k < t->decl->paramCount; k++)
                if (!strcmp(t->decl->params[k].name, f)) ft = t->decl->params[k].binding->type;
            if (!ft) {
                QkBuf n = {0};
                qkTypeName(t, &n);
                qkError(c->src, e->args[i]->line, e->args[i]->col, "%s has no field \"%s\"", n.data, f);
                qkBufFree(&n);
                return &qkTypeError;
            }
            for (int j = 0; j < i; j++)
                if (!strcmp(e->argNames[j], f)) qkError(c->src, e->args[i]->line, e->args[i]->col, "\"%s\" is given twice", f);
            if (e->args[i]->type->kind == TY_RANGE || e->args[i]->type->kind == TY_ITER) markIterUse(e->args[i]);
            coerce(c, &e->args[i], ft, f);
        }
        return t;
    }
    case E_INTERP: /* "lap {n}": each value as text */
        for (int i = 0; i < e->argCount; i++) {
            Type* t = checkExpr(c, e->args[i], true);
            if (!textable(t)) typeError(c, e->args[i], "%s has no text form (Int, Float, Bool, Text and enum cases do)", t, NULL);
            const char* f = e->formats ? e->formats[i] : NULL;
            if (f && strchr(f, '.')) { /* {x:.2}: decimals, for a number */
                const char* dot = strchr(f, '.');
                bool fine = dot[1] && !strchr(dot + 1, '.');
                if (!fine) qkError(c->src, e->args[i]->line, e->args[i]->col, "a format is a width, decimals or both: {x:8}, {x:.2}, {x:8.2}");
                else if (!isNumeric(t) && t->kind != TY_ERROR) typeError(c, e->args[i], "decimals ({x:.2}) are for numbers, not %s", t, NULL);
            }
        }
        return &qkTypeText;
    case E_TRY: { /* a?: what it holds, or leave with its None / Err */
        Type* t = checkExpr(c, e->a, true);
        if (t->kind == TY_ERROR) return t;
        Type* v = qkFallibleValue(t);
        if (!v) {
            typeError(c, e->a, "? takes an optional (T?) or a result (T!), not %s", t, NULL);
            return &qkTypeError;
        }
        if (c->lambdaDepth) {
            qkError(c->src, e->line, e->col, "? cannot leave a lambda (a lambda is one expression): use ?? or match");
        } else if (c->inConst || !c->fn) {
            qkError(c->src, e->line, e->col, "? cannot leave a constant: use ?? or match");
        } else if (strcmp(c->fn->name, "main") && !c->fn->isTest) { /* in main or a test: it stops with the error */
            Type* want = c->fn->retType;
            if (!want) {
                qkError(c->src, e->line, e->col, "%s uses ?, so it may return early: write its result type, fn %s(...) -> Type", c->fn->name,
                        c->fn->name);
            } else if (want->kind != TY_ERROR && isOption(t) && !isOption(want)) {
                typeError(c, e, "? passes a None on, so the function must give an optional (T?), not %s", want, NULL);
            } else if (want->kind != TY_ERROR && isResult(t) && !isResult(want)) {
                typeError(c, e, "? passes an Err on, so the function must give a result (T!), not %s", want, NULL);
            } else if (want->kind != TY_ERROR && isResult(t) && t->decl->errorOf != want->decl->errorOf) {
                typeError(c, e, "? passes an Err of %s on, but the function's Err holds %s: take it apart with match", t->decl->errorOf,
                          want->decl->errorOf);
            }
        }
        return v;
    }
    case E_MAP: { /* [key: value, ...] */
        if (!e->argCount) return &qkTypeEmptyMap; /* takes the map type it goes into */
        Type *k = NULL, *v = NULL;
        bool anyFloat = false;
        for (int i = 0; i + 1 < e->argCount; i += 2) {
            Type* kt = checkExpr(c, e->args[i], true);
            Type* vt = checkExpr(c, e->args[i + 1], true);
            anyFloat |= vt->kind == TY_FLOAT;
            if (!k) k = kt;
            if (!v) v = vt;
        }
        if (anyFloat && isNumeric(v)) v = &qkTypeFloat;
        if (v->kind == TY_RANGE || v->kind == TY_ITER) v = qkTypeArray(v->elem);
        if (k->kind == TY_ERROR || v->kind == TY_ERROR) return &qkTypeError;
        if (!keyable(k)) {
            typeError(c, e->args[0], "a map's key is an Int, a Text, a Bool, an enum's case or a record of two Ints, not %s", k, NULL);
            return &qkTypeError;
        }
        for (int i = 0; i + 1 < e->argCount; i += 2) {
            coerce(c, &e->args[i], k, "a key");
            if (e->args[i + 1]->type->kind == TY_RANGE || e->args[i + 1]->type->kind == TY_ITER) markIterUse(e->args[i + 1]);
            coerce(c, &e->args[i + 1], v, "a value");
        }
        return qkTypeMap(k, v);
    }
    case E_FIELD: {
        if (e->a->kind == E_NAME && typeLike(e->a->name)) {
            Decl* d = findDecl(c, e->a->name);
            if (d && d->kind == D_ENUM) {
                for (int k = 0; k < d->caseCount; k++)
                    if (!strcmp(d->cases[k], e->name)) {
                        if (d->caseFieldCount && d->caseFieldCount[k]) {
                            qkError(c->src, e->line, e->col, "%s.%s carries data: %s.%s(...)", d->name, e->name, d->name, e->name);
                            return &qkTypeError;
                        }
                        e->kind = E_CASE;
                        e->decl = d;
                        e->caseIndex = k;
                        return d->type;
                    }
                const char* close = qkClosest(e->name, d->cases, d->caseCount);
                qkError(c->src, e->line, e->col, "%s has no case %s%s%s%s", d->name, e->name, close ? ": did you mean " : "", close ? close : "", close ? "?" : "");
                return &qkTypeError;
            }
        }
        Type* t = checkExpr(c, e->a, true);
        if (t->kind == TY_RECORD) {
            Decl* d = t->decl;
            for (int f = 0; f < d->paramCount; f++)
                if (!strcmp(d->params[f].name, e->name)) {
                    e->caseIndex = f;
                    return d->params[f].binding->type;
                }
            const char* names[64];
            int n = 0;
            for (int f = 0; f < d->paramCount && n < 64; f++) names[n++] = d->params[f].name;
            const char* close = qkClosest(e->name, names, n);
            qkError(c->src, e->line, e->col, "%s has no field \"%s\"%s%s%s", d->name, e->name, close ? ": did you mean \"" : "", close ? close : "", close ? "\"?" : "");
            return &qkTypeError;
        }
        if (qkIsIterable(t) && !strcmp(e->name, "len")) {
            qkError(c->src, e->line, e->col, "the length is a function: len(xs) or xs.len()");
            return &qkTypeError;
        }
        typeError(c, e, "%s has no fields", t, NULL);
        return &qkTypeError;
    }
    case E_INDEX: {
        Type* t = checkExpr(c, e->a, true);
        if (t->kind == TY_RANGE || t->kind == TY_ITER) {
            markIterUse(e->a);
            coerce(c, &e->a, qkTypeArray(t->elem), "indexing");
            t = e->a->type;
        }
        if (e->b->kind == E_RANGE) { /* xs[a..b], t[a..]: a slice (b not included) */
            Expr* r = e->b;
            for (int k = 0; k < 2; k++) {
                Expr** end = k ? &r->b : &r->a;
                if (!*end) continue;
                checkExpr(c, *end, true);
                coerce(c, end, &qkTypeInt, "a slice's end");
            }
            r->type = &qkTypeRange;
            if (t->kind == TY_ARRAY || t->kind == TY_TEXT) return t;
            if (t->kind != TY_ERROR) typeError(c, e->a, "only arrays and texts are sliced, not %s", t, NULL);
            return &qkTypeError;
        }
        Type* i = checkExpr(c, e->b, true);
        if (t->kind == TY_MAP && t->key) { /* m[key]: the value, or None */
            coerce(c, &e->b, t->key, "the key");
            return qkTypeOption(t->elem);
        }
        if (t->kind == TY_TEXT) {
            qkError(c->src, e->line, e->col, "a text is sliced, not indexed: t[i..i + 1] is the piece at i");
            return &qkTypeError;
        }
        if (i->kind != TY_INT && i->kind != TY_ERROR) typeError(c, e->b, "an index is an Int, not %s", i, NULL);
        if (t->kind != TY_ARRAY) {
            if (t->kind != TY_ERROR) typeError(c, e->a, "only arrays are indexed, not %s", t, NULL);
            return &qkTypeError;
        }
        return t->elem;
    }
    case E_UNARY: {
        Type* t = checkExpr(c, e->a, true);
        if (e->op == T_NOT) {
            if (t->kind != TY_BOOL && t->kind != TY_ERROR) typeError(c, e->a, "not takes a Bool, not %s", t, NULL);
            return &qkTypeBool;
        }
        if (e->op == T_TILDE) {
            if (t->kind != TY_INT && t->kind != TY_ERROR) typeError(c, e->a, "~ takes an Int (its bits), not %s", t, NULL);
            return &qkTypeInt;
        }
        if (hasOperators(t)) return checkOperator(c, e, false);
        if (!isNumeric(t) && t->kind != TY_ERROR) typeError(c, e->a, "- takes a number, not %s", t, NULL);
        return t;
    }
    case E_BINARY: return checkBinary(c, e);
    case E_RANGE: {
        if (!e->a || !e->b) {
            qkError(c->src, e->line, e->col, "a range needs both ends here (a..b); xs[a..] and xs[..b] are slices");
            return &qkTypeError;
        }
        checkExpr(c, e->a, true);
        checkExpr(c, e->b, true);
        coerce(c, &e->a, &qkTypeInt, "a range's start");
        coerce(c, &e->b, &qkTypeInt, "a range's end");
        return &qkTypeRange;
    }
    case E_IF: return checkIf(c, e, wantValue);
    case E_MATCH: return checkMatch(c, e, wantValue);
    case E_BLOCK: return checkBlock(c, e->block, wantValue);
    case E_ARRAY: {
        Type* hint = c->hint;
        c->hint = NULL;
        if (!e->argCount) return &qkTypeEmpty; /* takes the array type it goes into */
        if (hint && hint->kind == TY_ARRAY && hint->elem) { /* let pets: [Speaker] = [Dog(...), Cat(...)] */
            for (int i = 0; i < e->argCount; i++) {
                Type* et = checkExpr(c, e->args[i], true);
                if (et->kind == TY_RANGE || et->kind == TY_ITER) markIterUse(e->args[i]);
                coerce(c, &e->args[i], hint->elem, "an array's element");
            }
            return hint;
        }
        Type* t = NULL;
        bool anyFloat = false;
        for (int i = 0; i < e->argCount; i++) {
            Type* et = checkExpr(c, e->args[i], true);
            anyFloat |= et->kind == TY_FLOAT;
            if (!t) t = et;
        }
        if (anyFloat && isNumeric(t)) t = &qkTypeFloat;
        if (t->kind == TY_RANGE || t->kind == TY_ITER) t = qkTypeArray(t->elem);
        for (int i = 0; i < e->argCount; i++) {
            if (e->args[i]->type->kind == TY_RANGE || e->args[i]->type->kind == TY_ITER) markIterUse(e->args[i]);
            coerce(c, &e->args[i], t, "an array's element");
        }
        return qkTypeArray(t);
    }
    case E_LAMBDA: return &lambdaType; /* it takes the function type it goes into */
    case E_CALL: return checkCall(c, e);
    }
    return &qkTypeError;
}

static Type* checkExpr(Checker* c, Expr* e, bool wantValue) {
    if (e->type && (e->implicit || e->typed)) return e->type;
    e->type = checkExprInner(c, e, wantValue);
    return e->type;
}

/* ---- functions ----------------------------------------------------------------------------------- */

static void checkFnBody(Checker* c, Decl* d);

static Type* fnReturnType(Checker* c, Decl* d, const Expr* at) {
    if (d->retType) return d->retType;
    if (d->state == 1) {
        qkError(c->src, at->line, at->col, "%s calls itself: write its result type, fn %s(...) -> Type", d->name, d->name);
        return &qkTypeError;
    }
    checkNow(c, d); /* infer it now, in a checker of its own */
    return d->retType ? d->retType : &qkTypeError;
}

static void decideFusion(Checker* c) {
    for (int i = 0; i < c->letCount; i++) {
        Binding* b = c->lets[i];
        b->lazy = !b->mutable && b->uses == 1 && b->iterUses == 1 && !b->readsVar;
        if (!b->lazy) { /* it is stored: an array */
            Expr** slot = &b->init;
            (void)slot;
        }
    }
}

static void checkFnBody(Checker* c, Decl* d) {
    if (d->state == 2) return;
    d->state = 1;
    enterDecl(c, d);
    c->fn = d;
    c->typing = d;
    c->loopDepth = 0;
    c->lambdaDepth = 0;
    int mark = c->scopeCount;
    for (int i = 0; i < d->paramCount; i++) {
        Binding* b = d->params[i].binding;
        QK_PUSH(c->scope, c->scopeCount, c->scopeCap, b);
    }
    if (d->ret) d->retType = resolveType(c, d->ret);
    Type* t;
    if (d->body->kind == E_BLOCK) {
        bool wantValue = !d->retType || d->retType->kind != TY_VOID;
        t = checkBlock(c, d->body->block, wantValue);
        d->body->type = t;
        Expr** result = blockResult(d->body);
        if (d->retType && d->retType->kind != TY_VOID) {
            Stmt* last = d->body->block->count ? d->body->block->stmts[d->body->block->count - 1] : NULL;
            if (result) {
                if ((*result)->type->kind == TY_RANGE || (*result)->type->kind == TY_ITER) markIterUse(*result);
                coerce(c, result, d->retType, "the result");
            } else if (!last || last->kind != S_RETURN) {
                qkError(c->src, d->line, d->col, "%s must end with its result (or a return)", d->name);
            }
        } else if (d->returnsEarly && t->kind != TY_VOID && t->kind != TY_ERROR && result) {
            qkError(c->src, (*result)->line, (*result)->col, "%s returns nothing early, so it gives nothing: write its type to give a value, fn %s(...) -> Type",
                    d->name, d->name);
        } else if (!d->retType) {
            if (t->kind == TY_RANGE || t->kind == TY_ITER) {
                markIterUse(*result);
                coerce(c, result, qkTypeArray(t->elem), "the result");
                t = qkTypeArray(t->elem);
            }
            d->retType = t;
        }
    } else {
        if (d->body->kind == E_IF || d->body->kind == E_MATCH) c->result = d->retType; /* fn f() -> Int!E = if ... { Err(E.A) } */
        t = checkExpr(c, d->body, true);
        c->result = NULL;
        if (t->kind == TY_RANGE || t->kind == TY_ITER) {
            markIterUse(d->body);
            Type* arr = qkTypeArray(t->elem);
            coerce(c, &d->body, d->retType ? d->retType : arr, "the result");
            t = arr;
        } else if (d->retType) {
            coerce(c, &d->body, d->retType, "the result");
        }
        if (!d->retType) d->retType = t;
    }
    decideFusion(c);
    c->scopeCount = mark;
    d->state = 2;
}

/* Does a value of type t hold one of `target` in its own slots (not behind an array, a map or a box)? */
static bool holdsByValue(const Type* t, const Type* target, const Decl** seen, int seenCount) {
    if (t == target) return true;
    if ((t->kind != TY_RECORD && t->kind != TY_ENUM) || !t->decl || seenCount >= 64) return false;
    const Decl* d = t->decl;
    for (int i = 0; i < seenCount; i++)
        if (seen[i] == d) return false; /* a loop that does not go through target */
    seen[seenCount++] = d;
    if (t->kind == TY_RECORD) {
        for (int f = 0; f < d->paramCount; f++) {
            const Param* prm = &d->params[f];
            if (!prm->boxed && prm->binding && holdsByValue(prm->binding->type, target, seen, seenCount)) return true;
        }
        return false;
    }
    for (int k = 0; d->caseFieldCount && k < d->caseCount; k++)
        for (int f = 0; f < d->caseFieldCount[k]; f++) {
            const Param* prm = &d->caseFields[k][f];
            if (!prm->boxed && prm->binding && holdsByValue(prm->binding->type, target, seen, seenCount)) return true;
        }
    return false;
}

static void boxIfRecursive(Param* prm, const Type* owner) {
    const Decl* seen[64];
    if (prm->binding && holdsByValue(prm->binding->type, owner, seen, 0)) prm->boxed = true;
}

void qkAddPrelude(Program* p, const Program* prelude) {
    int n = p->count + prelude->count;
    Decl** decls = qkNew(sizeof(Decl*) * (size_t)(n ? n : 1));
    for (int i = 0; i < prelude->count; i++) {
        decls[i] = prelude->decls[i];
        decls[i]->fromPrelude = true;
    }
    for (int i = 0; i < p->count; i++) decls[prelude->count + i] = p->decls[i];
    for (int i = 0; i < n; i++) decls[i]->index = i;
    p->decls = decls;
    p->count = p->cap = n;
}

bool qkCheck(Program* p, bool needMain) {
    int errorsBefore = qkErrorCount;
    effectUses = NULL; /* the arena it was in may be gone */
    effectUseCount = effectUseCap = 0;
    Checker c = {0};
    c.prog = p;
    c.src = &p->src;
    /* fn Vec2.length(self): self is a Vec2; fn Pair.swap(self) works on a Pair<A, B>, and takes its A and B */
    for (int i = 0; i < p->count; i++) {
        Decl* d = p->decls[i];
        const char* dot = d->kind == D_FN && !d->isTest ? strchr(d->name, '.') : NULL;
        if (!dot || !d->paramCount || d->params[0].type) continue;
        const char* name = qkDup(d->name, (size_t)(dot - d->name));
        Decl* owner = NULL;
        for (int k = 0; k < p->count && !owner; k++) {
            Decl* o = p->decls[k];
            if ((o->kind == D_TYPE || o->kind == D_ENUM) && !o->templateOf && !strcmp(o->name, name) &&
                (o->module == d->module || o->isPub || o->fromPrelude))
                owner = o;
        }
        TypeExpr* te = qkNew(sizeof *te);
        te->name = name;
        te->line = d->params[0].line;
        te->col = d->params[0].col;
        if (owner && owner->typeParamCount) {
            te->argCount = owner->typeParamCount;
            te->args = qkNew(sizeof(TypeExpr*) * (size_t)te->argCount);
            for (int k = 0; k < te->argCount; k++) {
                te->args[k] = qkNew(sizeof(TypeExpr));
                te->args[k]->name = owner->typeParams[k];
                te->args[k]->line = te->line;
                te->args[k]->col = te->col;
            }
            if (!d->typeParamCount) {
                d->typeParams = owner->typeParams;
                d->typeParamCount = owner->typeParamCount;
                d->impliedTypes = true;
            }
        }
        d->params[0].type = te;
        d->impliedSelf = true;
    }
    /* names: one meaning each */
    for (int i = 0; i < p->count; i++) {
        Decl* d = p->decls[i];
        enterDecl(&c, d);
        if (d->kind == D_USE) {
            if (!d->isPub && d->module == 0 && p->moduleCount == 0)
                qkError(c.src, d->line, d->col, "use %s: this program was not given its files (quark reads them next to it)", d->name);
            continue;
        }
        if (d->kind == D_FN && !d->isExtern && !d->templateOf) { /* generic, or takes a function: a template */
            bool takesFn = false;
            for (int f = 0; f < d->paramCount; f++) takesFn |= d->params[f].type->isFn;
            if ((d->typeParamCount || takesFn) && !d->isTemplate) {
                d->isTemplate = true;
                d->pristine = qkCloneDecl(d);
                d->typeArgs = qkNew(sizeof(Type*) * (size_t)(d->typeParamCount ? d->typeParamCount : 1));
                for (int k = 0; k < d->typeParamCount; k++) { /* while it is checked: T is any type */
                    Type* v = qkNew(sizeof(Type));
                    v->kind = TY_ERROR;
                    v->typeVar = d->typeParams[k];
                    d->typeArgs[k] = v;
                }
            }
        }
        if (d->kind == D_FN && d->isExtern) {
            for (int f = 0; f < d->paramCount; f++)
                if (d->params[f].type->isFn) qkError(c.src, d->params[f].line, d->params[f].col, "a host function takes values, not functions");
            if (d->typeParamCount) qkError(c.src, d->line, d->col, "a host function has no type parameters");
        }
        const char* dot = d->kind == D_FN && !d->isTest ? strchr(d->name, '.') : NULL;
        if (dot) d->owner = qkDup(d->name, (size_t)(dot - d->name)); /* fn Vec2.plus belongs to Vec2 */
        for (int j = 0; j < i; j++) { /* one meaning for each name a file sees */
            const Decl* o = p->decls[j];
            if (o->kind == D_USE || strcmp(o->name, d->name)) continue;
            bool clash = o->module == d->module || o->fromPrelude || d->fromPrelude || (o->isPub && usesModule(p, d->module, o->module)) ||
                         (d->isPub && usesModule(p, o->module, d->module));
            if (clash && o->module == d->module) qkError(c.src, d->line, d->col, "\"%s\" is declared twice", d->name);
            else if (clash) qkError(c.src, d->line, d->col, "\"%s\" is declared here and in %s: give one of them another name", d->name,
                                    p->modules[o->module ? o->module : d->module].path);
        }
        if (builtinNamed(d->name) && builtinNamed(d->name) < B_PARALLELMAP) /* the newer ones a program's own may hide */
            qkError(c.src, d->line, d->col, "\"%s\" is a built-in name", d->name);
        if (d->kind == D_TYPE || d->kind == D_ENUM) {
            d->type = qkNew(sizeof(Type));
            d->type->kind = d->kind == D_ENUM ? TY_ENUM : d->isExtern ? TY_HANDLE : TY_RECORD;
            d->type->decl = d;
            if (d->typeParamCount && !d->templateOf) { /* generic: an instance for each list of types it is given */
                d->isTemplate = true;
                d->pristine = qkCloneDecl(d);
            }
        }
        if ((d->kind == D_TYPE || d->kind == D_ENUM) && (!strcmp(d->name, "Some") || !strcmp(d->name, "None")))
            qkError(c.src, d->line, d->col, "%s is the built-in optional's case: pick another name", d->name);
    }
    /* record fields, enum cases' data and function parameters */
    for (int i = 0; i < p->count; i++) {
        Decl* d = p->decls[i];
        enterDecl(&c, d);
        if (d->kind == D_USE || d->templateOf || (d->isTemplate && d->kind != D_FN)) continue; /* instances: resolved when made */
        if (d->kind == D_ENUM) {
            for (int k = 0; k < d->caseCount; k++) {
                for (int j = 0; j < k; j++)
                    if (!strcmp(d->cases[j], d->cases[k])) qkError(c.src, d->line, d->col, "%s.%s is listed twice", d->name, d->cases[k]);
                for (int f = 0; d->caseFieldCount && f < d->caseFieldCount[k]; f++) {
                    Param* prm = &d->caseFields[k][f];
                    Binding* b = qkNew(sizeof *b);
                    b->name = prm->name;
                    b->type = resolveType(&c, prm->type);
                    prm->binding = b;
                }
            }
            continue;
        }
        if (d->kind == D_CONST) continue;
        c.typing = d; /* fn first<T>(xs: [T]): T is its own */
        for (int f = 0; f < d->paramCount; f++) {
            Param* prm = &d->params[f];
            for (int j = 0; j < f; j++)
                if (!strcmp(d->params[j].name, prm->name)) qkError(c.src, prm->line, prm->col, "\"%s\" is listed twice", prm->name);
            Binding* b = qkNew(sizeof *b);
            b->name = prm->name;
            b->type = resolveType(&c, prm->type);
            b->isParam = true;
            b->line = prm->line;
            b->col = prm->col;
            b->fnParam = d->kind == D_FN && b->type->kind == TY_FN;
            b->paramOf = d;
            b->paramIndex = f;
            prm->binding = b;
            if (b->type == d->type) qkError(c.src, prm->line, prm->col, "%s cannot hold itself", d->name);
            if (d->isExtern && (b->type->kind == TY_RANGE || b->type->kind == TY_ITER))
                qkError(c.src, prm->line, prm->col, "a host function takes values, not loops");
        }
        if (d->owner) { /* fn Vec2.plus(a: Vec2, ...): a function of a type, which its first value is */
            Decl* t = findDecl(&c, d->owner);
            if (!t || t->kind == D_FN || t->kind == D_CONST) {
                qkError(c.src, d->line, d->col, "%s is not a type of this program: a function belongs to a record, an enum or an extern type", d->owner);
            } else if (!d->paramCount || strcmp(d->params[0].name, "self")) {
                qkError(c.src, d->line, d->col, "a type's function works on self, its first value: fn %s(self%s)", d->name,
                        d->paramCount > 1 ? ", ..." : "");
            } else if (d->params[0].binding->type != t->type && d->params[0].binding->type->kind != TY_ERROR &&
                       d->params[0].binding->type->decl != t &&
                       (!d->params[0].binding->type->decl || d->params[0].binding->type->decl->templateOf != t)) {
                qkError(c.src, d->line, d->col, "%s's self is the %s it works on: fn %s(self, ...)", d->name, d->owner, d->name);
            }
        }
        if (d->kind == D_FN && d->isExtern) { /* the host's: its signature is all there is */
            d->retType = d->ret ? resolveType(&c, d->ret) : &qkTypeVoid;
            d->state = 2;
        }
        c.typing = NULL;
    }
    if (qkErrorCount > errorsBefore) return false;
    /* a field whose type holds its own type (enum Expr { Add(a: Expr, b: Expr) }, type Node { next: Node? }) is
     * boxed: its value lives in the arena, and the field keeps its address */
    for (int i = 0; i < p->count; i++) {
        Decl* d = p->decls[i];
        if (d->isTemplate && d->kind != D_FN) continue;
        if (d->kind == D_TYPE && !d->isExtern)
            for (int f = 0; f < d->paramCount; f++) boxIfRecursive(&d->params[f], d->type);
        if (d->kind == D_ENUM)
            for (int k = 0; d->caseFieldCount && k < d->caseCount; k++)
                for (int f = 0; f < d->caseFieldCount[k]; f++) boxIfRecursive(&d->caseFields[k][f], d->type);
    }
    /* constants and default values: pure, checked once */
    for (int i = 0; i < p->count; i++) {
        Decl* d = p->decls[i];
        enterDecl(&c, d);
        if (d->kind == D_CONST) {
            constType(&c, d, d->body);
            qkFoldConst(d, c.src); /* its value now, once */
        }
        if (d->kind != D_FN) continue;
        for (int f = 0; f < d->paramCount; f++)
            if (d->params[f].fallback)
                checkPure(&c, &d->params[f].fallback, NULL, d->params[f].binding->type, d->params[f].name);
    }
    if (qkErrorCount > errorsBefore) return false;
    for (int i = 0; i < p->count; i++)
        if (p->decls[i]->kind == D_FN && !p->decls[i]->isExtern) {
            c.scopeCount = 0;
            c.letCount = 0;
            checkFnBody(&c, p->decls[i]);
        }
    settleEffects();
    c.module = 0;
    c.src = &p->src;
    Decl* main = NULL;
    for (int i = 0; i < p->count && !main; i++)
        if (p->decls[i]->module == 0 && !strcmp(p->decls[i]->name, "main")) main = p->decls[i];
    if (!main && !needMain) return qkErrorCount == errorsBefore;
    if (!main || main->kind != D_FN) qkError(c.src, 1, 1, "a program starts at fn main() { ... }");
    else if (main->paramCount || (main->retType && main->retType->kind != TY_VOID && main->retType->kind != TY_NEVER))
        qkError(c.src, main->line, main->col, "main takes nothing and gives nothing: fn main() { ... }");
    return qkErrorCount == errorsBefore;
}
