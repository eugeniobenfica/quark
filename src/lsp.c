/* Quark — a language server: an editor talks to it over stdin and stdout (the Language Server Protocol). It
 * checks every open file as it changes and reports the errors where they are; formats a file into its one
 * canonical text; tells the type of the name under the cursor; goes to where a name was made; lists a file's
 * declarations; offers names to complete. Each request checks the text afresh (a check takes a fraction of a
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

static bool isNameChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

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

static void completion(const char* id, const Doc* d) {
    Program prog;
    bool parsed = analyze(d, &prog, false);
    QkBuf b = {0};
    qkBufPuts(&b, "[");
    int n = 0;
    for (int i = 0; i < qkKeywordCount; i++) {
        qkBufPuts(&b, n++ ? ", {\"label\": " : "{\"label\": ");
        qkJsonString(&b, qkKeywords[i]);
        qkBufPuts(&b, ", \"kind\": 14}");
    }
    for (int i = 1; i < B_COUNT; i++) {
        qkBufPuts(&b, ", {\"label\": ");
        qkJsonString(&b, qkBuiltinNames[i]);
        qkBufPuts(&b, ", \"kind\": 3}");
    }
    for (int i = 0; parsed && i < prog.count; i++) {
        const Decl* x = prog.decls[i];
        if (x->isTest) continue;
        const char* name = x->owner ? x->name + strlen(x->owner) + 1 : x->name;
        int kind = x->kind == D_FN ? (x->owner ? 2 : 3) : x->kind == D_ENUM ? 13 : x->kind == D_CONST ? 21 : 22;
        qkBufPuts(&b, ", {\"label\": ");
        qkJsonString(&b, name);
        qkBufPrintf(&b, ", \"kind\": %d}", kind);
        for (int k = 0; x->kind == D_ENUM && k < x->caseCount; k++) {
            qkBufPuts(&b, ", {\"label\": ");
            qkJsonString(&b, x->cases[k]);
            qkBufPuts(&b, ", \"kind\": 20}");
        }
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
                        "\"documentSymbolProvider\": true, \"documentFormattingProvider\": true, \"completionProvider\": {}, "
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
            if (d) completion(id, d);
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
