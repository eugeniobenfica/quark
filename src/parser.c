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
    const char* aliases[32]; /* use geometry as g: g.area is geometry's area */
    int aliasCount;
    bool typeText; /* a type alone (from a graph): any a.Type is a module's */
} Parser;

static bool isAlias(const Parser* p, const char* s, int n) {
    for (int i = 0; i < p->aliasCount; i++)
        if ((int)strlen(p->aliases[i]) == n && !memcmp(p->aliases[i], s, (size_t)n)) return true;
    return false;
}

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
        if (accept(p, T_COLON)) { /* [K: V]: a map */
            te->key = te->elem;
            te->elem = parseType(p);
        }
        expect(p, T_RBRACKET, te->key ? "after a map's value type" : "after an array's element type");
    } else if (t->kind == T_LPAREN || (t->kind == T_EFFECT && peekAt(p, 1)->kind == T_LPAREN)) {
        /* (A, B) -> R: a function; effect (A) -> R: one that changes the world */
        te->effect = accept(p, T_EFFECT);
        next(p);
        TypeExpr* params[16];
        int n = 0;
        while (!at(p, T_RPAREN) && !at(p, T_EOF) && n < 16) {
            params[n++] = parseType(p);
            if (!accept(p, T_COMMA)) break;
        }
        expect(p, T_RPAREN, "after a function type's parameters");
        if (!te->effect && n >= 2 && !at(p, T_ARROW)) { /* (Int, Text): a tuple */
            te->isTuple = true;
            te->params = qkNew(sizeof(TypeExpr*) * (size_t)n);
            memcpy(te->params, params, sizeof(TypeExpr*) * (size_t)n);
            te->paramCount = n;
            goto suffix;
        }
        expect(p, T_ARROW, "in a function type: (A, B) -> Result");
        te->isFn = true;
        te->params = qkNew(sizeof(TypeExpr*) * (size_t)(n ? n : 1));
        memcpy(te->params, params, sizeof(TypeExpr*) * (size_t)n);
        te->paramCount = n;
        te->elem = parseType(p);
        return te;
    } else if (t->kind == T_NAME && peekAt(p, 1)->kind == T_DOT && peekAt(p, 2)->kind == T_TYPENAME && (p->typeText || isAlias(p, t->start, t->len))) {
        next(p); /* g.Vec2: a type of the module g stands for */
        next(p);
        Token* tn = next(p);
        QkBuf q = {0};
        qkBufPrintf(&q, "%.*s.%.*s", t->len, t->start, tn->len, tn->start);
        te->name = qkDup(q.data, q.len);
        qkBufFree(&q);
    } else if (t->kind != T_TYPENAME) {
        fail(p, t, "%s", "expected a type (Int, Float, Bool, Text, a type's name, [Type], [Key: Value], Type? or Type!)");
        te->name = "Int";
        return te;
    } else {
        next(p);
        te->name = tokText(t);
        if (accept(p, T_LT)) { /* Pair<Int, Text>: a generic type, given its types */
            TypeExpr* args[16];
            int n = 0;
            while (!at(p, T_GT) && !at(p, T_EOF) && n < 16) {
                args[n++] = parseType(p);
                if (!accept(p, T_COMMA)) break;
            }
            expect(p, T_GT, "after the type's types: Pair<A, B>");
            te->args = qkNew(sizeof(TypeExpr*) * (size_t)(n ? n : 1));
            memcpy(te->args, args, sizeof(TypeExpr*) * (size_t)n);
            te->argCount = n;
        }
    }
suffix:
    if (at(p, T_QUESTION) || at(p, T_BANG)) { /* T?: a T, or None; T!: a T, or an error */
        bool fallible = at(p, T_BANG);
        next(p);
        TypeExpr* inner = qkNew(sizeof *inner);
        *inner = *te;
        te = qkNew(sizeof *te);
        te->line = inner->line;
        te->col = inner->col;
        te->elem = inner;
        te->optional = !fallible;
        te->fallible = fallible;
        if (fallible && at(p, T_TYPENAME) && peek(p)->line == p->lx.tokens[p->i - 1].line) /* Int!ParseError, Int ! ParseError */
            te->errType = parseType(p);
    }
    return te;
}

/* An interface's function: fn speak(loud: Bool) -> Text, effect fn log(line: Text). A field of a function type,
 * its values' names kept for the text. */
static Param parseMember(Parser* p) {
    Token* t = peek(p);
    TypeExpr* te = qkNew(sizeof *te);
    te->line = t->line;
    te->col = t->col;
    te->isFn = true;
    te->effect = accept(p, T_EFFECT);
    expect(p, T_FN, te->effect ? "after effect (an interface's effect fn)" : "for an interface's function");
    Token* name = expect(p, T_NAME, "after fn (the function's camelCase name)");
    expect(p, T_LPAREN, "after the function's name");
    TypeExpr* params[16];
    const char* names[16];
    int n = 0;
    while (!at(p, T_RPAREN) && !at(p, T_EOF) && n < 16) {
        names[n] = tokText(expect(p, T_NAME, "for a value (a camelCase name)"));
        expect(p, T_COLON, "after the value's name");
        params[n++] = parseType(p);
        if (!accept(p, T_COMMA)) break;
    }
    expect(p, T_RPAREN, "after the function's values");
    te->paramCount = n;
    te->params = qkNew(sizeof(TypeExpr*) * (size_t)(n ? n : 1));
    te->paramNames = qkNew(sizeof(char*) * (size_t)(n ? n : 1));
    memcpy(te->params, params, sizeof(TypeExpr*) * (size_t)n);
    memcpy(te->paramNames, names, sizeof(char*) * (size_t)n);
    if (accept(p, T_ARROW)) te->elem = parseType(p); /* none: it gives nothing */
    return (Param){tokText(name), te, name->line, name->col, NULL, NULL, false};
}

Param qkParseMemberText(const char* text) {
    QkSource* src = qkNew(sizeof *src);
    *src = (QkSource){"type", text ? text : "fn f()", text ? strlen(text) : 6};
    Parser p = {src, {0}, 0, 0, false, false};
    p.typeText = true;
    qkLex(src, &p.lx);
    return parseMember(&p);
}

TypeExpr* qkParseTypeText(const char* text) {
    QkSource* src = qkNew(sizeof *src);
    *src = (QkSource){"type", text ? text : "Int", text ? strlen(text) : 3};
    Parser p = {src, {0}, 0, 0, false, false};
    p.typeText = true;
    qkLex(src, &p.lx);
    return parseType(&p);
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
        if (peekAt(p, k)->kind == T_COLON) { /* (x: Int) => ...: a type, to the comma or the parenthesis */
            int depth = 0;
            for (k++;; k++) {
                TokKind kk = peekAt(p, k)->kind;
                if (kk == T_EOF || kk == T_NEWLINE) return false;
                if (kk == T_LPAREN || kk == T_LBRACKET || kk == T_LT) depth++;
                else if ((kk == T_RPAREN || kk == T_RBRACKET || kk == T_GT) && depth) depth--;
                else if ((kk == T_COMMA || kk == T_RPAREN) && !depth) break;
            }
        }
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
            TypeExpr* type = accept(p, T_COLON) ? parseType(p) : NULL; /* (x: Int) => ... */
            if (n < 8) params[n++] = (Param){tokText(name), type, name->line, name->col, NULL};
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
            case 'r': c = '\r'; break;
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '{': c = '{'; break;
            case '}': c = '}'; break;
            default: fail(p, t, "%s", "unknown escape in text (\\n, \\t, \\r, \\\", \\\\, \\{ and \\} are known)"); c = k;
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
/* """ ... """: the lines between the quotes, each without the closing quotes' indentation, joined by line breaks
 * (none after the last). The opening quotes end their line; the closing ones stand on a line of their own. */
static bool multilineText(Parser* p, Token* t, const char** s, const char** end) {
    const char* open = t->start + 3;
    const char* close = t->start + t->len - 3;
    const char* q = open;
    while (q < close && (*q == ' ' || *q == '\t' || *q == '\r')) q++;
    if (q >= close || *q != '\n') {
        fail(p, t, "%s", "the text starts on the line after its \"\"\"");
        return false;
    }
    const char* first = q + 1;
    const char* lastBreak = close;
    while (lastBreak > first && lastBreak[-1] != '\n') lastBreak--;
    for (const char* r = lastBreak; r < close; r++)
        if (*r != ' ' && *r != '\t') {
            fail(p, t, "%s", "the closing \"\"\" stands on a line of its own");
            return false;
        }
    int indent = (int)(close - lastBreak);
    QkBuf b = {0};
    for (const char* line = first; line < lastBreak;) {
        const char* nl = memchr(line, '\n', (size_t)(lastBreak - line));
        const char* stop = nl ? nl : lastBreak;
        const char* text = line;
        int strip = 0;
        while (strip < indent && text < stop && (*text == ' ' || *text == '\t')) text++, strip++;
        const char* tail = stop;
        while (tail > text && tail[-1] == '\r') tail--;
        if (strip < indent && text < tail) {
            fail(p, t, "%s", "each line of the text starts at least as far in as the closing \"\"\"");
            qkBufFree(&b);
            return false;
        }
        if (line != first) qkBufByte(&b, '\n');
        qkBufAdd(&b, text, (size_t)(tail - text));
        line = stop + 1;
    }
    *s = qkDup(b.data ? b.data : "", b.len);
    *end = *s + b.len;
    qkBufFree(&b);
    return true;
}

static Expr* parseText(Parser* p, Token* t) {
    const char* s = t->start + 1;
    const char* end = t->start + t->len - (t->len > 1 && t->start[t->len - 1] == '"' ? 1 : 0);
    bool multiline = t->len >= 6 && !strncmp(t->start, "\"\"\"", 3);
    if (multiline && !multilineText(p, t, &s, &end)) {
        Expr* e = qkNewExpr(E_TEXT, t->line, t->col);
        e->text = "";
        e->raw = "\"\"";
        return e;
    }
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
        e->multiline = multiline;
        if (multiline) { /* as a graph keeps it: the lines, without indentation */
            QkBuf r = {0};
            qkBufPuts(&r, "\"\"\"\n");
            qkBufAdd(&r, s, (size_t)(end - s));
            qkBufPuts(&r, "\n\"\"\"");
            e->raw = qkDup(r.data, r.len);
            qkBufFree(&r);
        } else {
            e->raw = tokText(t);
        }
        return e;
    }
    Expr* e = qkNewExpr(E_INTERP, t->line, t->col);
    e->multiline = multiline;
    const char* parts[64];
    const char* raws[64];
    const char* formats[64];
    bool anyFormat = false;
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
        /* {x:.2} or {n:5}: a format after the last colon outside parentheses, brackets and texts */
        const char* stop = q;
        const char* colon = NULL;
        int nest = 0;
        for (const char* r = from; r < q; r++) {
            if (*r == '"') {
                for (r++; r < q && *r != '"'; r++)
                    if (*r == '\\') r++;
            } else if (*r == '(' || *r == '[' || *r == '{') {
                nest++;
            } else if (*r == ')' || *r == ']' || *r == '}') {
                nest--;
            } else if (*r == ':' && !nest) {
                colon = r;
            }
        }
        formats[count - 1] = NULL;
        if (colon) {
            const char* f = colon + 1;
            bool ok = f < q;
            for (const char* r = f; r < q; r++) ok &= (*r >= '0' && *r <= '9') || *r == '.';
            if (ok) {
                formats[count - 1] = qkDup(f, (size_t)(q - f));
                anyFormat = true;
                stop = colon;
            }
        }
        QkSource* sub = qkNew(sizeof *sub);
        *sub = (QkSource){p->src->path, qkDup(from, (size_t)(stop - from)), (size_t)(stop - from)};
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
    if (anyFormat) {
        e->formats = qkNew(sizeof(char*) * (size_t)count);
        memcpy(e->formats, formats, sizeof(char*) * (size_t)(count - 1));
    }
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

/* 42 or -42, as a pattern */
static Expr* intLiteral(Parser* p) {
    bool negative = accept(p, T_MINUS);
    Token* num = next(p);
    Expr* lit = qkNewExpr(E_INT, num->line, num->col);
    lit->i = parseIntText(p, num);
    if (negative) lit->i = -lit->i;
    lit->raw = negative ? qkDup(num->start - 1, (size_t)num->len + 1) : tokText(num);
    return lit;
}

static Expr* intLiteral(Parser* p);
static void parseCasePattern(Parser* p, Arm* arm);

/* the fields of Circle(...), Point(...) or (...): names, _, or patterns inside */
static void parseFieldPatterns(Parser* p, Arm* arm) {
    const char* binds[32];
    Arm* subs[32];
    bool anySub = false;
    while (!at(p, T_RPAREN) && !at(p, T_EOF) && arm->bindCount < 32) {
        Token* b = peek(p);
        Arm* sub = NULL;
        if (b->kind == T_NAME || b->kind == T_UNDERSCORE) {
            next(p);
        } else if (b->kind == T_TYPENAME || b->kind == T_LPAREN) { /* Some(Circle(r)), ((a, b), c) */
            sub = qkNew(sizeof(Arm));
            parseCasePattern(p, sub);
        } else if (b->kind == T_INT || (b->kind == T_MINUS && peekAt(p, 1)->kind == T_INT)) { /* Point(x, 0) */
            sub = qkNew(sizeof(Arm));
            sub->intPattern = intLiteral(p);
        } else if (b->kind == T_TEXT) {
            sub = qkNew(sizeof(Arm));
            sub->textPattern = parseText(p, next(p));
        } else {
            fail(p, b, "%s", "expected a name, _, or a pattern (Circle(r), 0, \"text\") for the data");
            break;
        }
        anySub |= sub != NULL;
        subs[arm->bindCount] = sub;
        binds[arm->bindCount++] = b->kind == T_NAME ? tokText(b) : "_";
        if (!accept(p, T_COMMA)) break;
    }
    expect(p, T_RPAREN, "after the pattern's fields");
    arm->binds = qkNew(sizeof(char*) * (size_t)(arm->bindCount ? arm->bindCount : 1));
    memcpy(arm->binds, binds, sizeof(char*) * (size_t)arm->bindCount);
    if (anySub) {
        arm->subs = qkNew(sizeof(Arm*) * (size_t)arm->bindCount);
        memcpy(arm->subs, subs, sizeof(Arm*) * (size_t)arm->bindCount);
    }
}

/* Some(x), Shape.Circle(r, _), Empty, Point(x, 0), (q, r): a case or a record and its fields (a match arm, or a
 * let) */
static void parseCasePattern(Parser* p, Arm* arm) {
    if (accept(p, T_LPAREN)) { /* (q, r): a tuple */
        arm->pattern = "(,)";
        parseFieldPatterns(p, arm);
        return;
    }
    Token* name = next(p);
    if (accept(p, T_DOT)) name = expect(p, T_TYPENAME, "after the enum's name");
    arm->pattern = tokText(name);
    if (accept(p, T_LPAREN)) parseFieldPatterns(p, arm); /* Circle(r, _): the case's data */
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
        } else if (at(p, T_TYPENAME) || at(p, T_LPAREN)) {
            parseCasePattern(p, &arm);
        } else if (at(p, T_INT) || (at(p, T_MINUS) && peekAt(p, 1)->kind == T_INT) || at(p, T_DOTDOT)) {
            /* 3, or a range: 80..90 (90 not included), 90.., ..0 */
            if (!at(p, T_DOTDOT)) arm.intPattern = intLiteral(p);
            if (accept(p, T_DOTDOT)) {
                arm.isRange = true;
                if (at(p, T_INT) || (at(p, T_MINUS) && peekAt(p, 1)->kind == T_INT)) arm.intHigh = intLiteral(p);
                if (!arm.intPattern && !arm.intHigh) fail(p, pt, "%s", "a range pattern needs a start or an end: 90.., ..60 or 60..90");
            }
        } else if (at(p, T_TEXT)) {
            Token* tx = next(p);
            arm.textPattern = parseText(p, tx);
            if (arm.textPattern->kind != E_TEXT) fail(p, tx, "%s", "a text pattern is a plain text, without {values}");
        } else {
            fail(p, pt, "%s", "a match arm starts with an enum case, an Int, a range, a text, or _");
            break;
        }
        if (accept(p, T_IF)) arm.guard = parseExpr(p); /* Some(x) if x > 3 => ... */
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
        if (at(p, T_COMMA)) { /* (a, b): a tuple */
            Expr* tuple = qkNewExpr(E_CALL, t->line, t->col);
            tuple->name = "(,)";
            addArg(tuple, e, NULL);
            while (accept(p, T_COMMA) && !at(p, T_RPAREN)) addArg(tuple, parseExpr(p), NULL);
            expect(p, T_RPAREN, "to close the tuple");
            return tuple;
        }
        expect(p, T_RPAREN, "to close the parenthesis");
        return e;
    }
    case T_LBRACKET: {
        next(p);
        Expr* e = qkNewExpr(E_ARRAY, t->line, t->col);
        if (at(p, T_COLON) && peekAt(p, 1)->kind == T_RBRACKET) { /* [:]: an empty map */
            next(p);
            next(p);
            e->kind = E_MAP;
            return e;
        }
        while (!at(p, T_RBRACKET) && !at(p, T_EOF)) {
            addArg(e, parseExpr(p), NULL);
            if (e->argCount == 1 && at(p, T_COLON)) e->kind = E_MAP; /* [key: value, ...] */
            if (e->kind == E_MAP) {
                expect(p, T_COLON, "between a key and its value");
                addArg(e, parseExpr(p), NULL);
            }
            if (!accept(p, T_COMMA)) break;
        }
        expect(p, T_RBRACKET, e->kind == E_MAP ? "to close the map" : "to close the array");
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
            } else { /* pick(true)(5): a function a value gives, called; "()" with it as the first value */
                call->name = "()";
                addArg(call, e, NULL);
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
            if (name->kind != T_NAME && name->kind != T_TYPENAME && name->kind != T_INT) {
                fail(p, name, "%s", "expected a name after \".\"");
                return e;
            }
            next(p);
            if (e->kind == E_NAME && !strchr(e->name, '.') && isAlias(p, e->name, (int)strlen(e->name)) && name->kind != T_INT) {
                QkBuf q = {0}; /* g.area, g.Vec2, g.pi: a name of the module g stands for */
                qkBufPrintf(&q, "%s.%.*s", e->name, name->len, name->start);
                e->name = qkDup(q.data, q.len);
                qkBufFree(&q);
                continue;
            }
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
        } else if (t->kind == T_QUESTION) { /* a?: the value, or leave with the None / Err */
            next(p);
            Expr* q = qkNewExpr(E_TRY, t->line, t->col);
            q->a = e;
            e = q;
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
    if (t->kind == T_TILDE) { /* ~x: every bit of x the other way */
        next(p);
        Expr* e = qkNewExpr(E_UNARY, t->line, t->col);
        e->op = T_TILDE;
        e->a = parseUnary(p);
        return e;
    }
    return parsePostfix(p);
}

static Expr* binary(Token* t, Expr* a, Expr* b) {
    Expr* e = qkNewExpr(t->kind == T_DOTDOT ? E_RANGE : E_BINARY, a ? a->line : t->line, a ? a->col : t->col);
    e->op = t->kind;
    e->a = a;
    e->b = b;
    return e;
}

/* << and >>: two < or > side by side (the lexer keeps them apart, so Pair<A, Tree<B>> closes) */
static TokKind shiftAt(Parser* p) {
    Token* t = peek(p);
    Token* u = peekAt(p, 1);
    if ((t->kind == T_LT || t->kind == T_GT) && u->kind == t->kind && u->start == t->start + 1) return t->kind == T_LT ? T_SHL : T_SHR;
    return T_EOF;
}

/* * / % and the bits that work like them (<< >> &): one level, as in Go and Swift, so a & b == c is (a & b) == c */
static Expr* parseMul(Parser* p) {
    Expr* e = parseUnary(p);
    while (at(p, T_STAR) || at(p, T_SLASH) || at(p, T_PERCENT) || at(p, T_AMP) || shiftAt(p) != T_EOF) {
        TokKind shift = shiftAt(p);
        Token* t = next(p);
        if (shift != T_EOF) next(p);
        Expr* b = parseUnary(p);
        e = binary(t, e, b);
        if (shift != T_EOF) e->op = shift;
    }
    return e;
}

static Expr* parseAdd(Parser* p) {
    Expr* e = parseMul(p);
    while (at(p, T_PLUS) || at(p, T_MINUS) || at(p, T_PIPE) || at(p, T_CARET)) { /* | and ^ work like + */
        Token* t = next(p);
        e = binary(t, e, parseMul(p));
    }
    return e;
}

/* What can follow an open range's .. (xs[2..], 90.. =>): nothing that starts a value. */
static bool rangeEndsHere(Parser* p) {
    TokKind k = peek(p)->kind;
    return k == T_RBRACKET || k == T_RPAREN || k == T_COMMA || k == T_NEWLINE || k == T_FATARROW || k == T_LBRACE ||
           k == T_EOF || k == T_RBRACE;
}

static Expr* parseRange(Parser* p) {
    if (at(p, T_DOTDOT)) { /* ..b: from the start */
        Token* t = next(p);
        Expr* e = qkNewExpr(E_RANGE, t->line, t->col);
        e->op = T_DOTDOT;
        e->b = parseAdd(p);
        return e;
    }
    Expr* e = parseAdd(p);
    if (at(p, T_DOTDOT)) {
        Token* t = next(p);
        e = binary(t, e, rangeEndsHere(p) ? NULL : parseAdd(p)); /* a..: to the end */
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
    if (at(p, T_BANG)) { /* !x: Quark says not x (! is for results, T!) */
        fail(p, peek(p), "%s", "write not x: ! is only for results (Int!)");
        next(p);
    }
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
    if (at(p, T_LET) && (peekAt(p, 1)->kind == T_TYPENAME || peekAt(p, 1)->kind == T_LPAREN)) {
        /* let Some(x) = value else { ... }, let Point(x, y) = p, let (q, r) = divmod(7, 2) */
        next(p);
        s->kind = S_LET;
        s->pattern = qkNew(sizeof(Arm));
        parseCasePattern(p, s->pattern);
        expect(p, T_ASSIGN, "after the pattern");
        s->value = parseExpr(p);
        if (accept(p, T_ELSE)) s->elseBody = parseBlock(p); /* none: the pattern always matches (checked) */
    } else if (at(p, T_LET) || at(p, T_VAR)) {
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
    } else if (accept(p, T_DEFER)) { /* defer { ... }: runs when the block it is in is left, however it is left */
        s->kind = S_DEFER;
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
    if (at(p, T_TYPENAME) && peekAt(p, 1)->kind == T_DOT) { /* fn Vec2.plus: a function of the type Vec2 */
        Token* owner = next(p);
        next(p);
        Token* name = expect(p, T_NAME, "after the type's name and the dot (a camelCase name)");
        QkBuf full = {0};
        qkBufPrintf(&full, "%.*s.%.*s", owner->len, owner->start, name->len, name->start);
        d->name = qkDup(full.data, full.len);
        qkBufFree(&full);
    } else {
        Token* name = expect(p, T_NAME, "after fn (a camelCase name, or Type.name)");
        d->name = tokText(name);
    }
    if (accept(p, T_LT)) { /* fn first<T>(xs: [T]): type parameters */
        const char* names[16];
        const char* bounds[16] = {0};
        bool anyBound = false;
        int n = 0;
        while (!at(p, T_GT) && !at(p, T_EOF) && n < 16) {
            names[n] = tokText(expect(p, T_TYPENAME, "for a type parameter (a PascalCase name, like T)"));
            if (accept(p, T_COLON)) { /* T: Speaker: any type that fits the interface */
                bounds[n] = tokText(expect(p, T_TYPENAME, "after the colon (an interface T must fit, like T: Speaker)"));
                anyBound = true;
            }
            n++;
            if (!accept(p, T_COMMA)) break;
        }
        expect(p, T_GT, "after the type parameters");
        d->typeParams = qkNew(sizeof(char*) * (size_t)(n ? n : 1));
        memcpy(d->typeParams, names, sizeof(char*) * (size_t)n);
        d->typeParamCount = n;
        if (anyBound) {
            d->typeBounds = qkNew(sizeof(char*) * (size_t)n);
            memcpy(d->typeBounds, bounds, sizeof(char*) * (size_t)n);
        }
    }
    expect(p, T_LPAREN, "after the function's name");
    while (!at(p, T_RPAREN) && !at(p, T_EOF)) {
        Token* pn = expect(p, T_NAME, "for a parameter (a camelCase name)");
        bool self = !d->paramCount && strchr(d->name, '.') && pn->len == 4 && !memcmp(pn->start, "self", 4) && !at(p, T_COLON);
        if (!self) expect(p, T_COLON, "after a parameter's name (every parameter has a type)");
        Param prm = {tokText(pn), self ? NULL : parseType(p), pn->line, pn->col, NULL, NULL}; /* fn Vec2.length(self): its type */
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

/* test "it adds up" { ... }: a check quark test runs (it may print, and assert) */
static Decl* parseTest(Parser* p) {
    Token* t = next(p); /* test */
    Token* name = next(p);
    Decl* d = qkNew(sizeof *d);
    d->kind = D_FN;
    d->isTest = true;
    d->isEffect = true;
    d->line = t->line;
    d->col = t->col;
    Expr* title = parseText(p, name);
    if (title->kind != E_TEXT) fail(p, name, "%s", "a test's name is a plain text");
    d->testName = title->text ? title->text : "";
    QkBuf full = {0};
    qkBufPrintf(&full, "test %.*s", name->len, name->start);
    d->name = qkDup(full.data, full.len);
    qkBufFree(&full);
    d->body = blockExpr(p);
    return d;
}

/* use geometry, use shapes.round: the pub declarations of geometry.qk (shapes/round.qk), next to this file */
static Decl* parseUse(Parser* p) {
    Token* t = next(p); /* use */
    Decl* d = qkNew(sizeof *d);
    d->kind = D_USE;
    d->line = t->line;
    d->col = t->col;
    QkBuf name = {0};
    Token* part = expect(p, T_NAME, "after use (a module: the camelCase name of a file, without .qk)");
    qkBufAdd(&name, part->start, (size_t)part->len);
    while (accept(p, T_DOT)) {
        part = expect(p, T_NAME, "after the dot (a folder, then the file: use shapes.round)");
        qkBufPrintf(&name, ".%.*s", part->len, part->start);
    }
    d->name = qkDup(name.data, name.len);
    qkBufFree(&name);
    if (at(p, T_NAME) && peek(p)->len == 2 && !memcmp(peek(p)->start, "as", 2)) { /* use geometry as g */
        next(p);
        d->alias = tokText(expect(p, T_NAME, "after as (a camelCase name for the module)"));
        if (p->aliasCount < 32) p->aliases[p->aliasCount++] = d->alias;
    }
    return d;
}

/* const name = value, at the top: a constant (pure, worked out when the program is compiled) */
static Decl* parseConst(Parser* p) {
    Token* t = next(p); /* const */
    Decl* d = qkNew(sizeof *d);
    d->kind = D_CONST;
    d->line = t->line;
    d->col = t->col;
    d->name = tokText(expect(p, T_NAME, "after const (a camelCase name)"));
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
    if (accept(p, T_LT)) { /* type Pair<A, B>: generic, its types given where it is used */
        const char* names[16];
        int n = 0;
        while (!at(p, T_GT) && !at(p, T_EOF) && n < 16) {
            names[n++] = tokText(expect(p, T_TYPENAME, "for a type parameter (a PascalCase name, like T)"));
            if (!accept(p, T_COMMA)) break;
        }
        expect(p, T_GT, "after the type parameters");
        d->typeParams = qkNew(sizeof(char*) * (size_t)(n ? n : 1));
        memcpy(d->typeParams, names, sizeof(char*) * (size_t)n);
        d->typeParamCount = n;
    }
    expect(p, T_LBRACE, "to list its fields or cases");
    skipNewlines(p);
    while (!at(p, T_RBRACE) && !at(p, T_EOF) && !p->failed) {
        if (d->kind == D_TYPE) {
            bool member = at(p, T_FN) || at(p, T_EFFECT); /* type Speaker { fn speak() -> Text }: an interface */
            if (d->paramCount && member != d->isInterface) {
                fail(p, peek(p), "%s", "an interface lists functions (fn name(...) -> Type), a record fields (name: Type): not both");
                break;
            }
            d->isInterface = member;
            if (member) {
                Param f = parseMember(p);
                QK_PUSH(d->params, d->paramCount, d->paramCap, f);
            } else {
                Token* fn = expect(p, T_NAME, "for a field (a camelCase name), or fn for an interface's function");
                expect(p, T_COLON, "after the field's name");
                Param f = {tokText(fn), parseType(p), fn->line, fn->col, NULL};
                QK_PUSH(d->params, d->paramCount, d->paramCap, f);
            }
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
        bool pub = accept(&p, T_PUB);
        if (pub && !at(&p, T_FN) && !at(&p, T_TYPE) && !at(&p, T_ENUM) && !at(&p, T_EFFECT) && !at(&p, T_CONST)) {
            fail(&p, peek(&p), "%s", "pub goes before fn, type, enum or const");
            break;
        }
        if (at(&p, T_USE)) d = parseUse(&p);
        else if (at(&p, T_FN)) d = parseFn(&p);
        else if (at(&p, T_TYPE) || at(&p, T_ENUM)) d = parseTypeDecl(&p);
        else if (at(&p, T_EXTERN)) d = parseExtern(&p);
        else if (at(&p, T_EFFECT)) { /* a function's effects are worked out from its body */
            fail(&p, t, "%s", "effects are inferred: write fn, without effect (it is written only where there is no body: extern effect fn, effect (A) -> R)");
            break;
        }
        else if (at(&p, T_CONST)) d = parseConst(&p);
        else if (at(&p, T_LET)) { /* a name for a value inside a function; at the top, a constant */
            fail(&p, t, "%s", "at the top of a file a constant is written const: const name = value (let names a value inside a function)");
            break;
        }
        else if (at(&p, T_NAME) && t->len == 4 && !memcmp(t->start, "test", 4) && peekAt(&p, 1)->kind == T_TEXT) {
            d = parseTest(&p); /* test "name" { ... } */
        } else {
            fail(&p, t, "%s", "expected fn, const, type, enum, extern, use, pub or test at the top of the file");
            break;
        }
        d->isPub = pub;
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
