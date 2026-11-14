/* Quark — a language server: an editor talks to it over stdin and stdout (the Language Server Protocol). It
 * checks every open file as it changes and reports the errors where they are; formats a file into its one
 * canonical text; tells the type of the name under the cursor; goes to where a name was made; lists a file's
 * declarations; offers names to complete (after a dot: fields, cases, a type's functions; in a call: the names
 * of its values); offers the fixes the errors' messages ask for. Each request checks the text afresh (a check takes a fraction of a
 * millisecond), so nothing is kept between requests but the files' texts. */
#include "qk.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

typedef struct Doc {
    char* uri;
    char* text;
    size_t len;
} Doc;

static Doc* docs;
static int docCount, docCap;
static const char* preludeText; /* the host's declarations (--prelude), or NULL */
static size_t preludeLen;
static const char* preludeName;

static char* copyOf(const char* s, size_t n) {
    char* c = malloc(n + 1);
    memcpy(c, s, n);
    c[n] = '\0';
    return c;
}

static Doc* findDoc(const char* uri) {
    for (int i = 0; i < docCount; i++)
        if (!strcmp(docs[i].uri, uri)) return &docs[i];
    return NULL;
}

static void setDoc(const char* uri, const char* text) {
    Doc* d = findDoc(uri);
    if (!d) {
        if (docCount == docCap) {
            docCap = docCap ? docCap * 2 : 8;
            docs = realloc(docs, sizeof(Doc) * (size_t)docCap);
        }
        d = &docs[docCount++];
        d->uri = copyOf(uri, strlen(uri));
    } else {
        free(d->text);
    }
    d->len = strlen(text);
    d->text = copyOf(text, d->len);
}

static void dropDoc(const char* uri) {
    Doc* d = findDoc(uri);
    if (!d) return;
    free(d->uri);
    free(d->text);
    *d = docs[--docCount];
}

/* ---- messages: "Content-Length: n\r\n\r\n" and n bytes of JSON ------------------------------------- */

static char* readMessage(void) {
    char line[1024];
    long length = -1;
    for (;;) {
        if (!fgets(line, sizeof line, stdin)) return NULL;
        if (!strcmp(line, "\r\n") || !strcmp(line, "\n")) break;
        if (!strncmp(line, "Content-Length:", 15)) length = strtol(line + 15, NULL, 10);
    }
    if (length < 0) return NULL;
    char* body = malloc((size_t)length + 1);
    if (fread(body, 1, (size_t)length, stdin) != (size_t)length) {
        free(body);
        return NULL;
    }
    body[length] = '\0';
    return body;
}

static void sendMessage(const QkBuf* body) {
    printf("Content-Length: %zu\r\n\r\n", body->len);
    fwrite(body->data, 1, body->len, stdout);
    fflush(stdout);
}

/* {"jsonrpc": "2.0", "id": id, "result": result} */
static void respond(const char* id, const char* result) {
    QkBuf b = {0};
    qkBufPrintf(&b, "{\"jsonrpc\": \"2.0\", \"id\": %s, \"result\": %s}", id, result);
    sendMessage(&b);
    qkBufFree(&b);
}

/* an id as it came: a number, or a text */
static char* idText(const J* id) {
    QkBuf b = {0};
    if (!id) qkBufPuts(&b, "null");
    else if (id->kind == J_NUM) qkBufPrintf(&b, "%.0f", id->num);
    else if (id->kind == J_STR) qkJsonString(&b, id->str);
    else qkBufPuts(&b, "null");
    char* s = copyOf(b.data, b.len);
    qkBufFree(&b);
    return s;
}

/* ---- positions: Quark's (line and byte column, from 1) and the editor's (from 0, in UTF-16 units) ---- */

static const char* lineAt(const char* text, int line) { /* line from 1 */
    const char* p = text;
    for (int l = 1; l < line && *p; p++)
        if (*p == '\n') l++;
    return p;
}

static int utf16Column(const char* text, int line, int byteCol) { /* -> the editor's character, from 0 */
    const char* p = lineAt(text, line);
    int units = 0;
    for (int i = 0; i < byteCol - 1 && p[i] && p[i] != '\n'; i++) {
        unsigned char c = (unsigned char)p[i];
        if ((c & 0xC0) == 0x80) continue;
        units += c >= 0xF0 ? 2 : 1;
    }
    return units;
}

static int byteColumn(const char* text, int line, int units) { /* the editor's character -> byte column, from 1 */
    const char* p = lineAt(text, line);
    int i = 0, seen = 0;
    while (p[i] && p[i] != '\n' && seen < units) {
        unsigned char c = (unsigned char)p[i];
        seen += c >= 0xF0 ? 2 : 1;
        i++;
        while (p[i] && ((unsigned char)p[i] & 0xC0) == 0x80) i++;
    }
    return i + 1;
}

static bool isNameChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

/* {"start": {...}, "end": {...}}: the word at Quark's (line, col), at least one character */
static void rangeAt(QkBuf* b, const char* text, int line, int col) {
    const char* p = lineAt(text, line);
    int end = col;
    while (p[end - 1] && isNameChar(p[end - 1])) end++;
    if (end == col) end++;
    int l = line > 0 ? line - 1 : 0;
    qkBufPrintf(b, "{\"start\": {\"line\": %d, \"character\": %d}, \"end\": {\"line\": %d, \"character\": %d}}", l,
                utf16Column(text, line, col), l, utf16Column(text, line, end));
}

/* ---- checking a file ----------------------------------------------------------------------------- */

typedef struct Problem {
    int line, col;
    char* message;
    bool here; /* in the file, not in the prelude */
} Problem;

static Problem* problems;
static int problemCount, problemCap;
static const char* checkingUri;

static void collect(const char* path, int line, int col, const char* message) {
    if (problemCount == problemCap) {
        problemCap = problemCap ? problemCap * 2 : 16;
        problems = realloc(problems, sizeof(Problem) * (size_t)problemCap);
    }
    Problem* p = &problems[problemCount++];
    p->here = !strcmp(path, checkingUri);
    p->line = p->here ? line : 1;
    p->col = p->here ? col : 1;
    if (p->here) {
        p->message = copyOf(message, strlen(message));
    } else {
        QkBuf b = {0};
        qkBufPrintf(&b, "in %s:%d: %s", path, line, message);
        p->message = copyOf(b.data, b.len);
        qkBufFree(&b);
    }
}

static void clearProblems(void) {
    for (int i = 0; i < problemCount; i++) free(problems[i].message);
    problemCount = 0;
}

/* file:///C:/x/a.qk (or file:///home/x/a.qk) as a path, for the files its uses name */
static const char* uriPath(const char* uri) {
    if (strncmp(uri, "file://", 7)) return uri;
    const char* s = uri + 7;
    if (s[0] == '/' && s[1] && s[2] == ':') s++; /* /C:/... on Windows */
    char* out = qkNew(strlen(s) + 1);
    size_t n = 0;
    for (; *s; s++) {
        if (*s == '%' && s[1] && s[2]) {
            char hex[3] = {s[1], s[2], 0};
            out[n++] = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            out[n++] = *s;
        }
    }
    out[n] = 0;
    return out;
}

/* The file parsed (and checked, when it parses): its problems collected. False when it does not parse. */
static bool analyze(const Doc* d, Program* prog, bool check) {
    qkNewReset();
    qkTypesReset();
    clearProblems();
    checkingUri = d->uri;
    qkErrorSink = collect;
    char* text = qkNew(d->len + 1);
    memcpy(text, d->text, d->len);
    QkSource src = {d->uri, text, d->len};
    bool ok = qkParse(&src, prog);
    if (ok && preludeText) {
        QkSource* ps = qkNew(sizeof *ps);
        char* ptext = qkNew(preludeLen + 1);
        memcpy(ptext, preludeText, preludeLen);
        *ps = (QkSource){preludeName, ptext, preludeLen};
        Program prelude;
        if (qkParse(ps, &prelude)) qkAddPrelude(prog, &prelude);
    }
    if (ok) qkLoadUses(prog, uriPath(d->uri));
    if (ok && check) qkCheck(prog, false);
    qkErrorSink = NULL;
    return ok;
}

static void publish(const Doc* d) {
    Program prog;
    analyze(d, &prog, true);
    QkBuf b = {0};
    qkBufPuts(&b, "{\"jsonrpc\": \"2.0\", \"method\": \"textDocument/publishDiagnostics\", \"params\": {\"uri\": ");
    qkJsonString(&b, d->uri);
    qkBufPuts(&b, ", \"diagnostics\": [");
    for (int i = 0; i < problemCount; i++) {
        if (i) qkBufPuts(&b, ", ");
        qkBufPuts(&b, "{\"range\": ");
        rangeAt(&b, d->text, problems[i].line > 0 ? problems[i].line : 1, problems[i].col > 0 ? problems[i].col : 1);
        qkBufPuts(&b, ", \"severity\": 1, \"source\": \"quark\", \"message\": ");
        qkJsonString(&b, problems[i].message);
        qkBufPuts(&b, "}");
    }
    qkBufPuts(&b, "]}}");
    sendMessage(&b);
    qkBufFree(&b);
}

/* ---- what is at a place ---------------------------------------------------------------------------- */

typedef struct Found {
    int line, col;  /* the place asked about */
    const Expr* expr;
    const Stmt* stmt; /* a let / var / for whose name is there */
    const Decl* decl; /* a function or type whose name is there */
    const Param* param;
    const Decl* paramOf;
} Found;

static bool covers(int line, int col, const char* name, int atLine, int atCol) {
    return name && line == atLine && col >= atCol && col < atCol + (int)strlen(name);
}

static void findBlock(Found* f, const Block* b);

static void findExpr(Found* f, const Expr* e) {
    if (!e) return;
    if ((e->kind == E_NAME || e->kind == E_FIELD || (e->kind == E_CALL && !e->implicit)) && covers(f->line, f->col, e->name, e->line, e->col))
        f->expr = e;
    if (e->kind == E_LAMBDA && e->sugar) return; /* a function's name, made into a lambda: its name is the sugar */
    findExpr(f, e->a);
    findExpr(f, e->b);
    findExpr(f, e->body);
    findExpr(f, e->elseExpr);
    for (int i = 0; i < e->argCount; i++) findExpr(f, e->args[i]);
    for (int i = 0; i < e->armCount; i++) {
        findExpr(f, e->arms[i].guard);
        findExpr(f, e->arms[i].body);
    }
    if (e->thenBlock) findBlock(f, e->thenBlock);
    if (e->block) findBlock(f, e->block);
}

static void findBlock(Found* f, const Block* b) {
    for (int i = 0; i < b->count; i++) {
        const Stmt* s = b->stmts[i];
        if ((s->kind == S_LET || s->kind == S_VAR || s->kind == S_FOR) && covers(f->line, f->col, s->name, s->line, s->col + 4))
            f->stmt = s;
        findExpr(f, s->value);
        findExpr(f, s->target);
        findExpr(f, s->iter);
        if (s->body) findBlock(f, s->body);
        if (s->elseBody) findBlock(f, s->elseBody);
    }
}

/* where a declaration's name is: after fn, let, type, enum, extern (effect) fn ... */
static int nameColumn(const Decl* d) {
    if (d->isTest) return d->col + 5;
    int at = d->col + (d->isExtern ? 7 : 0);
    if (d->kind == D_FN) return at + (d->isExtern && d->isEffect ? 10 : 3);
    if (d->kind == D_CONST) return at + 6;
    return at + 5; /* type, enum */
}

static Found find(const Program* p, int line, int col) {
    Found f = {line, col, NULL, NULL, NULL, NULL, NULL};
    for (int i = 0; i < p->count; i++) {
        const Decl* d = p->decls[i];
        if (d->fromPrelude || d->templateOf || d->lifted) continue;
        if (covers(line, col, d->name, d->line, nameColumn(d))) f.decl = d;
        for (int k = 0; k < d->paramCount; k++)
            if (covers(line, col, d->params[k].name, d->params[k].line, d->params[k].col)) {
                f.param = &d->params[k];
                f.paramOf = d;
            }
        if (d->kind == D_FN && d->body) findExpr(&f, d->body);
        if (d->kind == D_CONST) findExpr(&f, d->body);
    }
    return f;
}

static void typeText(QkBuf* b, const Type* t) {
    if (!t) {
        qkBufPuts(b, "?");
        return;
    }
    qkTypeName(t, b);
}

static void signature(QkBuf* b, const Decl* d) {
    if (d->kind != D_FN) {
        qkBufPrintf(b, "%s %s", d->kind == D_ENUM ? "enum" : d->kind == D_CONST ? "const" : d->isExtern ? "extern type" : "type", d->name);
        if (d->kind == D_CONST && d->retType) {
            qkBufPuts(b, ": ");
            typeText(b, d->retType);
        }
        return;
    }
    qkBufPrintf(b, "%s%sfn %s", d->isExtern ? "extern " : "", d->isExtern && d->isEffect ? "effect " : "", d->name);
    for (int i = 0; i < d->typeParamCount; i++) qkBufPrintf(b, "%s%s", i ? ", " : "<", d->typeParams[i]);
    qkBufPuts(b, d->typeParamCount ? ">(" : "(");
    for (int i = 0; i < d->paramCount; i++) {
        qkBufPrintf(b, "%s%s: ", i ? ", " : "", d->params[i].name);
        qkFormatTypeExpr(d->params[i].type, b);
    }
    qkBufPuts(b, ")");
    if (d->retType && d->retType->kind != TY_VOID) {
        qkBufPuts(b, " -> ");
        typeText(b, d->retType);
    } else if (d->ret) {
        qkBufPuts(b, " -> ");
        qkFormatTypeExpr(d->ret, b);
    }
}

/* ---- requests ------------------------------------------------------------------------------------ */

static void position(const J* params, const Doc* d, int* line, int* col) {
    J* pos = qkJsonGet(params, "position");
    J* l = qkJsonGet(pos, "line");
    J* c = qkJsonGet(pos, "character");
    *line = (l ? (int)l->num : 0) + 1;
    *col = byteColumn(d->text, *line, c ? (int)c->num : 0);
}

static void hover(const char* id, const Doc* d, int line, int col) {
    Program prog;
    if (!analyze(d, &prog, true)) {
        respond(id, "null");
        return;
    }
    Found f = find(&prog, line, col);
    QkBuf text = {0};
    const Expr* e = f.expr;
    if (e && e->kind == E_CALL && e->decl && e->decl->kind == D_FN) {
        signature(&text, e->decl->templateOf ? e->decl->templateOf : e->decl);
        if (e->decl->templateOf) { /* an instance: what its types are here */
            qkBufPuts(&text, "\n// here: ");
            typeText(&text, e->type);
        }
    } else if (e && e->kind == E_CALL && e->builtin) {
        qkBufPrintf(&text, "%s(...) -> ", e->name);
        typeText(&text, e->type);
        qkBufPuts(&text, "  // built in");
    } else if (e && e->kind == E_CALL && e->decl) {
        signature(&text, e->decl);
    } else if (e && (e->kind == E_NAME || e->kind == E_FIELD)) {
        if (e->kind == E_NAME && e->decl && e->decl->kind == D_FN) signature(&text, e->decl);
        else {
            const Binding* b = e->binding;
            qkBufPrintf(&text, "%s%s: ", b && b->mutable ? "var " : b ? "let " : "", e->name);
            typeText(&text, e->type);
        }
    } else if (f.stmt && f.stmt->binding) {
        qkBufPrintf(&text, "%s %s: ", f.stmt->kind == S_VAR ? "var" : f.stmt->kind == S_FOR ? "for" : "let", f.stmt->name);
        typeText(&text, f.stmt->binding->type);
    } else if (f.param && f.param->binding) {
        qkBufPrintf(&text, "%s: ", f.param->name);
        typeText(&text, f.param->binding->type);
    } else if (f.decl) {
        signature(&text, f.decl);
    }
    if (!text.len) {
        respond(id, "null");
        return;
    }
    QkBuf value = {0};
    qkBufPuts(&value, "```quark\n");
    qkBufAdd(&value, text.data, text.len);
    qkBufPuts(&value, "\n```");
    QkBuf result = {0};
    qkBufPuts(&result, "{\"contents\": {\"kind\": \"markdown\", \"value\": ");
    qkJsonString(&result, value.data);
    qkBufPuts(&result, "}}");
    respond(id, result.data);
    qkBufFree(&text);
    qkBufFree(&value);
    qkBufFree(&result);
}

static void location(QkBuf* b, const Doc* d, int line, int col) {
    qkBufPuts(b, "{\"uri\": ");
    qkJsonString(b, d->uri);
    qkBufPuts(b, ", \"range\": ");
    rangeAt(b, d->text, line, col);
    qkBufPuts(b, "}");
}

static void definition(const char* id, const Doc* d, int line, int col) {
    Program prog;
    if (!analyze(d, &prog, true)) {
        respond(id, "null");
        return;
    }
    Found f = find(&prog, line, col);
    const Expr* e = f.expr;
    QkBuf b = {0};
    if (e && e->binding && e->binding->line) {
        location(&b, d, e->binding->line, e->binding->col);
    } else if (e && e->decl && !e->decl->fromPrelude) {
        const Decl* to = e->decl->templateOf ? e->decl->templateOf : e->decl;
        location(&b, d, to->line, nameColumn(to));
    }
    respond(id, b.len ? b.data : "null");
    qkBufFree(&b);
}

/* ---- references, rename, inlay hints, signature help ------------------------------------------------ */

typedef struct Target {
    const Binding* binding; /* a let, a var, a parameter, a loop's or a pattern's name */
    const Decl* decl;       /* a function, a type, a constant */
} Target;

static const Binding* rootOf(const Binding* b) {
    while (b && b->capturedFrom) b = b->capturedFrom;
    return b;
}

static const Decl* declOf(const Decl* d) { return d && d->templateOf ? d->templateOf : d; }

/* what the name at (line, col) is */
static Target targetAt(const Program* p, int line, int col) {
    Found f = find(p, line, col);
    Target t = {NULL, NULL};
    if (f.expr && f.expr->binding) t.binding = rootOf(f.expr->binding);
    else if (f.expr && f.expr->decl && !f.expr->decl->fromPrelude) t.decl = declOf(f.expr->decl);
    else if (f.stmt) t.binding = f.stmt->binding;
    else if (f.param && f.param->binding) t.binding = f.param->binding;
    else if (f.decl) t.decl = f.decl;
    return t;
}

typedef struct Refs {
    Target t;
    int* lines;
    int* cols;
    int count, cap, colCap;
} Refs;

static void addRef(Refs* r, int line, int col) {
    for (int i = 0; i < r->count; i++)
        if (r->lines[i] == line && r->cols[i] == col) return;
    int c = r->cap;
    QK_PUSH(r->lines, r->count, r->cap, line);
    r->count--;
    r->cap = c;
    QK_PUSH(r->cols, r->count, r->cap, col);
}

static void refsBlock(Refs* r, const Block* b);

static void refsExpr(Refs* r, const Expr* e) {
    if (!e) return;
    if ((e->kind == E_NAME || (e->kind == E_CALL && !e->implicit)) && e->name) {
        if (r->t.binding && e->binding && rootOf(e->binding) == r->t.binding) addRef(r, e->line, e->col);
        if (r->t.decl && e->decl && declOf(e->decl) == r->t.decl && !strchr(e->name, '.')) addRef(r, e->line, e->col);
    }
    if (e->kind == E_LAMBDA && e->sugar) {
        refsExpr(r, e->sugar);
        return;
    }
    refsExpr(r, e->a);
    refsExpr(r, e->b);
    refsExpr(r, e->body);
    refsExpr(r, e->elseExpr);
    for (int i = 0; i < e->argCount; i++) refsExpr(r, e->args[i]);
    for (int i = 0; i < e->armCount; i++) {
        refsExpr(r, e->arms[i].guard);
        refsExpr(r, e->arms[i].body);
    }
    if (e->thenBlock) refsBlock(r, e->thenBlock);
    if (e->block) refsBlock(r, e->block);
}

static void refsBlock(Refs* r, const Block* b) {
    for (int i = 0; i < b->count; i++) {
        const Stmt* s = b->stmts[i];
        if (r->t.binding && s->binding == r->t.binding) addRef(r, s->line, s->col + 4);
        refsExpr(r, s->value);
        refsExpr(r, s->target);
        refsExpr(r, s->iter);
        if (s->body) refsBlock(r, s->body);
        if (s->elseBody) refsBlock(r, s->elseBody);
    }
}

/* every place the target's name is written in the file: its declaration first */
static Refs references(const Program* p, Target t) {
    Refs r = {t, NULL, NULL, 0, 0, 0};
    if (t.decl && !t.decl->fromPrelude && t.decl->module == 0) addRef(&r, t.decl->line, nameColumn(t.decl));
    if (t.binding && t.binding->line) addRef(&r, t.binding->line, t.binding->col);
    for (int i = 0; i < p->count; i++) {
        const Decl* d = p->decls[i];
        if (d->fromPrelude || d->templateOf || d->lifted || d->module) continue;
        for (int k = 0; k < d->paramCount; k++) refsExpr(&r, d->params[k].fallback);
        if ((d->kind == D_FN || d->kind == D_CONST) && d->body) refsExpr(&r, d->body);
    }
    return r;
}

static void referencesRequest(const char* id, const Doc* d, int line, int col) {
    Program prog;
    if (!analyze(d, &prog, true)) {
        respond(id, "[]");
        return;
    }
    Target t = targetAt(&prog, line, col);
    Refs r = references(&prog, t);
    QkBuf b = {0};
    qkBufPuts(&b, "[");
    for (int i = 0; i < r.count; i++) {
        if (i) qkBufPuts(&b, ", ");
        location(&b, d, r.lines[i], r.cols[i]);
    }
    qkBufPuts(&b, "]");
    respond(id, b.data);
    qkBufFree(&b);
}

static void renameRequest(const char* id, const Doc* d, int line, int col, const char* newName) {
    char* to = newName ? copyOf(newName, strlen(newName)) : NULL; /* analyze frees the request's memory */
    Program prog;
    if (!analyze(d, &prog, true) || !to || !to[0]) {
        free(to);
        respond(id, "null");
        return;
    }
    Target t = targetAt(&prog, line, col);
    bool typeName = t.decl && (t.decl->kind == D_TYPE || t.decl->kind == D_ENUM);
    bool fine = (t.binding || t.decl) && !strchr(to, '_') && (typeName ? to[0] >= 'A' && to[0] <= 'Z' : to[0] >= 'a' && to[0] <= 'z');
    if (!fine || (t.decl && t.decl->owner)) { /* a name of the wrong case, or a type's own function (its type's part stays) */
        free(to);
        respond(id, "null");
        return;
    }
    Refs r = references(&prog, t);
    QkBuf b = {0};
    qkBufPuts(&b, "{\"changes\": {");
    qkJsonString(&b, d->uri);
    qkBufPuts(&b, ": [");
    for (int i = 0; i < r.count; i++) {
        qkBufPuts(&b, i ? ", {\"range\": " : "{\"range\": ");
        rangeAt(&b, d->text, r.lines[i], r.cols[i]);
        qkBufPuts(&b, ", \"newText\": ");
        qkJsonString(&b, to);
        qkBufPuts(&b, "}");
    }
    qkBufPuts(&b, "]}}");
    respond(id, b.data);
    qkBufFree(&b);
    free(to);
}

/* the types Quark worked out, shown after the names that do not write them: let total: Int = ... */
static void hintsBlock(QkBuf* b, const Doc* d, const Block* bl, int* n);

static void hintsExpr(QkBuf* b, const Doc* d, const Expr* e, int* n) {
    if (!e) return;
    hintsExpr(b, d, e->a, n);
    hintsExpr(b, d, e->b, n);
    hintsExpr(b, d, e->body, n);
    hintsExpr(b, d, e->elseExpr, n);
    for (int i = 0; i < e->argCount; i++) hintsExpr(b, d, e->args[i], n);
    for (int i = 0; i < e->armCount; i++) hintsExpr(b, d, e->arms[i].body, n);
    if (e->thenBlock) hintsBlock(b, d, e->thenBlock, n);
    if (e->block) hintsBlock(b, d, e->block, n);
}

static void hintsBlock(QkBuf* b, const Doc* d, const Block* bl, int* n) {
    for (int i = 0; i < bl->count; i++) {
        const Stmt* s = bl->stmts[i];
        if ((s->kind == S_LET || s->kind == S_VAR) && s->name && !s->typeAnn && s->binding && s->binding->type &&
            s->binding->type->kind != TY_ERROR) {
            int col = s->col + 4 + (int)strlen(s->name);
            qkBufPrintf(b, "%s{\"position\": {\"line\": %d, \"character\": %d}, \"label\": ", (*n)++ ? ", " : "", s->line - 1,
                        utf16Column(d->text, s->line, col));
            QkBuf label = {0};
            qkBufPuts(&label, ": ");
            qkTypeName(s->binding->type, &label);
            qkJsonString(b, label.data);
            qkBufFree(&label);
            qkBufPuts(b, ", \"kind\": 1}");
        }
        hintsExpr(b, d, s->value, n);
        hintsExpr(b, d, s->iter, n);
        if (s->body) hintsBlock(b, d, s->body, n);
        if (s->elseBody) hintsBlock(b, d, s->elseBody, n);
    }
}

static void inlayHints(const char* id, const Doc* d) {
    Program prog;
    if (!analyze(d, &prog, true)) {
        respond(id, "[]");
        return;
    }
    QkBuf b = {0};
    int n = 0;
    qkBufPuts(&b, "[");
    for (int i = 0; i < prog.count; i++) {
        const Decl* x = prog.decls[i];
        if (x->fromPrelude || x->templateOf || x->lifted || x->module || x->isTemplate || x->kind != D_FN || !x->body) continue;
        if (x->body->kind == E_BLOCK) hintsBlock(&b, d, x->body->block, &n);
        else hintsExpr(&b, d, x->body, &n);
    }
    qkBufPuts(&b, "]");
    respond(id, b.data);
    qkBufFree(&b);
}

/* the call the cursor is in: its function's signature, and which value is being written */
static void callsAt(const Expr* e, int line, int col, const Expr** best);

static void callsBlock(const Block* bl, int line, int col, const Expr** best) {
    for (int i = 0; i < bl->count; i++) {
        const Stmt* s = bl->stmts[i];
        callsAt(s->value, line, col, best);
        callsAt(s->target, line, col, best);
        callsAt(s->iter, line, col, best);
        if (s->body) callsBlock(s->body, line, col, best);
        if (s->elseBody) callsBlock(s->elseBody, line, col, best);
    }
}

static void callsAt(const Expr* e, int line, int col, const Expr** best) {
    if (!e) return;
    if (e->kind == E_CALL && !e->implicit && e->decl && e->decl->kind == D_FN && e->line == line && e->col < col &&
        (!*best || (*best)->col < e->col))
        *best = e;
    callsAt(e->a, line, col, best);
    callsAt(e->b, line, col, best);
    callsAt(e->body, line, col, best);
    callsAt(e->elseExpr, line, col, best);
    for (int i = 0; i < e->argCount; i++) callsAt(e->args[i], line, col, best);
    for (int i = 0; i < e->armCount; i++) callsAt(e->arms[i].body, line, col, best);
    if (e->thenBlock) callsBlock(e->thenBlock, line, col, best);
    if (e->block) callsBlock(e->block, line, col, best);
}

static void signatureHelp(const char* id, const Doc* d, int line, int col) {
    Program prog;
    if (!analyze(d, &prog, true)) {
        respond(id, "null");
        return;
    }
    const Expr* best = NULL;
    for (int i = 0; i < prog.count; i++) {
        const Decl* x = prog.decls[i];
        if (x->fromPrelude || x->templateOf || x->lifted || x->module || x->kind != D_FN || !x->body) continue;
        callsAt(x->body, line, col, &best);
    }
    const char* text = lineAt(d->text, line);
    int open = -1, depth = 0, commas = 0;
    for (int k = best ? best->col - 1 : 0; best && k < col - 1 && text[k] && text[k] != '\n'; k++) {
        if (text[k] == '(' && depth++ == 0) open = k;
        else if (text[k] == ')') depth--;
        else if (text[k] == ',' && depth == 1) commas++;
    }
    if (!best || open < 0 || depth <= 0) {
        respond(id, "null");
        return;
    }
    const Decl* f = declOf(best->decl);
    QkBuf label = {0};
    signature(&label, f);
    QkBuf b = {0};
    qkBufPuts(&b, "{\"signatures\": [{\"label\": ");
    qkJsonString(&b, label.data);
    qkBufPuts(&b, ", \"parameters\": [");
    for (int i = 0; i < f->paramCount; i++) {
        QkBuf pl = {0};
        qkBufPrintf(&pl, "%s: ", f->params[i].name);
        if (f->params[i].type) qkFormatTypeExpr(f->params[i].type, &pl);
        qkBufPuts(&b, i ? ", {\"label\": " : "{\"label\": ");
        qkJsonString(&b, pl.data);
        qkBufPuts(&b, "}");
        qkBufFree(&pl);
    }
    int active = commas + (best->method ? 1 : 0);
    qkBufPrintf(&b, "]}], \"activeSignature\": 0, \"activeParameter\": %d}", active);
    respond(id, b.data);
    qkBufFree(&b);
    qkBufFree(&label);
}

static void symbols(const char* id, const Doc* d) {
    Program prog;
    if (!analyze(d, &prog, false)) {
        respond(id, "[]");
        return;
    }
    QkBuf b = {0};
    qkBufPuts(&b, "[");
    int n = 0;
    for (int i = 0; i < prog.count; i++) {
        const Decl* x = prog.decls[i];
        if (x->fromPrelude || x->lifted || x->module || x->templateOf || x->kind == D_USE) continue;
        int kind = x->kind == D_FN ? (x->owner || strchr(x->name, '.') ? 6 : 12) : x->kind == D_ENUM ? 10 : x->kind == D_CONST ? 14 : 23;
        qkBufPuts(&b, n++ ? ", {\"name\": " : "{\"name\": ");
        qkJsonString(&b, x->isTest ? x->testName : x->name);
        qkBufPrintf(&b, ", \"kind\": %d, \"location\": ", kind);
        location(&b, d, x->line, nameColumn(x));
        qkBufPuts(&b, "}");
    }
    qkBufPuts(&b, "]");
    respond(id, b.data);
    qkBufFree(&b);
}

static void formatting(const char* id, const Doc* d) {
    Program prog;
    if (!analyze(d, &prog, false)) { /* it does not parse: nothing to format */
        respond(id, "null");
        return;
    }
    QkBuf text = {0};
    qkFormat(&prog, &text);
    int lines = 0;
    for (size_t i = 0; i < d->len; i++) lines += d->text[i] == '\n';
    QkBuf b = {0};
    qkBufPrintf(&b, "[{\"range\": {\"start\": {\"line\": 0, \"character\": 0}, \"end\": {\"line\": %d, \"character\": 0}}, \"newText\": ",
                lines + 1);
    qkJsonString(&b, text.data ? text.data : "");
    qkBufPuts(&b, "}]");
    respond(id, b.data);
    qkBufFree(&text);
    qkBufFree(&b);
}

/* ---- completion: names; after a dot, a record's fields, an enum's cases, a type's or a module's functions;
 * inside a call, the names of the values it takes ---------------------------------------------------------- */

typedef struct Items {
    QkBuf b;
    int n;
} Items;

static void item(Items* it, const char* label, int kind, const char* detail, const char* insert, const char* sort) {
    qkBufPuts(&it->b, it->n++ ? ", {\"label\": " : "{\"label\": ");
    qkJsonString(&it->b, label);
    qkBufPrintf(&it->b, ", \"kind\": %d", kind);
    if (detail) {
        qkBufPuts(&it->b, ", \"detail\": ");
        qkJsonString(&it->b, detail);
    }
    if (insert) {
        qkBufPuts(&it->b, ", \"insertText\": ");
        qkJsonString(&it->b, insert);
    }
    if (sort) {
        qkBufPuts(&it->b, ", \"sortText\": ");
        qkJsonString(&it->b, sort);
    }
    qkBufPuts(&it->b, "}");
}

/* a function's text, for an item's detail */
static const char* detailOf(const Decl* d) {
    QkBuf b = {0};
    signature(&b, d);
    const char* s = qkDup(b.data, b.len);
    qkBufFree(&b);
    return s;
}

static const char* typeExprText(const TypeExpr* t) {
    if (!t) return NULL;
    QkBuf b = {0};
    qkFormatTypeExpr(t, &b);
    const char* s = qkDup(b.data, b.len);
    qkBufFree(&b);
    return s;
}

/* a type's own functions (fn Vec2.length), named without the type: a value's (they take self), or the type's */
static void ownFunctions(Items* it, const Program* p, const char* type, bool values) {
    for (int i = 0; i < p->count; i++) {
        const Decl* x = p->decls[i];
        if (x->kind != D_FN || !x->owner || x->templateOf || x->lifted || strcmp(x->owner, type)) continue;
        bool takesSelf = x->paramCount && !strcmp(x->params[0].name, "self");
        if (values != takesSelf) continue;
        item(it, x->name + strlen(type) + 1, 2, detailOf(x), NULL, "1");
    }
}

static void membersOf(Items* it, const Program* p, const Type* t) {
    if (!t) return;
    const Decl* d = t->decl ? declOf(t->decl) : NULL;
    if (t->kind == TY_RECORD && d && !d->isTuple)
        for (int i = 0; i < d->paramCount; i++) item(it, d->params[i].name, d->isInterface ? 2 : 5, typeExprText(d->params[i].type), NULL, "0");
    if ((t->kind == TY_RECORD || t->kind == TY_ENUM) && d) {
        ownFunctions(it, p, d->name, true);
        return;
    }
    if (t->kind == TY_ERROR || t->kind == TY_VOID) return;
    for (int i = 1; i < B_INTERFACE; i++) item(it, qkBuiltinNames[i], 3, NULL, NULL, "2"); /* xs.map(f): a function after its first value */
}

/* the latest let, var, for, parameter or lambda's parameter named `name` at or before `line` */
typedef struct Latest {
    const char* name;
    int line;
    const Binding* found;
    int foundLine;
} Latest;

static void latestBlock(Latest* l, const Block* b);

static void latestExpr(Latest* l, const Expr* e) {
    if (!e) return;
    latestExpr(l, e->a);
    latestExpr(l, e->b);
    latestExpr(l, e->body);
    latestExpr(l, e->elseExpr);
    for (int i = 0; i < e->argCount; i++) latestExpr(l, e->args[i]);
    for (int i = 0; i < e->armCount; i++) latestExpr(l, e->arms[i].body);
    for (int i = 0; e->kind == E_LAMBDA && i < e->paramCount; i++)
        if (e->params[i].binding && !strcmp(e->params[i].name, l->name) && e->line <= l->line && e->line >= l->foundLine) {
            l->found = e->params[i].binding;
            l->foundLine = e->line;
        }
    if (e->thenBlock) latestBlock(l, e->thenBlock);
    if (e->block) latestBlock(l, e->block);
}

static void latestBlock(Latest* l, const Block* b) {
    for (int i = 0; i < b->count; i++) {
        const Stmt* s = b->stmts[i];
        if (s->line > l->line) return;
        if (s->binding && s->name && !strcmp(s->name, l->name) && s->line >= l->foundLine) {
            l->found = s->binding;
            l->foundLine = s->line;
        }
        latestExpr(l, s->value);
        latestExpr(l, s->iter);
        if (s->body) latestBlock(l, s->body);
        if (s->elseBody) latestBlock(l, s->elseBody);
    }
}

static const Type* typeOfName(const Program* p, const char* name, int line) {
    const Decl* in = NULL; /* the function the line is in: the last one to start before it */
    for (int i = 0; i < p->count; i++) {
        const Decl* x = p->decls[i];
        if (x->kind == D_FN && x->body && !x->module && !x->fromPrelude && !x->templateOf && !x->lifted && x->line <= line &&
            (!in || x->line > in->line))
            in = x;
    }
    if (!in) return NULL;
    Latest l = {name, line, NULL, 0};
    for (int i = 0; i < in->paramCount; i++)
        if (!strcmp(in->params[i].name, name)) l.found = in->params[i].binding;
    latestExpr(&l, in->body);
    return l.found ? l.found->type : NULL;
}

/* The text with [from, to) of its bytes made spaces: what is being typed there taken out, so the rest parses. */
static Doc blanked(const Doc* d, size_t from, size_t to) {
    Doc t = {d->uri, copyOf(d->text, d->len), d->len};
    for (size_t i = from; i < to && i < t.len; i++)
        if (t.text[i] != '\n') t.text[i] = ' ';
    return t;
}

static bool analyzeSome(Doc* t, Program* prog, int line) { /* the text, else the text without the cursor's line */
    if (analyze(t, prog, true)) return true;
    char* c = (char*)lineAt(t->text, line);
    for (; *c && *c != '\n'; c++) *c = ' ';
    return analyze(t, prog, true);
}

/* After a dot: what is before it, and what it offers. */
static void memberCompletion(Items* it, const Doc* d, int line, size_t dot) {
    size_t start = dot;
    while (start > 0 && isNameChar(d->text[start - 1])) start--;
    char* receiver = copyOf(d->text + start, dot - start);
    int recvCol = (int)(start - (size_t)(lineAt(d->text, line) - d->text)) + 1;
    size_t to = dot + 1;
    while (to < d->len && isNameChar(d->text[to])) to++;
    Doc t = blanked(d, dot, to);
    Program prog;
    bool ok = analyzeSome(&t, &prog, line);
    if (ok && receiver[0] >= 'A' && receiver[0] <= 'Z') { /* a type: an enum's cases, the type's own functions */
        for (int i = 0; i < prog.count; i++) {
            const Decl* x = prog.decls[i];
            if (x->kind != D_ENUM || strcmp(x->name, receiver)) continue;
            for (int k = 0; k < x->caseCount; k++) {
                const char* detail = NULL;
                if (x->caseFieldCount && x->caseFieldCount[k]) {
                    QkBuf b = {0};
                    for (int f = 0; f < x->caseFieldCount[k]; f++) {
                        qkBufPrintf(&b, "%s%s: ", f ? ", " : "(", x->caseFields[k][f].name);
                        qkFormatTypeExpr(x->caseFields[k][f].type, &b);
                    }
                    qkBufPuts(&b, ")");
                    detail = qkDup(b.data, b.len);
                    qkBufFree(&b);
                }
                item(it, x->cases[k], 20, detail, NULL, "0");
            }
        }
        ownFunctions(it, &prog, receiver, false);
    } else if (ok && receiver[0]) {
        int module = -1; /* use geometry as g: g. offers geometry's pub declarations */
        for (int i = 0; prog.moduleCount && prog.modules[0].aliases && i < prog.modules[0].useCount; i++)
            if (prog.modules[0].aliases[i] && !strcmp(prog.modules[0].aliases[i], receiver)) module = prog.modules[0].uses[i];
        if (module > 0) {
            for (int i = 0; i < prog.count; i++) {
                const Decl* x = prog.decls[i];
                if (x->module != module || !x->isPub || x->owner || x->templateOf || x->lifted || x->kind == D_USE) continue;
                int kind = x->kind == D_FN ? 3 : x->kind == D_ENUM ? 13 : x->kind == D_CONST ? 21 : 22;
                item(it, x->name, kind, x->kind == D_FN ? detailOf(x) : NULL, NULL, "0");
            }
        } else {
            Found f = find(&prog, line, recvCol);
            const Type* type = f.expr ? f.expr->type : f.stmt && f.stmt->binding ? f.stmt->binding->type
                             : f.param && f.param->binding ? f.param->binding->type : NULL;
            if (!type || type->kind == TY_ERROR) type = typeOfName(&prog, receiver, line);
            membersOf(it, &prog, type);
        }
    }
    free(t.text);
    free(receiver);
}

/* Inside a call's parentheses, where a value starts: the names of the values it takes, not written yet. */
static void argumentCompletion(Items* it, const Doc* d, size_t at) {
    size_t i = at;
    while (i > 0 && isNameChar(d->text[i - 1])) i--;
    size_t word = i;
    while (i > 0 && (d->text[i - 1] == ' ' || d->text[i - 1] == '\n' || d->text[i - 1] == '\r')) i--;
    if (!i || (d->text[i - 1] != '(' && d->text[i - 1] != ',')) return;
    int depth = 0; /* the call's ( */
    size_t open = i;
    bool found = false;
    while (open > 0) {
        char c = d->text[--open];
        if (c == ')' || c == ']' || c == '}') depth++;
        else if (c == '(' || c == '[' || c == '{') {
            if (!depth) {
                found = c == '(';
                break;
            }
            depth--;
        }
    }
    size_t nameStart = open;
    while (found && nameStart > 0 && isNameChar(d->text[nameStart - 1])) nameStart--;
    if (!found || nameStart == open) return;
    char* callee = copyOf(d->text + nameStart, open - nameStart);
    char* qualifier = NULL; /* Shape.Circle( or p.scale( */
    if (nameStart > 0 && d->text[nameStart - 1] == '.') {
        size_t q = nameStart - 1;
        while (q > 0 && isNameChar(d->text[q - 1])) q--;
        qualifier = copyOf(d->text + q, nameStart - 1 - q);
    }
    char* written = copyOf(d->text + open, at - open); /* the call so far: the names already given */
    Doc t = blanked(d, word, at);
    int line = 1;
    for (size_t k = 0; k < at; k++) line += d->text[k] == '\n';
    Program prog;
    bool ok = analyzeSome(&t, &prog, line);
    const Param* params = NULL;
    int count = 0, skip = 0;
    for (int k = 0; ok && k < prog.count && !params; k++) {
        const Decl* x = prog.decls[k];
        if (x->templateOf || x->lifted || x->isTest) continue;
        if (x->kind == D_TYPE && !x->isInterface && !x->isExtern && !qualifier && !strcmp(x->name, callee)) {
            params = x->params, count = x->paramCount;
        } else if (x->kind == D_FN && !x->owner && !qualifier && !strcmp(x->name, callee)) {
            params = x->params, count = x->paramCount;
        } else if (x->kind == D_FN && x->owner && qualifier && !strcmp(x->name + strlen(x->owner) + 1, callee)) {
            params = x->params, count = x->paramCount;
            skip = x->paramCount && !strcmp(x->params[0].name, "self"); /* p.scale(k: ...): self is p */
        } else if (x->kind == D_ENUM && qualifier && !strcmp(x->name, qualifier)) {
            for (int c = 0; c < x->caseCount; c++)
                if (!strcmp(x->cases[c], callee) && x->caseFieldCount) params = x->caseFields[c], count = x->caseFieldCount[c];
        }
    }
    int level = 0; /* the values given before, in order (not by name): they take the first parameters */
    bool named = false, started = false;
    for (const char* w = written + 1; *w; w++) {
        if (*w == '(' || *w == '[' || *w == '{') level++;
        else if (*w == ')' || *w == ']' || *w == '}') level--;
        else if (!level && *w == ',') {
            if (started && !named) skip++;
            named = started = false;
        } else if (!level && !started && *w != ' ' && *w != '\n' && *w != '\r') {
            started = true;
            const char* e = w;
            while (isNameChar(*e)) e++;
            while (*e == ' ') e++;
            named = e > w && *e == ':';
        }
    }
    for (int k = skip; params && k < count; k++) {
        size_t n = strlen(params[k].name);
        bool given = false;
        for (const char* w = written; (w = strstr(w, params[k].name)) != NULL; w += n) {
            const char* after = w + n;
            while (*after == ' ') after++;
            if (*after == ':' && (w == written || !isNameChar(w[-1]))) given = true;
        }
        if (given) continue;
        QkBuf label = {0}, insert = {0};
        qkBufPrintf(&label, "%s:", params[k].name);
        qkBufPrintf(&insert, "%s: ", params[k].name);
        item(it, label.data, 5, typeExprText(params[k].type), insert.data, "0");
        qkBufFree(&label);
        qkBufFree(&insert);
    }
    free(t.text);
    free(callee);
    free(qualifier);
    free(written);
}

static void completion(const char* id, const Doc* d, int line, int col) {
    Items it = {{0}, 0};
    qkBufPuts(&it.b, "[");
    size_t at = (size_t)(lineAt(d->text, line) - d->text) + (size_t)(col - 1); /* the cursor, as a byte of the text */
    if (at > d->len) at = d->len;
    size_t word = at;
    while (word > 0 && isNameChar(d->text[word - 1])) word--;
    if (word > 0 && d->text[word - 1] == '.' && (word < 2 || d->text[word - 2] != '.')) { /* not a range's .. */
        memberCompletion(&it, d, line, word - 1);
    } else {
        argumentCompletion(&it, d, at);
        Program prog;
        bool parsed = analyze(d, &prog, false);
        for (int i = 0; i < qkKeywordCount; i++) item(&it, qkKeywords[i], 14, NULL, NULL, NULL);
        for (int i = 1; i < B_INTERFACE; i++) item(&it, qkBuiltinNames[i], 3, NULL, NULL, NULL);
        for (int i = 0; parsed && i < prog.count; i++) {
            const Decl* x = prog.decls[i];
            if (x->isTest || x->owner || strchr(x->name, '.') || x->kind == D_USE || x->templateOf || x->lifted || x->fromPrelude) continue;
            int kind = x->kind == D_FN ? 3 : x->kind == D_ENUM ? 13 : x->kind == D_CONST ? 21 : 22;
            item(&it, x->name, kind, NULL, NULL, NULL);
        }
    }
    qkBufPuts(&it.b, "]");
    respond(id, it.b.data);
    qkBufFree(&it.b);
}

/* ---- quick fixes: the edits the errors' own messages ask for ------------------------------------------- */

static size_t offsetOf(const char* text, int line, int col) { return (size_t)(lineAt(text, line) - text) + (size_t)(col - 1); }

/* one edit: [from, to) of the text made `with` */
typedef struct Edit {
    size_t from, to;
    const char* with;
} Edit;

static void editRange(QkBuf* b, const Doc* d, size_t from, size_t to) {
    int l0 = 1, c0 = 1;
    for (size_t i = 0; i < from; i++) {
        if (d->text[i] == '\n') l0++, c0 = 1;
        else c0++;
    }
    int l1 = l0, c1 = c0;
    for (size_t i = from; i < to; i++) {
        if (d->text[i] == '\n') l1++, c1 = 1;
        else c1++;
    }
    qkBufPrintf(b, "{\"start\": {\"line\": %d, \"character\": %d}, \"end\": {\"line\": %d, \"character\": %d}}", l0 - 1,
                utf16Column(d->text, l0, c0), l1 - 1, utf16Column(d->text, l1, c1));
}

static bool wordAt(const char* text, size_t len, size_t at, const char* word) {
    size_t n = strlen(word);
    return at + n <= len && !memcmp(text + at, word, n) && (at == 0 || !isNameChar(text[at - 1])) && !isNameChar(text[at + n]);
}

/* the name after `after` in the message, without its quotes ("unknown name \"x\": did you mean \"y\"?") */
static char* quoted(const char* message, const char* after) {
    const char* p = strstr(message, after);
    if (!p) return NULL;
    p += strlen(after);
    if (*p == '"') p++;
    const char* end = p;
    while (isNameChar(*end)) end++;
    return end > p ? copyOf(p, (size_t)(end - p)) : NULL;
}

/* where `word` is written nearest the place on its line (at it, then before, then after); -1 if nowhere */
static long nearest(const Doc* d, int line, int col, const char* word) {
    size_t start = offsetOf(d->text, line, 1), at = offsetOf(d->text, line, col);
    size_t end = at;
    while (end < d->len && d->text[end] != '\n') end++;
    if (wordAt(d->text, d->len, at, word)) return (long)at;
    for (size_t i = at; i-- > start;)
        if (wordAt(d->text, d->len, i, word)) return (long)i;
    for (size_t i = at; i < end; i++)
        if (wordAt(d->text, d->len, i, word)) return (long)i;
    return -1;
}

static bool inText(const Doc* d, size_t at) { /* inside a "text" on its line, or a comment: not code */
    size_t i = at;
    while (i > 0 && d->text[i - 1] != '\n') i--;
    bool quote = false;
    for (; i < at; i++) {
        if (d->text[i] == '\\') i++;
        else if (d->text[i] == '"') quote = !quote;
        else if (!quote && d->text[i] == '/' && d->text[i + 1] == '/') return true;
    }
    return quote;
}

/* The fix a problem's message asks for: a title and its edits (false: none). */
static bool quickFix(const Doc* d, const Problem* pr, QkBuf* title, Edit* edits, int* count, int cap) {
    const char* m = pr->message;
    size_t at = offsetOf(d->text, pr->line, pr->col);
    *count = 0;
    char* fixed = quoted(m, "without underscores: write ");
    if (fixed) { /* every snake_case spelling of the name, in camelCase */
        size_t s = at, e = at;
        while (s > 0 && isNameChar(d->text[s - 1])) s--;
        while (e < d->len && isNameChar(d->text[e])) e++;
        char* old = copyOf(d->text + s, e - s);
        for (size_t i = 0; i < d->len && *count < cap; i++)
            if (wordAt(d->text, d->len, i, old) && !inText(d, i)) edits[(*count)++] = (Edit){i, i + strlen(old), fixed};
        qkBufPrintf(title, "Rename %s to %s", old, fixed);
        free(old);
        return *count > 0;
    }
    if (strstr(m, "did you mean")) { /* the name written, made the one meant */
        char* meant = quoted(m, "did you mean ");
        char* wrong = quoted(m, "unknown name ");
        if (!wrong) wrong = quoted(m, "has no field ");
        if (!wrong) wrong = quoted(m, "has no parameter ");
        if (!wrong) wrong = quoted(m, "has no case ");
        long w = meant && wrong ? nearest(d, pr->line, pr->col, wrong) : -1;
        if (w >= 0) {
            edits[(*count)++] = (Edit){(size_t)w, (size_t)w + strlen(wrong), meant};
            qkBufPrintf(title, "Change %s to %s", wrong, meant);
        }
        free(wrong);
        return *count > 0;
    }
    if (strstr(m, "write not x") && at < d->len && d->text[at] == '!') { /* !x -> not x */
        size_t e = at + 1;
        while (e < d->len && d->text[e] == ' ') e++;
        edits[(*count)++] = (Edit){at, e, "not "};
        qkBufPuts(title, "Write not");
        return true;
    }
    if (strstr(m, "a constant is written const") && (wordAt(d->text, d->len, at, "let") || wordAt(d->text, d->len, at, "var"))) {
        edits[(*count)++] = (Edit){at, at + 3, "const"};
        qkBufPuts(title, "Make it a const");
        return true;
    }
    if (strstr(m, "effects are inferred") && wordAt(d->text, d->len, at, "effect")) { /* effect fn -> fn */
        size_t e = at + 6;
        while (e < d->len && d->text[e] == ' ') e++;
        edits[(*count)++] = (Edit){at, e, ""};
        qkBufPuts(title, "Remove effect (effects are inferred)");
        return true;
    }
    char* name = strstr(m, "declare it with var") ? quoted(m, "") : NULL;
    if (name) { /* "x" is a let: the let that makes it, a var */
        size_t best = (size_t)-1;
        for (size_t i = 0; i < at; i++)
            if (wordAt(d->text, d->len, i, "let") && !inText(d, i)) {
                size_t n = i + 3;
                while (n < d->len && d->text[n] == ' ') n++;
                if (wordAt(d->text, d->len, n, name)) best = i;
            }
        if (best != (size_t)-1) {
            edits[(*count)++] = (Edit){best, best + 3, "var"};
            qkBufPrintf(title, "Make %s a var", name);
        }
        free(name);
        return *count > 0;
    }
    return false;
}

static void diagnostic(QkBuf* b, const Doc* d, const Problem* p) {
    qkBufPuts(b, "{\"range\": ");
    rangeAt(b, d->text, p->line > 0 ? p->line : 1, p->col > 0 ? p->col : 1);
    qkBufPuts(b, ", \"severity\": 1, \"source\": \"quark\", \"message\": ");
    qkJsonString(b, p->message);
    qkBufPuts(b, "}");
}

/* the quick fixes for the problems on lines [fromLine, toLine] */
static void codeActions(const char* id, const Doc* d, int fromLine, int toLine) {
    Program prog;
    analyze(d, &prog, true);
    QkBuf b = {0};
    qkBufPuts(&b, "[");
    int n = 0;
    for (int i = 0; i < problemCount; i++) {
        const Problem* p = &problems[i];
        if (!p->here || p->line < fromLine || p->line > toLine) continue;
        QkBuf title = {0};
        Edit edits[64];
        int count;
        if (!quickFix(d, p, &title, edits, &count, 64)) {
            qkBufFree(&title);
            continue;
        }
        qkBufPuts(&b, n++ ? ", {\"title\": " : "{\"title\": ");
        qkJsonString(&b, title.data);
        qkBufPuts(&b, ", \"kind\": \"quickfix\", \"isPreferred\": true, \"diagnostics\": [");
        diagnostic(&b, d, p);
        qkBufPuts(&b, "], \"edit\": {\"changes\": {");
        qkJsonString(&b, d->uri);
        qkBufPuts(&b, ": [");
        for (int k = 0; k < count; k++) {
            qkBufPuts(&b, k ? ", {\"range\": " : "{\"range\": ");
            editRange(&b, d, edits[k].from, edits[k].to);
            qkBufPuts(&b, ", \"newText\": ");
            qkJsonString(&b, edits[k].with);
            qkBufPuts(&b, "}");
        }
        qkBufPuts(&b, "]}}}");
        qkBufFree(&title);
    }
    qkBufPuts(&b, "]");
    respond(id, b.data);
    qkBufFree(&b);
}

/* ---- the loop ------------------------------------------------------------------------------------ */

int qkLsp(const char* prelude) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (prelude) {
        preludeText = qkReadFile(prelude, &preludeLen);
        preludeName = prelude;
    }
    bool shuttingDown = false;
    for (;;) {
        char* body = readMessage();
        if (!body) return shuttingDown ? 0 : 1;
        qkNewReset();
        qkTypesReset();
        J* m = qkJsonParse(body, strlen(body));
        free(body);
        if (!m) continue;
        const J* method = qkJsonGet(m, "method");
        const char* name = method && method->kind == J_STR ? method->str : "";
        char* id = idText(qkJsonGet(m, "id"));
        J* params = qkJsonGet(m, "params");
        J* textDoc = qkJsonGet(params, "textDocument");
        J* uriJ = qkJsonGet(textDoc, "uri");
        char* uri = uriJ && uriJ->kind == J_STR ? copyOf(uriJ->str, strlen(uriJ->str)) : NULL;
        Doc* d = uri ? findDoc(uri) : NULL;
        if (!strcmp(name, "initialize")) {
            respond(id, "{\"capabilities\": {\"textDocumentSync\": 1, \"hoverProvider\": true, \"definitionProvider\": true, "
                        "\"documentSymbolProvider\": true, \"documentFormattingProvider\": true, \"completionProvider\": {\"triggerCharacters\": [\".\"]}, "
                        "\"codeActionProvider\": {\"codeActionKinds\": [\"quickfix\"]}, "
                        "\"referencesProvider\": true, \"renameProvider\": true, \"inlayHintProvider\": true, "
                        "\"signatureHelpProvider\": {\"triggerCharacters\": [\"(\", \",\"]}}, "
                        "\"serverInfo\": {\"name\": \"quark\"}}");
        } else if (!strcmp(name, "shutdown")) {
            shuttingDown = true;
            respond(id, "null");
        } else if (!strcmp(name, "exit")) {
            free(id);
            free(uri);
            return shuttingDown ? 0 : 1;
        } else if (!strcmp(name, "textDocument/didOpen") || !strcmp(name, "textDocument/didChange")) {
            J* text = qkJsonGet(textDoc, "text");
            J* changes = qkJsonGet(params, "contentChanges");
            if (!text && changes && changes->count) text = qkJsonGet(changes->items[changes->count - 1], "text");
            if (uri && text && text->kind == J_STR) {
                setDoc(uri, text->str);
                publish(findDoc(uri));
            }
        } else if (!strcmp(name, "textDocument/didClose")) {
            if (uri) dropDoc(uri);
        } else if (!strcmp(name, "textDocument/hover") || !strcmp(name, "textDocument/definition")) {
            if (!d) {
                respond(id, "null");
            } else {
                int line, col;
                position(params, d, &line, &col);
                if (name[13] == 'h') hover(id, d, line, col);
                else definition(id, d, line, col);
            }
        } else if (!strcmp(name, "textDocument/documentSymbol")) {
            if (d) symbols(id, d);
            else respond(id, "[]");
        } else if (!strcmp(name, "textDocument/formatting")) {
            if (d) formatting(id, d);
            else respond(id, "null");
        } else if (!strcmp(name, "textDocument/references") || !strcmp(name, "textDocument/rename") ||
                   !strcmp(name, "textDocument/signatureHelp")) {
            if (!d) {
                respond(id, "null");
            } else {
                int line, col;
                position(params, d, &line, &col);
                J* to = qkJsonGet(params, "newName");
                if (!strcmp(name, "textDocument/references")) referencesRequest(id, d, line, col);
                else if (!strcmp(name, "textDocument/rename")) renameRequest(id, d, line, col, to && to->kind == J_STR ? to->str : NULL);
                else signatureHelp(id, d, line, col);
            }
        } else if (!strcmp(name, "textDocument/inlayHint")) {
            if (d) inlayHints(id, d);
            else respond(id, "[]");
        } else if (!strcmp(name, "textDocument/completion")) {
            if (d) {
                int line, col;
                position(params, d, &line, &col);
                completion(id, d, line, col);
            } else {
                respond(id, "[]");
            }
        } else if (!strcmp(name, "textDocument/codeAction")) {
            J* range = qkJsonGet(params, "range");
            J* from = qkJsonGet(qkJsonGet(range, "start"), "line");
            J* to = qkJsonGet(qkJsonGet(range, "end"), "line");
            if (d) codeActions(id, d, (from ? (int)from->num : 0) + 1, (to ? (int)to->num : 1 << 30) + 1);
            else respond(id, "[]");
        } else if (qkJsonGet(m, "id") && strcmp(name, "")) { /* a request it does not know */
            QkBuf b = {0};
            qkBufPrintf(&b, "{\"jsonrpc\": \"2.0\", \"id\": %s, \"error\": {\"code\": -32601, \"message\": \"not handled\"}}", id);
            sendMessage(&b);
            qkBufFree(&b);
        }
        free(id);
        free(uri);
    }
}
