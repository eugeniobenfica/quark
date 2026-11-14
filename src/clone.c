/* Quark — copies of a syntax tree, as the parser made it (nothing resolved). A generic function, or one that
 * takes a function, is a template: each use gets a copy of its body, checked with that use's types and
 * lambdas, so it costs what hand-written code would. */
#include "qk.h"

static Expr** cloneList(Expr* const* list, int n) {
    if (!list) return NULL;
    Expr** out = qkNew(sizeof(Expr*) * (size_t)(n ? n : 1));
    for (int i = 0; i < n; i++) out[i] = qkCloneExpr(list[i]);
    return out;
}

static const char** copyNames(const char* const* names, int n) {
    if (!names) return NULL;
    const char** out = qkNew(sizeof(char*) * (size_t)(n ? n : 1));
    memcpy(out, names, sizeof(char*) * (size_t)n);
    return out;
}

static Param* cloneParams(const Param* ps, int n) {
    if (!ps) return NULL;
    Param* out = qkNew(sizeof(Param) * (size_t)(n ? n : 1));
    for (int i = 0; i < n; i++) {
        out[i] = ps[i];
        out[i].binding = NULL;
        out[i].fallback = ps[i].fallback ? qkCloneExpr(ps[i].fallback) : NULL;
    }
    return out;
}

static void cloneArm(Arm* to, const Arm* a) {
    *to = *a;
    to->intPattern = a->intPattern ? qkCloneExpr(a->intPattern) : NULL;
    to->intHigh = a->intHigh ? qkCloneExpr(a->intHigh) : NULL;
    to->textPattern = a->textPattern ? qkCloneExpr(a->textPattern) : NULL;
    to->guard = a->guard ? qkCloneExpr(a->guard) : NULL;
    to->body = a->body ? qkCloneExpr(a->body) : NULL;
    to->binds = copyNames(a->binds, a->bindCount);
    to->bindings = NULL;
    to->caseIndex = 0;
}

Expr* qkCloneExpr(const Expr* e) {
    if (!e) return NULL;
    Expr* c = qkNew(sizeof *c);
    *c = *e;
    c->type = NULL;
    c->binding = NULL;
    c->builtin = 0;
    c->decl = NULL;
    c->caseIndex = 0;
    c->ordered = NULL;
    c->orderedCount = 0;
    c->typed = false;
    c->a = qkCloneExpr(e->a);
    c->b = qkCloneExpr(e->b);
    c->args = cloneList(e->args, e->argCount);
    c->argNames = copyNames(e->argNames, e->argCount);
    c->argCap = e->argCount;
    c->thenBlock = e->thenBlock ? qkCloneBlock(e->thenBlock) : NULL;
    c->elseExpr = qkCloneExpr(e->elseExpr);
    c->block = e->block ? qkCloneBlock(e->block) : NULL;
    if (e->arms) {
        c->arms = qkNew(sizeof(Arm) * (size_t)(e->armCount ? e->armCount : 1));
        for (int i = 0; i < e->armCount; i++) cloneArm(&c->arms[i], &e->arms[i]);
        c->armCap = e->armCount;
    }
    c->params = cloneParams(e->params, e->paramCount);
    c->body = qkCloneExpr(e->body);
    c->sugar = NULL;
    return c;
}

static Stmt* cloneStmt(const Stmt* s) {
    Stmt* c = qkNew(sizeof *c);
    *c = *s;
    c->target = qkCloneExpr(s->target);
    c->value = qkCloneExpr(s->value);
    c->iter = qkCloneExpr(s->iter);
    c->body = s->body ? qkCloneBlock(s->body) : NULL;
    c->binding = NULL;
    c->indexBinding = NULL;
    c->opValue = NULL;
    if (s->pattern) {
        c->pattern = qkNew(sizeof(Arm));
        cloneArm(c->pattern, s->pattern);
    }
    c->elseBody = s->elseBody ? qkCloneBlock(s->elseBody) : NULL;
    return c;
}

Block* qkCloneBlock(const Block* b) {
    Block* c = qkNew(sizeof *c);
    *c = *b;
    c->stmts = qkNew(sizeof(Stmt*) * (size_t)(b->count ? b->count : 1));
    for (int i = 0; i < b->count; i++) c->stmts[i] = cloneStmt(b->stmts[i]);
    c->cap = b->count;
    return c;
}

Decl* qkCloneDecl(const Decl* d) {
    Decl* c = qkNew(sizeof *c);
    *c = *d;
    c->params = cloneParams(d->params, d->paramCount);
    c->paramCap = d->paramCount;
    if (d->caseFields) { /* an enum's cases' data */
        c->caseFields = qkNew(sizeof(Param*) * 64);
        c->caseFieldCount = qkNew(sizeof(int) * 64);
        for (int k = 0; k < d->caseCount && k < 64; k++) {
            c->caseFieldCount[k] = d->caseFieldCount[k];
            c->caseFields[k] = cloneParams(d->caseFields[k], d->caseFieldCount[k]);
        }
    }
    c->body = qkCloneExpr(d->body);
    c->type = NULL;
    c->retType = NULL;
    c->state = 0;
    c->instances = NULL;
    c->instanceCount = c->instanceCap = 0;
    c->pristine = NULL;
    c->returnsEarly = false;
    return c;
}
