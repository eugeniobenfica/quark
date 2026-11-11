/* Quark — types: every expression gets one, inferred from its parts; only function parameters (and record
 * fields) are written. An Int goes where a Float is wanted; a loop (map, filter, a range) goes where an array
 * is wanted. A let holding a loop that is used once, in the same loop depth, is fused into that use. */
#include "quark.h"

#include <stdio.h>
#include <stdlib.h>

Type qkTypeError = {TY_ERROR}, qkTypeVoid = {TY_VOID}, qkTypeInt = {TY_INT}, qkTypeFloat = {TY_FLOAT},
     qkTypeBool = {TY_BOOL}, qkTypeText = {TY_TEXT}, qkTypeRange = {TY_RANGE, 0, &qkTypeInt};

static Type vecTypes[3] = {{TY_VEC, 2}, {TY_VEC, 3}, {TY_VEC, 4}};

Type* qkTypeVec(int n) { return &vecTypes[n - 2]; }

typedef struct Interned {
    Type* elem;
    Type* array;
    Type* iter;
    struct Interned* next;
} Interned;
static Interned* interned;

static Interned* internFor(Type* elem) {
    for (Interned* i = interned; i; i = i->next)
        if (i->elem == elem) return i;
    Interned* i = qkNew(sizeof *i);
    i->elem = elem;
    i->array = qkNew(sizeof(Type));
    *i->array = (Type){TY_ARRAY, 0, elem, NULL};
    i->iter = qkNew(sizeof(Type));
    *i->iter = (Type){TY_ITER, 0, elem, NULL};
    i->next = interned;
    interned = i;
    return i;
}

Type* qkTypeArray(Type* elem) { return internFor(elem)->array; }
Type* qkTypeIter(Type* elem) { return internFor(elem)->iter; }

bool qkTypeSame(const Type* a, const Type* b) { return a == b || a->kind == TY_ERROR || b->kind == TY_ERROR; }

void qkTypeName(const Type* t, QkBuf* out) {
    switch (t->kind) {
    case TY_ERROR: qkBufPuts(out, "?"); break;
    case TY_VOID: qkBufPuts(out, "nothing"); break;
    case TY_INT: qkBufPuts(out, "Int"); break;
    case TY_FLOAT: qkBufPuts(out, "Float"); break;
    case TY_BOOL: qkBufPuts(out, "Bool"); break;
    case TY_TEXT: qkBufPuts(out, "Text"); break;
    case TY_VEC: qkBufPrintf(out, "Vec%d", t->n); break;
    case TY_ARRAY: qkBufPuts(out, "["); qkTypeName(t->elem, out); qkBufPuts(out, "]"); break;
    case TY_RECORD: case TY_ENUM: qkBufPuts(out, t->decl->name); break;
    case TY_RANGE: qkBufPuts(out, "a range of Int"); break;
    case TY_ITER: qkBufPuts(out, "a loop of "); qkTypeName(t->elem, out); break;
    }
}

int qkTypeSlots(const Type* t) {
    switch (t->kind) {
    case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_ENUM: return 1;
    case TY_TEXT: case TY_ARRAY: return 2;
    case TY_VEC: return t->n;
    case TY_RECORD: {
        int n = 0;
        for (int i = 0; i < t->decl->paramCount; i++) n += qkTypeSlots(t->decl->params[i].binding->type);
        return n;
    }
    default: return 0;
    }
}

void qkTypeSlotKinds(const Type* t, char* out) {
    switch (t->kind) {
    case TY_FLOAT: out[0] = 'F'; break;
    case TY_VEC: for (int i = 0; i < t->n; i++) out[i] = 'F'; break;
    case TY_RECORD: {
        int at = 0;
        for (int i = 0; i < t->decl->paramCount; i++) {
            Type* ft = t->decl->params[i].binding->type;
            qkTypeSlotKinds(ft, out + at);
            at += qkTypeSlots(ft);
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
    "round", "toFloat", "toInt", "min", "max", "clamp", "lerp", "length", "dot", "len", "fill", "map", "filter", "fold",
    "sum", "histogram", "any", "all", "toArray", "print", "now", "Vec2", "Vec3", "Vec4"};

/* ---- the checker --------------------------------------------------------------------------------- */

typedef struct Checker {
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
} Checker;

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

static Type* resolveType(Checker* c, TypeExpr* te) {
    if (!te->name) return qkTypeArray(resolveType(c, te->elem));
    if (!strcmp(te->name, "Int")) return &qkTypeInt;
    if (!strcmp(te->name, "Float")) return &qkTypeFloat;
    if (!strcmp(te->name, "Bool")) return &qkTypeBool;
    if (!strcmp(te->name, "Text")) return &qkTypeText;
    if (!strcmp(te->name, "Vec2")) return qkTypeVec(2);
    if (!strcmp(te->name, "Vec3")) return qkTypeVec(3);
    if (!strcmp(te->name, "Vec4")) return qkTypeVec(4);
    Decl* d = qkFindDecl(c->prog, te->name);
    if (d && d->kind != D_FN) return d->type;
    qkError(c->src, te->line, te->col, "unknown type \"%s\"", te->name);
    return &qkTypeError;
}

Decl* qkFindDecl(Program* p, const char* name) {
    for (int i = 0; i < p->count; i++)
        if (!strcmp(p->decls[i]->name, name)) return p->decls[i];
    return NULL;
}

static Binding* lookup(Checker* c, const char* name) {
    for (int i = c->scopeCount - 1; i >= 0; i--)
        if (!strcmp(c->scope[i]->name, name)) return c->scope[i];
    return NULL;
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
    QK_PUSH(c->scope, c->scopeCount, c->scopeCap, b);
    return b;
}

/* "did you mean" over every name in reach */
static void unknownName(Checker* c, const Expr* e) {
    const char* names[512];
    int n = 0;
    for (int i = 0; i < c->scopeCount && n < 400; i++) names[n++] = c->scope[i]->name;
    for (int i = 0; i < c->prog->count && n < 480; i++) names[n++] = c->prog->decls[i]->name;
    for (int i = 1; i < B_COUNT && n < 512; i++) names[n++] = qkBuiltinNames[i];
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

/* Makes *slot fit `want`, converting when Quark allows it. */
static bool coerce(Checker* c, Expr** slot, Type* want, const char* where) {
    Type* have = (*slot)->type;
    if (qkTypeSame(have, want)) return true;
    if (have->kind == TY_INT && want->kind == TY_FLOAT) {
        wrapImplicit(slot, B_TOFLOAT, &qkTypeFloat);
        return true;
    }
    if ((have->kind == TY_RANGE || have->kind == TY_ITER) && want->kind == TY_ARRAY && qkTypeSame(have->elem, want->elem)) {
        wrapImplicit(slot, B_TOARRAY, want);
        return true;
    }
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

/* ---- lambdas ------------------------------------------------------------------------------------- */

/* Checks a function argument: a lambda, or a function's name (made into one). Returns its result type. */
static Type* checkLambda(Checker* c, Expr** slot, Type** paramTypes, int maxParams, int minParams, const char* builtin) {
    Expr* e = *slot;
    if (e->kind == E_NAME) {
        Decl* d = lookup(c, e->name) ? NULL : qkFindDecl(c->prog, e->name);
        if (d && d->kind == D_FN) {
            if (d->paramCount < minParams || d->paramCount > maxParams) {
                qkError(c->src, e->line, e->col, "%s needs a function of %d value%s here; %s takes %d", builtin, minParams,
                        minParams == 1 ? "" : "s", d->name, d->paramCount);
                return &qkTypeError;
            }
            Expr* lam = qkNewExpr(E_LAMBDA, e->line, e->col);
            lam->paramCount = d->paramCount;
            lam->params = qkNew(sizeof(Param) * (size_t)d->paramCount);
            Expr* call = qkNewExpr(E_CALL, e->line, e->col);
            call->name = d->name;
            for (int i = 0; i < d->paramCount; i++) {
                char* pn = qkNew(8);
                snprintf(pn, 8, "#%d", i);
                lam->params[i] = (Param){pn, NULL, e->line, e->col, NULL};
                Expr* ref = qkNewExpr(E_NAME, e->line, e->col);
                ref->name = pn;
                if (call->argCount == call->argCap) {
                    call->argCap = d->paramCount;
                    call->args = qkNew(sizeof(Expr*) * (size_t)d->paramCount);
                    call->argNames = qkNew(sizeof(char*) * (size_t)d->paramCount);
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
    if (e->argCount >= lo && e->argCount <= hi) {
        for (int i = 0; i < e->argCount; i++)
            if (e->argNames[i]) {
                qkError(c->src, e->args[i]->line, e->args[i]->col, "%s takes no named values", e->name);
                return false;
            }
        return true;
    }
    if (lo == hi) qkError(c->src, e->line, e->col, "%s takes %d value%s, not %d", e->name, lo, lo == 1 ? "" : "s", e->argCount);
    else qkError(c->src, e->line, e->col, "%s takes %d to %d values, not %d", e->name, lo, hi, e->argCount);
    return false;
}

static Type* checkSource(Checker* c, Expr* e, int i) {
    Type* t = checkExpr(c, e->args[i], true);
    if (!qkIsIterable(t)) {
        typeError(c, e->args[i], "expected an array, a range or a loop, found %s", t, NULL);
        return &qkTypeError;
    }
    markIterUse(e->args[i]);
    return t->elem;
}

static bool printable(const Type* t) {
    switch (t->kind) {
    case TY_ARRAY: return printable(t->elem);
    case TY_RANGE: case TY_ITER: case TY_VOID: case TY_ERROR: return false;
    default: return true;
    }
}

static Type* checkBuiltin(Checker* c, Expr* e, int b) {
    e->builtin = b;
    switch (b) {
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
    case B_LERP: {
        if (!argCount(c, e, 3, 3)) return &qkTypeError;
        for (int i = 0; i < 3; i++) checkExpr(c, e->args[i], true);
        Type* t = e->args[0]->type->kind == TY_VEC ? e->args[0]->type : &qkTypeFloat;
        coerce(c, &e->args[0], t, "lerp");
        coerce(c, &e->args[1], t, "lerp");
        coerce(c, &e->args[2], &qkTypeFloat, "lerp");
        return t;
    }
    case B_LENGTH: case B_DOT: {
        int n = b == B_LENGTH ? 1 : 2;
        if (!argCount(c, e, n, n)) return &qkTypeError;
        Type* t = checkExpr(c, e->args[0], true);
        if (t->kind != TY_VEC) {
            typeError(c, e->args[0], "expected a vector (Vec2, Vec3, Vec4), found %s", t, NULL);
            return &qkTypeError;
        }
        if (n == 2) {
            checkExpr(c, e->args[1], true);
            coerce(c, &e->args[1], t, "dot");
        }
        return &qkTypeFloat;
    }
    case B_LEN: {
        if (!argCount(c, e, 1, 1)) return &qkTypeError;
        checkSource(c, e, 0);
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
        if (t->kind != TY_ERROR && !isNumeric(t) && t->kind != TY_VEC) typeError(c, e->args[0], "sum: expected numbers or vectors, found a list of %s", t, NULL);
        return t;
    }
    case B_HISTOGRAM: {
        if (!argCount(c, e, 2, 3)) return &qkTypeError;
        Type* elem = checkSource(c, e, 0);
        checkExpr(c, e->args[1], true);
        coerce(c, &e->args[1], &qkTypeInt, "histogram's number of bins");
        if (e->argCount == 3) {
            Type* params[1] = {elem};
            Type* r = checkLambda(c, &e->args[2], params, 1, 1, "histogram");
            if (r->kind != TY_INT && r->kind != TY_ERROR) typeError(c, e->args[2], "histogram: the lambda must give an Int (the bin), not %s", r, NULL);
        } else if (elem->kind != TY_INT && elem->kind != TY_ERROR) {
            typeError(c, e, "histogram of %s needs a lambda giving each value's bin", elem, NULL);
        }
        return qkTypeArray(&qkTypeInt);
    }
    case B_TOARRAY: {
        if (!argCount(c, e, 1, 1)) return &qkTypeError;
        return qkTypeArray(checkSource(c, e, 0));
    }
    case B_PRINT:
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
        argCount(c, e, 0, 0);
        return &qkTypeFloat;
    case B_VEC2: case B_VEC3: case B_VEC4: {
        int n = b - B_VEC2 + 2;
        if (!argCount(c, e, n, n)) return &qkTypeError;
        for (int i = 0; i < n; i++) {
            checkExpr(c, e->args[i], true);
            coerce(c, &e->args[i], &qkTypeFloat, e->name);
        }
        return qkTypeVec(n);
    }
    }
    return &qkTypeError;
}

static Type* fnReturnType(Checker* c, Decl* d, const Expr* at);

static Type* checkCall(Checker* c, Expr* e) {
    Decl* d = qkFindDecl(c->prog, e->name);
    if (d && d->kind == D_TYPE) { /* a record */
        e->decl = d;
        if (e->argCount != d->paramCount) {
            qkError(c->src, e->line, e->col, "%s has %d field%s, given %d", d->name, d->paramCount, d->paramCount == 1 ? "" : "s", e->argCount);
            return &qkTypeError;
        }
        bool named = e->argCount > 0 && e->argNames[0];
        Expr** ordered = qkNew(sizeof(Expr*) * (size_t)(e->argCount ? e->argCount : 1));
        for (int i = 0; i < e->argCount; i++) {
            int field = i;
            if (named != (e->argNames[i] != NULL)) {
                qkError(c->src, e->args[i]->line, e->args[i]->col, "name every field or none");
                return &qkTypeError;
            }
            if (named) {
                field = -1;
                for (int f = 0; f < d->paramCount; f++)
                    if (!strcmp(d->params[f].name, e->argNames[i])) field = f;
                if (field < 0) {
                    qkError(c->src, e->args[i]->line, e->args[i]->col, "%s has no field \"%s\"", d->name, e->argNames[i]);
                    return &qkTypeError;
                }
                if (ordered[field]) {
                    qkError(c->src, e->args[i]->line, e->args[i]->col, "\"%s\" is given twice", e->argNames[i]);
                    return &qkTypeError;
                }
            }
            checkExpr(c, e->args[i], true);
            ordered[field] = e->args[i];
            coerce(c, &ordered[field], d->params[field].binding->type, d->params[field].name);
        }
        /* keep the written order for printing; lowering reads `ordered` through argNames */
        if (!named)
            for (int i = 0; i < e->argCount; i++) e->args[i] = ordered[i];
        else
            for (int i = 0; i < e->argCount; i++)
                for (int f = 0; f < d->paramCount; f++)
                    if (!strcmp(d->params[f].name, e->argNames[i])) e->args[i] = ordered[f];
        return d->type;
    }
    if (d && d->kind == D_FN && !lookup(c, e->name)) {
        e->decl = d;
        if (!argCount(c, e, d->paramCount, d->paramCount)) {
            for (int i = 0; i < e->argCount; i++) checkExpr(c, e->args[i], true);
            return fnReturnType(c, d, e);
        }
        for (int i = 0; i < e->argCount; i++) {
            Type* t = checkExpr(c, e->args[i], true);
            if (t->kind == TY_RANGE || t->kind == TY_ITER) markIterUse(e->args[i]);
            coerce(c, &e->args[i], d->params[i].binding->type, d->params[i].name);
        }
        return fnReturnType(c, d, e);
    }
    if (d && d->kind == D_ENUM) {
        qkError(c->src, e->line, e->col, "%s is an enum: pick a case, %s.%s", d->name, d->name, d->caseCount ? d->cases[0] : "Case");
        return &qkTypeError;
    }
    int b = builtinNamed(e->name);
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
    return unifyBranches(c, slots, 2, e);
}

static Type* checkMatch(Checker* c, Expr* e, bool wantValue) {
    Type* t = checkExpr(c, e->a, true);
    if (t->kind != TY_ENUM && t->kind != TY_INT) {
        if (t->kind != TY_ERROR) typeError(c, e->a, "match works on an enum or an Int, not %s", t, NULL);
        return &qkTypeError;
    }
    bool wildcard = false;
    bool seen[256] = {0};
    for (int i = 0; i < e->armCount; i++) {
        Arm* a = &e->arms[i];
        if (!a->pattern && !a->intPattern) {
            wildcard = true;
            if (i != e->armCount - 1) qkError(c->src, a->body->line, a->body->col, "_ catches everything: it goes last");
        } else if (a->pattern) {
            if (t->kind != TY_ENUM) {
                qkError(c->src, a->body->line, a->body->col, "%s is a case, but the match is on an Int", a->pattern);
                continue;
            }
            Decl* d = t->decl;
            a->caseIndex = -1;
            for (int k = 0; k < d->caseCount; k++)
                if (!strcmp(d->cases[k], a->pattern)) a->caseIndex = k;
            if (a->caseIndex < 0) {
                const char* close = qkClosest(a->pattern, d->cases, d->caseCount);
                qkError(c->src, e->line, e->col, "%s has no case %s%s%s%s", d->name, a->pattern, close ? ": did you mean " : "",
                        close ? close : "", close ? "?" : "");
                continue;
            }
            if (a->caseIndex < 256 && seen[a->caseIndex]) qkError(c->src, a->body->line, a->body->col, "%s is matched twice", a->pattern);
            if (a->caseIndex < 256) seen[a->caseIndex] = true;
        } else if (t->kind != TY_INT) {
            qkError(c->src, a->intPattern->line, a->intPattern->col, "an Int pattern, but the match is on %s", t->decl->name);
        }
        checkExpr(c, a->body, wantValue);
    }
    if (!wildcard) {
        if (t->kind == TY_INT) qkError(c->src, e->line, e->col, "a match on an Int needs a last arm _ => ...");
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
    return unifyBranches(c, slots, e->armCount, e);
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

static void checkStmt(Checker* c, Stmt* s) {
    switch (s->kind) {
    case S_LET: case S_VAR: {
        int readsBefore = c->varReads;
        Type* t = checkExpr(c, s->value, true);
        if (s->typeAnn) {
            Type* want = resolveType(c, s->typeAnn);
            coerce(c, &s->value, want, s->name);
            t = want;
        }
        if (t->kind == TY_VOID) {
            qkError(c->src, s->value->line, s->value->col, "this gives nothing to keep in \"%s\"", s->name);
            t = &qkTypeError;
        }
        bool loop = t->kind == TY_RANGE || t->kind == TY_ITER;
        if (loop && s->kind == S_VAR) {
            markIterUse(s->value);
            coerce(c, &s->value, qkTypeArray(t->elem), s->name);
            t = s->value->type;
        }
        Binding* b = declare(c, s->name, loop ? qkTypeArray(t->elem) : t, s->kind == S_VAR, s->line, s->col);
        b->init = s->value;
        b->readsVar = c->varReads != readsBefore;
        s->binding = b;
        if (loop) QK_PUSH(c->lets, c->letCount, c->letCap, b);
        break;
    }
    case S_ASSIGN: {
        Type* target = checkExpr(c, s->target, true);
        bool direct;
        Binding* root = placeRoot(s->target, &direct);
        if (!root) {
            qkError(c->src, s->target->line, s->target->col, "only a var, an element of a var array, or a field of a var can be set");
        } else if (!root->mutable) {
            qkError(c->src, s->target->line, s->target->col, "\"%s\" is a let (it never changes); declare it with var to change it", root->name);
        } else if (!direct) {
            qkError(c->src, s->target->line, s->target->col, "set an element through the array's own name: arr[i] = ...");
        }
        Type* value = checkExpr(c, s->value, true);
        if (s->op != T_ASSIGN) {
            bool ok = isNumeric(target) || (target->kind == TY_VEC && s->op != T_ASSIGN);
            if (!ok) typeError(c, s->target, "%s cannot be changed with +=, -=, *= or /=", target, NULL);
            if (target->kind == TY_VEC && (s->op == T_STAREQ || s->op == T_SLASHEQ) && isNumeric(value)) {
                coerce(c, &s->value, &qkTypeFloat, "the factor");
                break;
            }
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
        if (e->kind != E_CALL && e->kind != E_IF && e->kind != E_MATCH && e->kind != E_BLOCK && e->type->kind != TY_ERROR)
            qkError(c->src, e->line, e->col, "this value is not used: keep it with let, or remove it");
        break;
    }
    case S_FOR: {
        Type* t = checkExpr(c, s->iter, true);
        if (!qkIsIterable(t)) {
            if (t->kind != TY_ERROR) typeError(c, s->iter, "for goes over an array, a range or a loop, not %s", t, NULL);
            t = &qkTypeError;
        }
        markIterUse(s->iter);
        int mark = c->scopeCount;
        c->loopDepth++;
        s->binding = declare(c, s->name, t->kind == TY_ERROR ? t : t->elem, false, s->line, s->col);
        if (s->indexName) s->indexBinding = declare(c, s->indexName, &qkTypeInt, false, s->line, s->col);
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
    switch (e->op) {
    case T_AND: case T_OR:
        if (a->kind != TY_BOOL || b->kind != TY_BOOL) {
            typeError(c, e, "and / or join Bools, not %s and %s", a, b);
            return &qkTypeError;
        }
        return &qkTypeBool;
    case T_PLUS: case T_MINUS: case T_STAR: case T_SLASH: case T_PERCENT:
        if (a->kind == TY_VEC || b->kind == TY_VEC) {
            if (a->kind == TY_VEC && b->kind == TY_VEC && a == b && e->op != T_PERCENT) return a;
            if (a->kind == TY_VEC && isNumeric(b) && (e->op == T_STAR || e->op == T_SLASH)) {
                coerce(c, &e->b, &qkTypeFloat, "");
                return a;
            }
            if (b->kind == TY_VEC && isNumeric(a) && e->op == T_STAR) {
                coerce(c, &e->a, &qkTypeFloat, "");
                return b;
            }
            typeError(c, e, "vectors: add and subtract vectors of one size, scale them by numbers (not %s and %s)", a, b);
            return &qkTypeError;
        }
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
        if (a == b && (a->kind == TY_BOOL || a->kind == TY_ENUM || a->kind == TY_VEC)) return &qkTypeBool;
        typeError(c, e, "== compares numbers, Bools, enum cases or vectors of one kind, not %s and %s", a, b);
        return &qkTypeError;
    default: /* < <= > >= */
        if (isNumeric(a) && isNumeric(b)) {
            unifyNumeric(c, &e->a, &e->b);
            return &qkTypeBool;
        }
        typeError(c, e, "< and > compare numbers, not %s and %s", a, b);
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
        Binding* b = lookup(c, e->name);
        if (b) {
            e->binding = b;
            b->uses++;
            if (b->mutable) c->varReads++;
            if (b->depth != c->loopDepth) b->iterUses += 1000; /* used deeper in loops: never fused */
            return b->type;
        }
        Decl* d = qkFindDecl(c->prog, e->name);
        if (d && d->kind == D_FN) qkError(c->src, e->line, e->col, "%s is a function: call it, %s(...)", e->name, e->name);
        else if (d) qkError(c->src, e->line, e->col, "%s is a type, not a value", e->name);
        else unknownName(c, e);
        return &qkTypeError;
    }
    case E_FIELD: {
        if (e->a->kind == E_NAME && e->a->name[0] >= 'A' && e->a->name[0] <= 'Z') {
            Decl* d = qkFindDecl(c->prog, e->a->name);
            if (d && d->kind == D_ENUM) {
                for (int k = 0; k < d->caseCount; k++)
                    if (!strcmp(d->cases[k], e->name)) {
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
        if (t->kind == TY_VEC) {
            static const char* const comps = "xyzw";
            if (strlen(e->name) == 1 && strchr(comps, e->name[0]) && strchr(comps, e->name[0]) - comps < t->n) {
                e->caseIndex = (int)(strchr(comps, e->name[0]) - comps);
                return &qkTypeFloat;
            }
            static const char* const fields[] = {"x and y", "x, y and z", "x, y, z and w"};
            qkError(c->src, e->line, e->col, "Vec%d has %s", t->n, fields[t->n - 2]);
            return &qkTypeError;
        }
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
        Type* i = checkExpr(c, e->b, true);
        if (i->kind != TY_INT && i->kind != TY_ERROR) typeError(c, e->b, "an index is an Int, not %s", i, NULL);
        if (t->kind == TY_RANGE || t->kind == TY_ITER) {
            markIterUse(e->a);
            coerce(c, &e->a, qkTypeArray(t->elem), "indexing");
            t = e->a->type;
        }
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
        if (!isNumeric(t) && t->kind != TY_VEC && t->kind != TY_ERROR) typeError(c, e->a, "- takes a number or a vector, not %s", t, NULL);
        return t;
    }
    case E_BINARY: return checkBinary(c, e);
    case E_RANGE: {
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
        if (!e->argCount) {
            qkError(c->src, e->line, e->col, "an empty array needs its type: let xs: [Int] = fill(0, 0)");
            return &qkTypeError;
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
    case E_LAMBDA:
        qkError(c->src, e->line, e->col, "a lambda goes only into map, filter, fold, histogram, any or all");
        return &qkTypeError;
    case E_CALL: return checkCall(c, e);
    }
    return &qkTypeError;
}

static Type* checkExpr(Checker* c, Expr* e, bool wantValue) {
    if (e->type && e->implicit) return e->type;
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
    /* infer it now, in a checker of its own */
    Checker inner = *c;
    inner.scope = NULL;
    inner.scopeCount = inner.scopeCap = 0;
    inner.lets = NULL;
    inner.letCount = inner.letCap = 0;
    checkFnBody(&inner, d);
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
    c->fn = d;
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
        } else if (!d->retType) {
            if (t->kind == TY_RANGE || t->kind == TY_ITER) {
                markIterUse(*result);
                coerce(c, result, qkTypeArray(t->elem), "the result");
                t = qkTypeArray(t->elem);
            }
            d->retType = t;
        }
    } else {
        t = checkExpr(c, d->body, true);
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

bool qkCheck(Program* p) {
    int errorsBefore = qkErrorCount;
    Checker c = {0};
    c.prog = p;
    c.src = &p->src;
    /* names: one meaning each */
    for (int i = 0; i < p->count; i++) {
        Decl* d = p->decls[i];
        for (int j = 0; j < i; j++)
            if (!strcmp(p->decls[j]->name, d->name)) qkError(c.src, d->line, d->col, "\"%s\" is declared twice", d->name);
        if (builtinNamed(d->name)) qkError(c.src, d->line, d->col, "\"%s\" is a built-in name", d->name);
        if (d->kind != D_FN) {
            d->type = qkNew(sizeof(Type));
            d->type->kind = d->kind == D_TYPE ? TY_RECORD : TY_ENUM;
            d->type->decl = d;
        }
    }
    /* record fields and function parameters */
    for (int i = 0; i < p->count; i++) {
        Decl* d = p->decls[i];
        if (d->kind == D_ENUM) {
            for (int k = 0; k < d->caseCount; k++)
                for (int j = 0; j < k; j++)
                    if (!strcmp(d->cases[j], d->cases[k])) qkError(c.src, d->line, d->col, "%s.%s is listed twice", d->name, d->cases[k]);
            continue;
        }
        for (int f = 0; f < d->paramCount; f++) {
            Param* prm = &d->params[f];
            for (int j = 0; j < f; j++)
                if (!strcmp(d->params[j].name, prm->name)) qkError(c.src, prm->line, prm->col, "\"%s\" is listed twice", prm->name);
            Binding* b = qkNew(sizeof *b);
            b->name = prm->name;
            b->type = resolveType(&c, prm->type);
            b->isParam = true;
            prm->binding = b;
            if (b->type == d->type) qkError(c.src, prm->line, prm->col, "%s cannot hold itself", d->name);
        }
    }
    if (qkErrorCount > errorsBefore) return false;
    for (int i = 0; i < p->count; i++)
        if (p->decls[i]->kind == D_FN) {
            c.scopeCount = 0;
            c.letCount = 0;
            checkFnBody(&c, p->decls[i]);
        }
    Decl* main = qkFindDecl(p, "main");
    if (!main || main->kind != D_FN) qkError(c.src, 1, 1, "a program starts at fn main() { ... }");
    else if (main->paramCount || (main->retType && main->retType->kind != TY_VOID))
        qkError(c.src, main->line, main->col, "main takes nothing and gives nothing: fn main() { ... }");
    return qkErrorCount == errorsBefore;
}
