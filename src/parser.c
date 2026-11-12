/* Quark — the parser: tokens to a syntax tree. Comments stay attached to the declarations and statements
 * they sit above (or beside), so the formatter and the node graph keep them. */
#include "qk.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct Parser {
    const QkSource* src;
    Lexed lx;
    int i;
    int comment;  /* the next comment not yet attached */
    bool failed;
    bool externNext; /* the fn being parsed is an extern one */
} Parser;

Expr* qkNewExpr(ExprKind kind, int line, int col) {
    Expr* e = qkNew(sizeof *e);
    e->kind = kind;
    e->line = line;
    e->col = col;
    return e;
}

static Token* peek(Parser* p) { return &p->lx.tokens[p->i]; }
static Token* peekAt(Parser* p, int ahead) {
    int k = p->i + ahead;
    return &p->lx.tokens[k < p->lx.count ? k : p->lx.count - 1];
}
static bool at(Parser* p, TokKind k) { return peek(p)->kind == k; }
static Token* next(Parser* p) {
    Token* t = peek(p);
    if (t->kind != T_EOF) p->i++;
    return t;
}
static bool accept(Parser* p, TokKind k) {
    if (!at(p, k)) return false;
    next(p);
    return true;
}

static void fail(Parser* p, Token* t, const char* fmt, const char* a) {
    if (!p->failed) qkError(p->src, t->line, t->col, fmt, a);
    p->failed = true;
}

static Token* expect(Parser* p, TokKind k, const char* where) {
    Token* t = peek(p);
    if (t->kind != k) {
        char what[160];
        snprintf(what, sizeof what, "expected \"%s\" %s, found \"%s\"", qkTokName(k), where,
                 t->kind == T_NAME || t->kind == T_TYPENAME ? qkDup(t->start, (size_t)t->len) : qkTokName(t->kind));
        fail(p, t, "%s", what);
        return t;
    }
    return next(p);
}

static void skipNewlines(Parser* p) {
    while (at(p, T_NEWLINE)) next(p);
}

static char* tokText(Token* t) { return qkDup(t->start, (size_t)t->len); }

/* ---- comments ------------------------------------------------------------------------------------ */

static const char* joinComments(Parser* p, int beforeLine) {
    QkBuf b = {0};
    while (p->comment < p->lx.commentCount && p->lx.comments[p->comment].line < beforeLine) {
        Comment* c = &p->lx.comments[p->comment++];
        if (b.len) qkBufByte(&b, '\n');
        qkBufPuts(&b, c->text);
    }
    if (!b.len) return NULL;
    const char* s = qkDup(b.data, b.len);
    qkBufFree(&b);
    return s;
}

static const char* trailingOn(Parser* p, int line) {
    if (p->comment < p->lx.commentCount) {
        Comment* c = &p->lx.comments[p->comment];
        if (c->line == line && !c->ownLine) {
            p->comment++;
            return c->text;
        }
    }
    return NULL;
}

static int lastLine(Parser* p) { return p->i > 0 ? p->lx.tokens[p->i - 1].line : 1; }

/* ---- types --------------------------------------------------------------------------------------- */

static TypeExpr* parseType(Parser* p) {
    Token* t = peek(p);
    TypeExpr* te = qkNew(sizeof *te);
    te->line = t->line;
    te->col = t->col;
    if (accept(p, T_LBRACKET)) {
        te->elem = parseType(p);
        expect(p, T_RBRACKET, "after an array's element type");
    } else if (t->kind != T_TYPENAME) {
        fail(p, t, "%s", "expected a type (Int, Float, Bool, Text, Vec2, a type's name, [Type] or Type?)");
        te->name = "Int";
        return te;
    } else {
        next(p);
        te->name = tokText(t);
    }
    if (accept(p, T_QUESTION)) { /* T?: a T, or None */
        TypeExpr* opt = qkNew(sizeof *opt);
        *opt = *te;
        te = qkNew(sizeof *te);
        te->line = opt->line;
        te->col = opt->col;
        te->elem = opt;
        te->optional = true;
    }
    return te;
}

/* ---- expressions --------------------------------------------------------------------------------- */

static Expr* parseExpr(Parser* p);
static Block* parseBlock(Parser* p);

static Expr* blockExpr(Parser* p) {
    Token* t = peek(p);
    Expr* e = qkNewExpr(E_BLOCK, t->line, t->col);
    e->block = parseBlock(p);
    return e;
}

static void addArg(Expr* e, Expr* arg, const char* name) {
    if (e->argCount == e->argCap) {
        int cap = e->argCap ? e->argCap * 2 : 4;
        Expr** args = qkNew(sizeof(Expr*) * (size_t)cap);
        const char** names = qkNew(sizeof(char*) * (size_t)cap);
        if (e->argCount) {
            memcpy(args, e->args, sizeof(Expr*) * (size_t)e->argCount);
            memcpy(names, e->argNames, sizeof(char*) * (size_t)e->argCount);
        }
        e->args = args;
        e->argNames = names;
        e->argCap = cap;
    }
    e->args[e->argCount] = arg;
    e->argNames[e->argCount] = name;
    e->argCount++;
}

static bool lambdaAhead(Parser* p) {
    if (at(p, T_NAME) && peekAt(p, 1)->kind == T_FATARROW) return true;
    if (!at(p, T_LPAREN)) return false;
    int k = 1;
    if (peekAt(p, k)->kind == T_RPAREN) return peekAt(p, k + 1)->kind == T_FATARROW;
    for (;;) {
        if (peekAt(p, k)->kind != T_NAME) return false;
        k++;
        if (peekAt(p, k)->kind == T_COMMA) {
            k++;
            continue;
        }
        return peekAt(p, k)->kind == T_RPAREN && peekAt(p, k + 1)->kind == T_FATARROW;
    }
}

static Expr* parseLambda(Parser* p) {
    Token* t = peek(p);
    Expr* e = qkNewExpr(E_LAMBDA, t->line, t->col);
    Param params[8];
    int n = 0;
    if (at(p, T_NAME)) {
        Token* name = next(p);
        params[n++] = (Param){tokText(name), NULL, name->line, name->col, NULL};
    } else {
        expect(p, T_LPAREN, "");
        while (!at(p, T_RPAREN) && !at(p, T_EOF)) {
            Token* name = expect(p, T_NAME, "in a lambda's parameters");
            if (n < 8) params[n++] = (Param){tokText(name), NULL, name->line, name->col, NULL};
            if (!accept(p, T_COMMA)) break;
        }
        expect(p, T_RPAREN, "after a lambda's parameters");
    }
    expect(p, T_FATARROW, "in a lambda");
    e->params = qkNew(sizeof(Param) * (size_t)(n ? n : 1));
    memcpy(e->params, params, sizeof(Param) * (size_t)n);
    e->paramCount = n;
    e->body = parseExpr(p);
    return e;
}

static int64_t parseIntText(Parser* p, Token* t) {
    uint64_t v = 0;
    for (int i = 0; i < t->len; i++) {
        char c = t->start[i];
        if (c == '_') continue;
        uint64_t d = (uint64_t)(c - '0');
        if (v > (UINT64_C(9223372036854775808) - d) / 10) {
            fail(p, t, "%s", "this number does not fit in an Int (64 bits)");
            return 0;
        }
        v = v * 10 + d;
    }
    return (int64_t)v; /* 9223372036854775808 wraps to the minimum, written -9223372036854775808 */
}

static double parseFloatText(Token* t) {
    char buf[128];
    int n = 0;
    for (int i = 0; i < t->len && n < 127; i++)
        if (t->start[i] != '_') buf[n++] = t->start[i];
    buf[n] = '\0';
    return strtod(buf, NULL);
}

/* The text between s and end, escapes decoded. */
static const char* decodePiece(Parser* p, Token* t, const char* s, const char* end, int* len) {
    QkBuf b = {0};
    while (s < end) {
        char c = *s++;
        if (c == '\\' && s < end) {
            char k = *s++;
            switch (k) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '{': c = '{'; break;
            case '}': c = '}'; break;
            default: fail(p, t, "%s", "unknown escape in text (\\n, \\t, \\\", \\\\, \\{ and \\} are known)"); c = k;
            }
        }
        qkBufByte(&b, (uint8_t)c);
    }
    const char* r = qkDup(b.data ? b.data : "", b.len);
    *len = (int)b.len;
    qkBufFree(&b);
    return r;
}

static Expr* parseExpr(Parser* p);
static void addArg(Expr* e, Expr* arg, const char* name);

/* A text; with {expressions} inside, an E_INTERP: the pieces around them, and each expression parsed. */
static Expr* parseText(Parser* p, Token* t) {
    const char* s = t->start + 1;
    const char* end = t->start + t->len - (t->len > 1 && t->start[t->len - 1] == '"' ? 1 : 0);
    const char* open = NULL;
    for (const char* q = s; q < end; q++) {
        if (*q == '\\') {
            q++;
            continue;
        }
        if (*q == '{') {
            open = q;
            break;
        }
    }
    if (!open) {
        Expr* e = qkNewExpr(E_TEXT, t->line, t->col);
        e->text = decodePiece(p, t, s, end, &e->textLen);
        e->raw = tokText(t);
        return e;
    }
    Expr* e = qkNewExpr(E_INTERP, t->line, t->col);
    const char* parts[64];
    const char* raws[64];
    int count = 0;
    const char* piece = s;
    const char* q = s;
    while (q < end && count < 63) {
        if (*q == '\\') {
            q += 2;
            continue;
        }
        if (*q != '{') {
            q++;
            continue;
        }
        int len;
        parts[count] = decodePiece(p, t, piece, q, &len);
        raws[count] = qkDup(piece, (size_t)(q - piece));
        count++;
        /* the expression: to the matching brace, texts inside it skipped */
        const char* from = ++q;
        int depth = 1;
        while (q < end && depth) {
            if (*q == '"') {
                for (q++; q < end && *q != '"'; q++)
                    if (*q == '\\') q++;
            } else if (*q == '{') {
                depth++;
            } else if (*q == '}') {
                depth--;
                if (!depth) break;
            }
            q++;
        }
        if (q >= end) {
            fail(p, t, "%s", "a { in a text needs its } (write \\{ for the character)");
            return e;
        }
        QkSource* sub = qkNew(sizeof *sub);
        *sub = (QkSource){p->src->path, qkDup(from, (size_t)(q - from)), (size_t)(q - from)};
        Parser inner = {sub, {0}, 0, 0, false, false};
        qkLex(sub, &inner.lx);
        for (int k = 0; k < inner.lx.count; k++) { /* point errors at the text's line */
            inner.lx.tokens[k].line = t->line;
            inner.lx.tokens[k].col += t->col + (int)(from - t->start) - 1;
        }
        Expr* x = parseExpr(&inner);
        if (inner.lx.tokens[inner.i].kind != T_EOF && inner.lx.tokens[inner.i].kind != T_NEWLINE)
            fail(&inner, &inner.lx.tokens[inner.i], "%s", "one expression goes between { and }");
        if (inner.failed) p->failed = true;
        addArg(e, x, NULL);
        piece = ++q;
    }
    int len;
    parts[count] = decodePiece(p, t, piece, end, &len);
    raws[count] = qkDup(piece, (size_t)(end - piece));
    count++;
    e->parts = qkNew(sizeof(char*) * (size_t)count);
    e->rawParts = qkNew(sizeof(char*) * (size_t)count);
    memcpy(e->parts, parts, sizeof(char*) * (size_t)count);
    memcpy(e->rawParts, raws, sizeof(char*) * (size_t)count);
    return e;
}

static Expr* parseIf(Parser* p) {
    Token* t = next(p); /* if */
    Expr* e = qkNewExpr(E_IF, t->line, t->col);
    e->a = parseExpr(p);
    e->thenBlock = parseBlock(p);
    if (accept(p, T_ELSE)) {
        if (at(p, T_IF)) e->elseExpr = parseIf(p);
        else e->elseExpr = blockExpr(p);
    }
    return e;
}

static Expr* parseMatch(Parser* p) {
    Token* t = next(p); /* match */
    Expr* e = qkNewExpr(E_MATCH, t->line, t->col);
    e->a = parseExpr(p);
    expect(p, T_LBRACE, "to start the match's arms");
    skipNewlines(p);
    while (!at(p, T_RBRACE) && !at(p, T_EOF) && !p->failed) {
        Arm arm = {0};
        Token* pt = peek(p);
        arm.comment = joinComments(p, pt->line);
        if (accept(p, T_UNDERSCORE)) {
        } else if (at(p, T_TYPENAME)) {
            Token* name = next(p);
            if (accept(p, T_DOT)) name = expect(p, T_TYPENAME, "after the enum's name");
            arm.pattern = tokText(name);
            if (accept(p, T_LPAREN)) { /* Circle(r, _): names for the case's data */
                const char* binds[32];
                while (!at(p, T_RPAREN) && !at(p, T_EOF) && arm.bindCount < 32) {
                    Token* b = peek(p);
                    if (b->kind != T_NAME && b->kind != T_UNDERSCORE) {
                        fail(p, b, "%s", "expected a name (or _) for the case's data");
                        break;
                    }
                    next(p);
                    binds[arm.bindCount++] = b->kind == T_UNDERSCORE ? "_" : tokText(b);
                    if (!accept(p, T_COMMA)) break;
                }
                expect(p, T_RPAREN, "after the names");
                arm.binds = qkNew(sizeof(char*) * (size_t)(arm.bindCount ? arm.bindCount : 1));
                memcpy(arm.binds, binds, sizeof(char*) * (size_t)arm.bindCount);
            }
        } else if (at(p, T_INT) || (at(p, T_MINUS) && peekAt(p, 1)->kind == T_INT)) {
            bool negative = accept(p, T_MINUS);
            Token* num = next(p);
            Expr* lit = qkNewExpr(E_INT, num->line, num->col);
            lit->i = parseIntText(p, num);
            if (negative) lit->i = -lit->i;
            lit->raw = negative ? qkDup(num->start - 1, (size_t)num->len + 1) : tokText(num);
            arm.intPattern = lit;
        } else {
            fail(p, pt, "%s", "a match arm starts with an enum case, an Int, or _");
            break;
        }
        expect(p, T_FATARROW, "after the arm's pattern");
        arm.body = at(p, T_LBRACE) ? blockExpr(p) : parseExpr(p);
        QK_PUSH(e->arms, e->armCount, e->armCap, arm);
        if (!accept(p, T_COMMA) && !at(p, T_NEWLINE) && !at(p, T_RBRACE)) {
            fail(p, peek(p), "%s", "match arms are separated by commas or new lines");
            break;
        }
        skipNewlines(p);
    }
    expect(p, T_RBRACE, "to close the match");
    return e;
}

static Expr* parsePrimary(Parser* p) {
    Token* t = peek(p);
    if (lambdaAhead(p)) return parseLambda(p);
    switch (t->kind) {
    case T_INT: {
        next(p);
        Expr* e = qkNewExpr(E_INT, t->line, t->col);
        e->i = parseIntText(p, t);
        e->raw = tokText(t);
        return e;
    }
    case T_FLOAT: {
        next(p);
        Expr* e = qkNewExpr(E_FLOAT, t->line, t->col);
        e->f = parseFloatText(t);
        e->raw = tokText(t);
        return e;
    }
    case T_TEXT:
        next(p);
        return parseText(p, t);
    case T_TRUE: case T_FALSE: {
        next(p);
        Expr* e = qkNewExpr(E_BOOL, t->line, t->col);
        e->i = t->kind == T_TRUE;
        return e;
    }
    case T_NAME: case T_TYPENAME: {
        next(p);
        Expr* e = qkNewExpr(E_NAME, t->line, t->col);
        e->name = tokText(t);
        return e;
    }
    case T_LPAREN: {
        next(p);
        Expr* e = parseExpr(p);
        expect(p, T_RPAREN, "to close the parenthesis");
        return e;
    }
    case T_LBRACKET: {
        next(p);
        Expr* e = qkNewExpr(E_ARRAY, t->line, t->col);
        while (!at(p, T_RBRACKET) && !at(p, T_EOF)) {
            addArg(e, parseExpr(p), NULL);
            if (!accept(p, T_COMMA)) break;
        }
        expect(p, T_RBRACKET, "to close the array");
        return e;
    }
    case T_IF: return parseIf(p);
    case T_MATCH: return parseMatch(p);
    case T_LBRACE: return blockExpr(p);
    default: {
        const char* what = qkTokName(t->kind);
        fail(p, t, "expected a value, found \"%s\"", what);
        next(p);
        return qkNewExpr(E_INT, t->line, t->col);
    }
    }
}

static void parseArgs(Parser* p, Expr* call) {
    while (!at(p, T_RPAREN) && !at(p, T_EOF)) {
        const char* name = NULL;
        if (at(p, T_NAME) && peekAt(p, 1)->kind == T_COLON) {
            name = tokText(next(p));
            next(p);
        }
        addArg(call, parseExpr(p), name);
        if (!accept(p, T_COMMA)) break;
    }
    expect(p, T_RPAREN, "to close the call");
}

static Expr* parsePostfix(Parser* p) {
    Expr* e = parsePrimary(p);
    for (;;) {
        Token* t = peek(p);
        if (t->kind == T_LPAREN) {
            next(p);
            Expr* call = qkNewExpr(E_CALL, e->line, e->col);
            if (e->kind == E_NAME) {
                call->name = e->name;
            } else if (e->kind == E_FIELD && e->a->kind == E_NAME && e->name[0] >= 'A' && e->name[0] <= 'Z') {
                call->name = e->name; /* Shape.Circle(2): a case with data */
                call->qualifier = e->a->name;
            } else {
                fail(p, t, "%s", "only a function, a type or an enum case can be called");
            }
            parseArgs(p, call);
            e = call;
        } else if (t->kind == T_LBRACKET) {
            next(p);
            Expr* ix = qkNewExpr(E_INDEX, t->line, t->col);
            ix->a = e;
            ix->b = parseExpr(p);
            expect(p, T_RBRACKET, "to close the index");
            e = ix;
        } else if (t->kind == T_DOT) {
            next(p);
            Token* name = peek(p);
            if (name->kind != T_NAME && name->kind != T_TYPENAME) {
                fail(p, name, "%s", "expected a name after \".\"");
                return e;
            }
            next(p);
            if (at(p, T_LPAREN) && name->kind == T_TYPENAME && e->kind == E_NAME) { /* Shape.Circle(2): a case with data */
                next(p);
                Expr* call = qkNewExpr(E_CALL, e->line, e->col);
                call->name = tokText(name);
                call->qualifier = e->name;
                parseArgs(p, call);
                e = call;
            } else if (at(p, T_LPAREN)) { /* a.f(b) is f(a, b) */
                next(p);
                Expr* call = qkNewExpr(E_CALL, name->line, name->col);
                call->name = tokText(name);
                call->method = true;
                addArg(call, e, NULL);
                parseArgs(p, call);
                e = call;
            } else {
                Expr* f = qkNewExpr(E_FIELD, name->line, name->col);
                f->a = e;
                f->name = tokText(name);
                e = f;
            }
        } else if (t->kind == T_WITH) { /* ship with { laps: ship.laps + 1 } */
            next(p);
            Expr* w = qkNewExpr(E_WITH, t->line, t->col);
            w->a = e;
            expect(p, T_LBRACE, "after with (the fields that change)");
            skipNewlines(p);
            while (!at(p, T_RBRACE) && !at(p, T_EOF) && !p->failed) {
                Token* field = expect(p, T_NAME, "for a field (a camelCase name)");
                expect(p, T_COLON, "after the field's name");
                addArg(w, parseExpr(p), tokText(field));
                if (!accept(p, T_COMMA) && !at(p, T_NEWLINE) && !at(p, T_RBRACE)) {
                    fail(p, peek(p), "%s", "separate the fields with commas or new lines");
                    break;
                }
                skipNewlines(p);
            }
            expect(p, T_RBRACE, "to close the fields");
            e = w;
        } else {
            return e;
        }
    }
}

static Expr* parseUnary(Parser* p) {
    Token* t = peek(p);
    if (t->kind == T_MINUS) {
        next(p);
        Expr* operand = parseUnary(p);
        if (operand->kind == E_INT && operand->raw && operand->raw[0] != '-') { /* a negative literal */
            operand->i = -operand->i;
            QkBuf b = {0};
            qkBufPrintf(&b, "-%s", operand->raw);
            operand->raw = qkDup(b.data, b.len);
            qkBufFree(&b);
            operand->line = t->line;
            operand->col = t->col;
            return operand;
        }
        if (operand->kind == E_FLOAT && operand->raw && operand->raw[0] != '-') {
            operand->f = -operand->f;
            QkBuf b = {0};
            qkBufPrintf(&b, "-%s", operand->raw);
            operand->raw = qkDup(b.data, b.len);
            qkBufFree(&b);
            return operand;
        }
        Expr* e = qkNewExpr(E_UNARY, t->line, t->col);
        e->op = T_MINUS;
        e->a = operand;
        return e;
    }
    return parsePostfix(p);
}

static Expr* binary(Token* t, Expr* a, Expr* b) {
    Expr* e = qkNewExpr(t->kind == T_DOTDOT ? E_RANGE : E_BINARY, a->line, a->col);
    e->op = t->kind;
    e->a = a;
    e->b = b;
    return e;
}

static Expr* parseMul(Parser* p) {
    Expr* e = parseUnary(p);
    while (at(p, T_STAR) || at(p, T_SLASH) || at(p, T_PERCENT)) {
        Token* t = next(p);
        e = binary(t, e, parseUnary(p));
    }
    return e;
}

static Expr* parseAdd(Parser* p) {
    Expr* e = parseMul(p);
    while (at(p, T_PLUS) || at(p, T_MINUS)) {
        Token* t = next(p);
        e = binary(t, e, parseMul(p));
    }
    return e;
}

static Expr* parseRange(Parser* p) {
    Expr* e = parseAdd(p);
    if (at(p, T_DOTDOT)) {
        Token* t = next(p);
        e = binary(t, e, parseAdd(p));
    }
    return e;
}

static Expr* parseCompare(Parser* p) {
    Expr* e = parseRange(p);
    TokKind k = peek(p)->kind;
    if (k == T_EQ || k == T_NE || k == T_LT || k == T_LE || k == T_GT || k == T_GE) {
        Token* t = next(p);
        e = binary(t, e, parseRange(p));
        k = peek(p)->kind;
        if (k == T_EQ || k == T_NE || k == T_LT || k == T_LE || k == T_GT || k == T_GE)
            fail(p, peek(p), "%s", "comparisons do not chain: write a < b and b < c");
    }
    return e;
}

static Expr* parseNot(Parser* p) {
    if (at(p, T_NOT)) {
        Token* t = next(p);
        Expr* e = qkNewExpr(E_UNARY, t->line, t->col);
        e->op = T_NOT;
        e->a = parseNot(p);
        return e;
    }
    return parseCompare(p);
}

static Expr* parseAnd(Parser* p) {
    Expr* e = parseNot(p);
    while (at(p, T_AND)) {
        Token* t = next(p);
        e = binary(t, e, parseNot(p));
    }
    return e;
}

static Expr* parseOr(Parser* p) {
    Expr* e = parseAnd(p);
    while (at(p, T_OR)) {
        Token* t = next(p);
        e = binary(t, e, parseAnd(p));
    }
    return e;
}

/* a ?? b: the value inside an optional, or b */
static Expr* parseExpr(Parser* p) {
    Expr* e = parseOr(p);
    while (at(p, T_QQ)) {
        Token* t = next(p);
        e = binary(t, e, parseOr(p));
    }
    return e;
}

/* ---- statements ---------------------------------------------------------------------------------- */

static void endStatement(Parser* p, const char* what) {
    if (at(p, T_NEWLINE)) {
        skipNewlines(p);
        return;
    }
    if (at(p, T_RBRACE) || at(p, T_EOF)) return;
    fail(p, peek(p), "expected a new line after %s", what);
}

static Stmt* parseStmt(Parser* p) {
    Token* t = peek(p);
    Stmt* s = qkNew(sizeof *s);
    s->line = t->line;
    s->col = t->col;
    s->blankBefore = t->blankBefore;
    s->leading = joinComments(p, t->line);
    if (at(p, T_LET) || at(p, T_VAR)) {
        s->kind = at(p, T_LET) ? S_LET : S_VAR;
        next(p);
        s->name = tokText(expect(p, T_NAME, "after let / var (a camelCase name)"));
        if (accept(p, T_COLON)) s->typeAnn = parseType(p);
        expect(p, T_ASSIGN, "after the name");
        s->value = parseExpr(p);
    } else if (accept(p, T_FOR)) {
        s->kind = S_FOR;
        s->name = tokText(expect(p, T_NAME, "after for"));
        if (accept(p, T_COMMA)) s->indexName = tokText(expect(p, T_NAME, "for the index"));
        expect(p, T_IN, "in a for loop");
        s->iter = parseExpr(p);
        s->body = parseBlock(p);
    } else if (accept(p, T_WHILE)) {
        s->kind = S_WHILE;
        s->value = parseExpr(p);
        s->body = parseBlock(p);
    } else if (accept(p, T_RETURN)) {
        s->kind = S_RETURN;
        if (!at(p, T_NEWLINE) && !at(p, T_RBRACE)) s->value = parseExpr(p);
    } else if (accept(p, T_BREAK)) {
        s->kind = S_BREAK;
    } else if (accept(p, T_CONTINUE)) {
        s->kind = S_CONTINUE;
    } else {
        Expr* e = parseExpr(p);
        TokKind k = peek(p)->kind;
        if (k == T_ASSIGN || k == T_PLUSEQ || k == T_MINUSEQ || k == T_STAREQ || k == T_SLASHEQ) {
            next(p);
            s->kind = S_ASSIGN;
            s->target = e;
            s->op = k;
            s->value = parseExpr(p);
        } else {
            s->kind = S_EXPR;
            s->value = e;
        }
    }
    s->trailing = trailingOn(p, lastLine(p));
    endStatement(p, "a statement");
    return s;
}

static Block* parseBlock(Parser* p) {
    Block* b = qkNew(sizeof *b);
    expect(p, T_LBRACE, "to start a block");
    if (at(p, T_NEWLINE)) skipNewlines(p);
    while (!at(p, T_RBRACE) && !at(p, T_EOF) && !p->failed) {
        Stmt* s = parseStmt(p);
        QK_PUSH(b->stmts, b->count, b->cap, s);
    }
    b->closingComment = joinComments(p, peek(p)->line);
    expect(p, T_RBRACE, "to close the block");
    return b;
}

/* ---- declarations -------------------------------------------------------------------------------- */

static Decl* parseFn(Parser* p) {
    Token* t = next(p); /* fn */
    Decl* d = qkNew(sizeof *d);
    d->kind = D_FN;
    d->isExtern = p->externNext;
    p->externNext = false;
    d->line = t->line;
    d->col = t->col;
    Token* name = expect(p, T_NAME, "after fn (a camelCase name)");
    d->name = tokText(name);
    expect(p, T_LPAREN, "after the function's name");
    while (!at(p, T_RPAREN) && !at(p, T_EOF)) {
        Token* pn = expect(p, T_NAME, "for a parameter (a camelCase name)");
        expect(p, T_COLON, "after a parameter's name (every parameter has a type)");
        Param prm = {tokText(pn), parseType(p), pn->line, pn->col, NULL, NULL};
        if (accept(p, T_ASSIGN)) prm.fallback = parseExpr(p); /* a default value */
        QK_PUSH(d->params, d->paramCount, d->paramCap, prm);
        if (!accept(p, T_COMMA)) break;
    }
    expect(p, T_RPAREN, "after the parameters");
    if (accept(p, T_ARROW)) d->ret = parseType(p);
    if (d->isExtern) return d; /* the host's: no body */
    if (accept(p, T_ASSIGN)) {
        d->body = parseExpr(p);
    } else {
        d->body = blockExpr(p);
    }
    return d;
}

/* effect fn ... (a function with effects) */
static Decl* parseEffect(Parser* p, bool external) {
    Token* t = next(p); /* effect */
    if (!at(p, T_FN)) {
        fail(p, peek(p), "%s", "expected fn after effect");
        Decl* d = qkNew(sizeof *d);
        d->name = "";
        return d;
    }
    p->externNext = external;
    Decl* d = parseFn(p);
    d->isEffect = true;
    d->line = t->line;
    d->col = t->col;
    return d;
}

/* let name = value, at the top: a constant */
static Decl* parseConst(Parser* p) {
    Token* t = next(p); /* let */
    Decl* d = qkNew(sizeof *d);
    d->kind = D_CONST;
    d->line = t->line;
    d->col = t->col;
    d->name = tokText(expect(p, T_NAME, "after let (a camelCase name)"));
    if (accept(p, T_COLON)) d->ret = parseType(p);
    expect(p, T_ASSIGN, "after the constant's name");
    d->body = parseExpr(p);
    return d;
}

/* extern fn name(params) -> Result   |   extern effect fn ...   |   extern type Name */
static Decl* parseExtern(Parser* p) {
    Token* t = next(p); /* extern */
    if (at(p, T_EFFECT)) {
        Decl* d = parseEffect(p, true);
        d->line = t->line;
        d->col = t->col;
        return d;
    }
    if (at(p, T_FN)) {
        p->externNext = true;
        Decl* d = parseFn(p);
        d->line = t->line;
        d->col = t->col;
        return d;
    }
    expect(p, T_TYPE, "after extern (extern fn or extern type)");
    Decl* d = qkNew(sizeof *d);
    d->kind = D_TYPE;
    d->isExtern = true;
    d->line = t->line;
    d->col = t->col;
    d->name = tokText(expect(p, T_TYPENAME, "after extern type (a PascalCase name)"));
    return d;
}

static Decl* parseTypeDecl(Parser* p) {
    Token* t = next(p);
    Decl* d = qkNew(sizeof *d);
    d->kind = t->kind == T_TYPE ? D_TYPE : D_ENUM;
    d->line = t->line;
    d->col = t->col;
    d->name = tokText(expect(p, T_TYPENAME, "after type / enum (a PascalCase name)"));
    expect(p, T_LBRACE, "to list its fields or cases");
    skipNewlines(p);
    while (!at(p, T_RBRACE) && !at(p, T_EOF) && !p->failed) {
        if (d->kind == D_TYPE) {
            Token* fn = expect(p, T_NAME, "for a field (a camelCase name)");
            expect(p, T_COLON, "after the field's name");
            Param f = {tokText(fn), parseType(p), fn->line, fn->col, NULL};
            QK_PUSH(d->params, d->paramCount, d->paramCap, f);
        } else {
            const char* c = tokText(expect(p, T_TYPENAME, "for a case (a PascalCase name)"));
            int k = d->caseCount;
            QK_PUSH(d->cases, d->caseCount, d->caseCap, c);
            if (d->caseCount > 64) {
                fail(p, peek(p), "%s", "an enum has at most 64 cases");
                break;
            }
            if (!d->caseFields) {
                d->caseFields = qkNew(sizeof(Param*) * 64);
                d->caseFieldCount = qkNew(sizeof(int) * 64);
            }
            if (accept(p, T_LPAREN)) { /* Circle(radius: Float): the case carries data */
                Param fields[32];
                int n = 0;
                while (!at(p, T_RPAREN) && !at(p, T_EOF) && n < 32) {
                    Token* fn = expect(p, T_NAME, "for the case's data (a camelCase name)");
                    expect(p, T_COLON, "after its name");
                    fields[n++] = (Param){tokText(fn), parseType(p), fn->line, fn->col, NULL, NULL};
                    if (!accept(p, T_COMMA)) break;
                }
                expect(p, T_RPAREN, "after the case's data");
                d->caseFields[k] = qkNew(sizeof(Param) * (size_t)(n ? n : 1));
                memcpy(d->caseFields[k], fields, sizeof(Param) * (size_t)n);
                d->caseFieldCount[k] = n;
            }
        }
        if (!accept(p, T_COMMA) && !at(p, T_NEWLINE) && !at(p, T_RBRACE)) {
            fail(p, peek(p), "%s", "separate them with commas or new lines");
            break;
        }
        skipNewlines(p);
    }
    expect(p, T_RBRACE, "to close the list");
    return d;
}

bool qkParse(const QkSource* src, Program* out) {
    memset(out, 0, sizeof *out);
    out->src = *src;
    Parser p = {src, {0}, 0, 0, false, false};
    int errorsBefore = qkErrorCount;
    qkLex(src, &p.lx);
    if (qkErrorCount > errorsBefore) return false;
    skipNewlines(&p);
    while (!at(&p, T_EOF) && !p.failed) {
        Token* t = peek(&p);
        const char* leading = joinComments(&p, t->line);
        Decl* d = NULL;
        if (at(&p, T_FN)) d = parseFn(&p);
        else if (at(&p, T_TYPE) || at(&p, T_ENUM)) d = parseTypeDecl(&p);
        else if (at(&p, T_EXTERN)) d = parseExtern(&p);
        else if (at(&p, T_EFFECT)) d = parseEffect(&p, false);
        else if (at(&p, T_LET)) d = parseConst(&p);
        else {
            fail(&p, t, "%s", "expected fn, effect fn, let, type, enum or extern at the top of the file");
            break;
        }
        d->leading = leading;
        d->trailing = trailingOn(&p, lastLine(&p));
        d->index = out->count;
        QK_PUSH(out->decls, out->count, out->cap, d);
        if (!at(&p, T_EOF)) expect(&p, T_NEWLINE, "after a declaration");
        skipNewlines(&p);
    }
    out->closingComment = joinComments(&p, 1 << 30);
    return !p.failed && qkErrorCount == errorsBefore;
}
