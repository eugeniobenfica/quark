/* Quark — constants worked out when the program is compiled. A const's value is pure, so the compiler works
 * it out once: numbers, Bools, texts (joined, with values put in), arrays of them, ifs on what is known, other
 * constants. What it gives is a literal the lowering puts where the constant is used (an array of numbers goes
 * into the program's data, made once, never allocated again). A value it cannot work out here (a call to a
 * function, a record) stays the expression it is, worked out where it is used, as before. A mistake it finds
 * (a division by zero) is an error before the program runs. */
#include "qk.h"

#include <stdio.h>
#include <string.h>

static Expr* literal(ExprKind kind, const Expr* at, Type* type) {
    Expr* e = qkNewExpr(kind, at->line, at->col);
    e->type = type;
    return e;
}

static Expr* intLit(const Expr* at, int64_t v) {
    Expr* e = literal(E_INT, at, &qkTypeInt);
    e->i = v;
    return e;
}

static Expr* floatLit(const Expr* at, double v) {
    Expr* e = literal(E_FLOAT, at, &qkTypeFloat);
    e->f = v;
    return e;
}

static Expr* boolLit(const Expr* at, bool v) {
    Expr* e = literal(E_BOOL, at, &qkTypeBool);
    e->i = v;
    return e;
}

static Expr* textLit(const Expr* at, const char* s, int n) {
    Expr* e = literal(E_TEXT, at, &qkTypeText);
    e->text = qkDup(s, (size_t)n);
    e->textLen = n;
    return e;
}

/* floored, as Quark's / and % are */
static int64_t floorDiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

static Expr* fold(Expr* e, const QkSource* src, int depth);

/* the value of a block that is only an expression: { 3 } */
static Expr* blockValue(Block* b, const QkSource* src, int depth) {
    if (!b || b->count != 1 || b->stmts[0]->kind != S_EXPR) return NULL;
    return fold(b->stmts[0]->value, src, depth);
}

static Expr* foldBinary(Expr* e, Expr* a, Expr* b, const QkSource* src) {
    int op = e->op;
    if (a->kind == E_INT && b->kind == E_INT) {
        uint64_t x = (uint64_t)a->i, y = (uint64_t)b->i;
        switch (op) {
        case T_PLUS: return intLit(e, (int64_t)(x + y)); /* wraps around, as at run time */
        case T_MINUS: return intLit(e, (int64_t)(x - y));
        case T_STAR: return intLit(e, (int64_t)(x * y));
        case T_SLASH: case T_PERCENT:
            if (b->i == 0) {
                qkError(src, e->line, e->col, "division by zero (in a constant, worked out when the program is compiled)");
                return NULL;
            }
            if (a->i == INT64_MIN && b->i == -1) return op == T_SLASH ? intLit(e, INT64_MIN) : intLit(e, 0);
            return op == T_SLASH ? intLit(e, floorDiv(a->i, b->i)) : intLit(e, a->i - floorDiv(a->i, b->i) * b->i);
        case T_EQ: return boolLit(e, a->i == b->i);
        case T_NE: return boolLit(e, a->i != b->i);
        case T_LT: return boolLit(e, a->i < b->i);
        case T_LE: return boolLit(e, a->i <= b->i);
        case T_GT: return boolLit(e, a->i > b->i);
        case T_GE: return boolLit(e, a->i >= b->i);
        default: return NULL;
        }
    }
    if (a->kind == E_FLOAT && b->kind == E_FLOAT) {
        double x = a->f, y = b->f;
        switch (op) {
        case T_PLUS: return floatLit(e, x + y);
        case T_MINUS: return floatLit(e, x - y);
        case T_STAR: return floatLit(e, x * y);
        case T_SLASH: return floatLit(e, x / y);
        case T_EQ: return boolLit(e, x == y);
        case T_NE: return boolLit(e, x != y);
        case T_LT: return boolLit(e, x < y);
        case T_LE: return boolLit(e, x <= y);
        case T_GT: return boolLit(e, x > y);
        case T_GE: return boolLit(e, x >= y);
        default: return NULL;
        }
    }
    if (a->kind == E_BOOL && b->kind == E_BOOL) {
        switch (op) {
        case T_AND: return boolLit(e, a->i && b->i);
        case T_OR: return boolLit(e, a->i || b->i);
        case T_EQ: return boolLit(e, a->i == b->i);
        case T_NE: return boolLit(e, a->i != b->i);
        default: return NULL;
        }
    }
    if (a->kind == E_TEXT && b->kind == E_TEXT) {
        int n = a->textLen < b->textLen ? a->textLen : b->textLen;
        int c = memcmp(a->text, b->text, (size_t)n);
        if (!c) c = (a->textLen > b->textLen) - (a->textLen < b->textLen);
        switch (op) {
        case T_PLUS: {
            QkBuf joined = {0};
            qkBufAdd(&joined, a->text, (size_t)a->textLen);
            qkBufAdd(&joined, b->text, (size_t)b->textLen);
            Expr* r = textLit(e, joined.data ? joined.data : "", (int)joined.len);
            qkBufFree(&joined);
            return r;
        }
        case T_EQ: return boolLit(e, c == 0);
        case T_NE: return boolLit(e, c != 0);
        case T_LT: return boolLit(e, c < 0);
        case T_LE: return boolLit(e, c <= 0);
        case T_GT: return boolLit(e, c > 0);
        case T_GE: return boolLit(e, c >= 0);
        default: return NULL;
        }
    }
    return NULL;
}

static Expr* fold(Expr* e, const QkSource* src, int depth) {
    if (!e || !e->type || depth > 64) return NULL;
    switch (e->kind) {
    case E_INT: case E_FLOAT: case E_BOOL: case E_TEXT: return e;
    case E_NAME:
        if (e->decl && e->decl->kind == D_CONST) {
            if (!e->decl->folded) e->decl->folded = fold(e->decl->body, e->decl->src ? e->decl->src : src, depth + 1);
            if (e->decl->folded && e->decl->folded->kind == E_ARRAY && e->type != e->decl->folded->type) return NULL;
            return e->decl->folded;
        }
        return NULL;
    case E_UNARY: {
        if (e->decl) return NULL; /* -v of a type: its function */
        Expr* a = fold(e->a, src, depth + 1);
        if (!a) return NULL;
        if (e->op == T_MINUS && a->kind == E_INT) return intLit(e, (int64_t)(0 - (uint64_t)a->i));
        if (e->op == T_MINUS && a->kind == E_FLOAT) return floatLit(e, -a->f);
        if (e->op == T_NOT && a->kind == E_BOOL) return boolLit(e, !a->i);
        return NULL;
    }
    case E_BINARY: {
        if (e->decl) return NULL; /* an operator of a type */
        if (e->op == T_AND || e->op == T_OR) { /* the right side only when it counts, as at run time */
            Expr* a = fold(e->a, src, depth + 1);
            if (!a || a->kind != E_BOOL) return NULL;
            if (e->op == T_AND && !a->i) return boolLit(e, false);
            if (e->op == T_OR && a->i) return boolLit(e, true);
            Expr* b = fold(e->b, src, depth + 1);
            return b && b->kind == E_BOOL ? boolLit(e, b->i) : NULL;
        }
        Expr* a = fold(e->a, src, depth + 1);
        Expr* b = a ? fold(e->b, src, depth + 1) : NULL;
        if (!a || !b) return NULL;
        return foldBinary(e, a, b, src);
    }
    case E_CALL:
        if (e->implicit && e->builtin == B_TOFLOAT) { /* an Int where a Float goes */
            Expr* a = fold(e->args[0], src, depth + 1);
            if (a && a->kind == E_INT) return floatLit(e, (double)a->i);
            if (a && a->kind == E_FLOAT) return a;
            return NULL;
        }
        if (e->builtin == B_LEN && e->argCount == 1) {
            Expr* a = fold(e->args[0], src, depth + 1);
            if (a && a->kind == E_TEXT) return intLit(e, a->textLen);
            if (a && a->kind == E_ARRAY) return intLit(e, a->argCount);
            return NULL;
        }
        return NULL;
    case E_IF: {
        Expr* c = fold(e->a, src, depth + 1);
        if (!c || c->kind != E_BOOL || !e->elseExpr) return NULL;
        if (c->i) return blockValue(e->thenBlock, src, depth + 1);
        return e->elseExpr->kind == E_BLOCK ? blockValue(e->elseExpr->block, src, depth + 1) : fold(e->elseExpr, src, depth + 1);
    }
    case E_INTERP: { /* "v{n}": Ints, Bools and texts without a format */
        QkBuf out = {0};
        for (int i = 0; i <= e->argCount; i++) {
            if (e->parts[i]) qkBufPuts(&out, e->parts[i]);
            if (i == e->argCount) break;
            Expr* a = e->formats && e->formats[i] ? NULL : fold(e->args[i], src, depth + 1);
            if (!a || (a->kind != E_INT && a->kind != E_BOOL && a->kind != E_TEXT)) {
                qkBufFree(&out);
                return NULL;
            }
            if (a->kind == E_INT) qkBufPrintf(&out, "%lld", (long long)a->i);
            else if (a->kind == E_BOOL) qkBufPuts(&out, a->i ? "true" : "false");
            else qkBufAdd(&out, a->text, (size_t)a->textLen);
        }
        Expr* r = textLit(e, out.data ? out.data : "", (int)out.len);
        qkBufFree(&out);
        return r;
    }
    case E_ARRAY: {
        if (!e->argCount) return NULL;
        Expr* r = literal(E_ARRAY, e, e->type);
        r->args = qkNew(sizeof(Expr*) * (size_t)e->argCount);
        r->argCount = r->argCap = e->argCount;
        for (int i = 0; i < e->argCount; i++) {
            Expr* a = fold(e->args[i], src, depth + 1);
            if (!a || (a->kind != E_INT && a->kind != E_FLOAT && a->kind != E_BOOL && a->kind != E_TEXT)) return NULL;
            r->args[i] = a;
        }
        return r;
    }
    default: return NULL;
    }
}

void qkFoldConst(Decl* d, const QkSource* src) {
    if (d->kind != D_CONST || d->folded || !d->body || !d->retType || d->retType->kind == TY_ERROR) return;
    d->folded = fold(d->body, src, 0);
}
