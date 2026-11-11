/* Quark — programs as node graphs, and back.
 *
 * Every expression that is not a plain value is a node; its inputs are wires from other nodes or values typed
 * into the pin. Every let, var, parameter and loop variable is a named node, and a use of the name is a wire
 * from it. Statements run in the order of their block's list: that order is the execution wire. Literals,
 * enum cases and function names are pin values, never nodes. Comments and blank lines ride on the nodes, so
 * text -> graph -> text gives back the formatter's text exactly.
 *
 * {"quark": 1, "decls": [
 *   {"kind": "fn", "name": "luma", "params": [{"id": 1, "name": "r", "type": "Int"}], "returns": "Int",
 *    "form": "expr", "result": {"node": 4}, "nodes": [{"id": 4, "kind": "op", "op": "+", "inputs": [...]}, ...]},
 *   {"kind": "type", "name": "Point", "fields": [{"name": "x", "type": "Float"}]},
 *   {"kind": "enum", "name": "Mode", "cases": ["Waveform", "Parade"]}]}
 *
 * Inputs: {"node": id} | {"value": "12", "of": "Int"} | {"case": "Mode.Parade"} | {"fn": "luma"}.
 * Blocks: {"stmts": [ids], "closing": "comment"}. A node's "key" (fn/name, for named nodes) is stable while
 * the name is: an editor keeps positions in a side file under it. */
#include "qk.h"

#include <stdio.h>
#include <stdlib.h>

/* ---- reading JSON -------------------------------------------------------------------------------- */

typedef enum JKind { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JKind;

typedef struct J {
    JKind kind;
    double num;
    bool b;
    const char* str;
    struct J** items; /* array items, or object values */
    const char** keys;
    int count, cap;
} J;

typedef struct JParser {
    const char* p;
    const char* end;
    bool failed;
} JParser;

static void jws(JParser* jp) {
    while (jp->p < jp->end && (*jp->p == ' ' || *jp->p == '\n' || *jp->p == '\r' || *jp->p == '\t')) jp->p++;
}

static const char* jstring(JParser* jp) {
    QkBuf b = {0};
    jp->p++; /* the opening quote */
    while (jp->p < jp->end && *jp->p != '"') {
        char c = *jp->p++;
        if (c == '\\' && jp->p < jp->end) {
            char k = *jp->p++;
            if (k == 'n') c = '\n';
            else if (k == 't') c = '\t';
            else if (k == 'u' && jp->p + 4 <= jp->end) {
                char hex[5] = {jp->p[0], jp->p[1], jp->p[2], jp->p[3], 0};
                c = (char)strtol(hex, NULL, 16);
                jp->p += 4;
            } else c = k;
        }
        qkBufByte(&b, (uint8_t)c);
    }
    if (jp->p >= jp->end) jp->failed = true;
    else jp->p++;
    const char* s = qkDup(b.data ? b.data : "", b.len);
    qkBufFree(&b);
    return s;
}

static J* jvalue(JParser* jp) {
    jws(jp);
    J* v = qkNew(sizeof *v);
    if (jp->p >= jp->end) {
        jp->failed = true;
        return v;
    }
    char c = *jp->p;
    if (c == '{' || c == '[') {
        bool obj = c == '{';
        v->kind = obj ? J_OBJ : J_ARR;
        jp->p++;
        jws(jp);
        if (jp->p < jp->end && *jp->p == (obj ? '}' : ']')) {
            jp->p++;
            return v;
        }
        while (!jp->failed) {
            const char* key = NULL;
            if (obj) {
                jws(jp);
                if (jp->p >= jp->end || *jp->p != '"') {
                    jp->failed = true;
                    break;
                }
                key = jstring(jp);
                jws(jp);
                if (jp->p >= jp->end || *jp->p != ':') {
                    jp->failed = true;
                    break;
                }
                jp->p++;
            }
            J* item = jvalue(jp);
            if (v->count == v->cap) {
                int cap = v->cap ? v->cap * 2 : 8;
                J** items = qkNew(sizeof(J*) * (size_t)cap);
                const char** keys = qkNew(sizeof(char*) * (size_t)cap);
                if (v->count) {
                    memcpy(items, v->items, sizeof(J*) * (size_t)v->count);
                    memcpy(keys, v->keys, sizeof(char*) * (size_t)v->count);
                }
                v->items = items;
                v->keys = keys;
                v->cap = cap;
            }
            v->keys[v->count] = key;
            v->items[v->count++] = item;
            jws(jp);
            if (jp->p < jp->end && *jp->p == ',') {
                jp->p++;
                continue;
            }
            if (jp->p < jp->end && *jp->p == (obj ? '}' : ']')) {
                jp->p++;
                break;
            }
            jp->failed = true;
        }
        return v;
    }
    if (c == '"') {
        v->kind = J_STR;
        v->str = jstring(jp);
        return v;
    }
    if (jp->end - jp->p >= 4 && !strncmp(jp->p, "true", 4)) {
        v->kind = J_BOOL;
        v->b = true;
        jp->p += 4;
        return v;
    }
    if (jp->end - jp->p >= 5 && !strncmp(jp->p, "false", 5)) {
        v->kind = J_BOOL;
        jp->p += 5;
        return v;
    }
    if (jp->end - jp->p >= 4 && !strncmp(jp->p, "null", 4)) {
        jp->p += 4;
        return v;
    }
    char* stop;
    v->kind = J_NUM;
    v->num = strtod(jp->p, &stop);
    if (stop == jp->p) jp->failed = true;
    jp->p = stop;
    return v;
}

static J* jget(const J* o, const char* key) {
    if (!o || o->kind != J_OBJ) return NULL;
    for (int i = 0; i < o->count; i++)
        if (!strcmp(o->keys[i], key)) return o->items[i];
    return NULL;
}

static const char* jstr(const J* o, const char* key) {
    J* v = jget(o, key);
    return v && v->kind == J_STR ? v->str : NULL;
}

static int jint(const J* o, const char* key, int fallback) {
    J* v = jget(o, key);
    return v && v->kind == J_NUM ? (int)v->num : fallback;
}

/* ---- writing ------------------------------------------------------------------------------------- */

typedef struct Out {
    QkBuf nodes;      /* this function's nodes, written as they are met */
    int nextId;
    const char* fn;
    int nodeCount;
    const J* layout;  /* {"key": [x, y]} or NULL */
} Out;

static void jsonString(QkBuf* b, const char* s) {
    qkBufByte(b, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"') qkBufPuts(b, "\\\"");
        else if (c == '\\') qkBufPuts(b, "\\\\");
        else if (c == '\n') qkBufPuts(b, "\\n");
        else if (c == '\t') qkBufPuts(b, "\\t");
        else if (c < 0x20) qkBufPrintf(b, "\\u%04x", c);
        else qkBufByte(b, c);
    }
    qkBufByte(b, '"');
}

static void typeString(const TypeExpr* t, QkBuf* b) {
    QkBuf s = {0};
    qkFormatTypeExpr(t, &s);
    jsonString(b, s.data);
    qkBufFree(&s);
}

static const Expr* visible(const Expr* e) {
    while (e->kind == E_CALL && e->implicit) e = e->args[0];
    return e;
}

static char* keyJoin(const char* a, const char* sep, const char* b) {
    QkBuf k = {0};
    qkBufPrintf(&k, "%s%s%s", a, sep, b);
    char* s = qkDup(k.data, k.len);
    qkBufFree(&k);
    return s;
}

static char* keyIndex(const char* a, const char* sep, int i) {
    char n[16];
    snprintf(n, sizeof n, "%d", i);
    return keyJoin(a, sep, n);
}

static void writeInput(Out* o, const Expr* e, QkBuf* b, const char* key);
static void writeBlock(Out* o, const Block* bl, QkBuf* b, const char* prefix);
static int writeStmt(Out* o, const Stmt* s, const char* key);

/* Every node: its id, kind, key (stable while the code around it keeps its shape), line and, from the
 * layout, its position. */
static void startNode(Out* o, int id, const char* kind, const char* key, int line) {
    if (o->nodeCount++) qkBufPuts(&o->nodes, ",\n");
    qkBufPrintf(&o->nodes, "    {\"id\": %d, \"kind\": ", id);
    jsonString(&o->nodes, kind);
    qkBufPuts(&o->nodes, ", \"key\": ");
    jsonString(&o->nodes, key);
    if (line > 0) qkBufPrintf(&o->nodes, ", \"line\": %d", line);
    const J* pos = jget(o->layout, key);
    if (pos && pos->kind == J_ARR && pos->count == 2 && pos->items[0]->kind == J_NUM && pos->items[1]->kind == J_NUM)
        qkBufPrintf(&o->nodes, ", \"pos\": [%g, %g]", pos->items[0]->num, pos->items[1]->num);
}

static void typeField(Out* o, const Type* t) {
    if (!t || t->kind == TY_ERROR || t->kind == TY_VOID) return;
    QkBuf s = {0};
    qkTypeName(t, &s);
    qkBufPuts(&o->nodes, ", \"type\": ");
    jsonString(&o->nodes, s.data);
    qkBufFree(&s);
}

static void notes(QkBuf* b, const char* leading, const char* trailing, bool blank) {
    if (leading) {
        qkBufPuts(b, ", \"comment\": ");
        jsonString(b, leading);
    }
    if (trailing) {
        qkBufPuts(b, ", \"trailing\": ");
        jsonString(b, trailing);
    }
    if (blank) qkBufPuts(b, ", \"blank\": true");
}

/* inputs: the i-th one's key is key.i */
static void inputs(Out* o, QkBuf* node, Expr* const* list, int n, const char* key) {
    QkBuf in = {0};
    for (int i = 0; i < n; i++) {
        if (i) qkBufPuts(&in, ", ");
        writeInput(o, list[i], &in, keyIndex(key, ".", i));
    }
    qkBufPuts(node, ", \"inputs\": [");
    qkBufAdd(node, in.data ? in.data : "", in.len);
    qkBufPuts(node, "]");
    qkBufFree(&in);
}

/* An expression as a node; returns its id. Its inputs are written (and numbered) first. */
static int writeNode(Out* o, const Expr* e, const char* key) {
    e = visible(e);
    int id = o->nextId++;
    QkBuf node = {0};
    Expr* pair[2];
    switch (e->kind) {
    case E_CALL:
        qkBufPuts(&node, ", \"fn\": ");
        jsonString(&node, e->name);
        if (e->method) qkBufPuts(&node, ", \"method\": true");
        if (e->argCount && e->argNames && e->argNames[0]) {
            qkBufPuts(&node, ", \"names\": [");
            for (int i = 0; i < e->argCount; i++) {
                if (i) qkBufPuts(&node, ", ");
                jsonString(&node, e->argNames[i] ? e->argNames[i] : "");
            }
            qkBufPuts(&node, "]");
        }
        inputs(o, &node, e->args, e->argCount, key);
        break;
    case E_BINARY: case E_RANGE: case E_UNARY:
        qkBufPuts(&node, ", \"op\": ");
        jsonString(&node, e->kind == E_UNARY ? (e->op == T_NOT ? "not" : "neg") : qkTokName((TokKind)e->op));
        pair[0] = e->a;
        pair[1] = e->b;
        inputs(o, &node, pair, e->kind == E_UNARY ? 1 : 2, key);
        break;
    case E_FIELD:
        qkBufPuts(&node, ", \"field\": ");
        jsonString(&node, e->name);
        inputs(o, &node, &((Expr*)e)->a, 1, key);
        break;
    case E_INDEX:
        pair[0] = e->a;
        pair[1] = e->b;
        inputs(o, &node, pair, 2, key);
        break;
    case E_ARRAY: inputs(o, &node, e->args, e->argCount, key); break;
    case E_LAMBDA: {
        qkBufPuts(&node, ", \"params\": [");
        for (int i = 0; i < e->paramCount; i++) {
            int pid = o->nextId++;
            e->params[i].binding->nodeId = pid;
            qkBufPrintf(&node, "%s{\"id\": %d, \"name\": ", i ? ", " : "", pid);
            jsonString(&node, e->params[i].name);
            qkBufPuts(&node, "}");
        }
        qkBufPuts(&node, "]");
        inputs(o, &node, &((Expr*)e)->body, 1, key);
        break;
    }
    case E_IF:
        inputs(o, &node, &((Expr*)e)->a, 1, key);
        qkBufPuts(&node, ", \"then\": ");
        writeBlock(o, e->thenBlock, &node, keyJoin(key, "", "/then"));
        if (e->elseExpr && e->elseExpr->kind == E_IF) {
            qkBufPrintf(&node, ", \"elseIf\": %d", writeNode(o, e->elseExpr, keyJoin(key, "", "/else")));
        } else if (e->elseExpr) {
            qkBufPuts(&node, ", \"else\": ");
            writeBlock(o, e->elseExpr->block, &node, keyJoin(key, "", "/else"));
        }
        break;
    case E_MATCH:
        inputs(o, &node, &((Expr*)e)->a, 1, key);
        qkBufPuts(&node, ", \"arms\": [");
        for (int i = 0; i < e->armCount; i++) {
            const Arm* a = &e->arms[i];
            char* armKey = keyIndex(key, "/arm", i);
            qkBufPuts(&node, i ? ", {\"pattern\": " : "{\"pattern\": ");
            jsonString(&node, a->pattern ? a->pattern : a->intPattern ? a->intPattern->raw : "_");
            if (a->comment) {
                qkBufPuts(&node, ", \"comment\": ");
                jsonString(&node, a->comment);
            }
            if (a->body->kind == E_BLOCK) {
                qkBufPuts(&node, ", \"block\": ");
                writeBlock(o, a->body->block, &node, armKey);
            } else {
                qkBufPuts(&node, ", \"value\": ");
                writeInput(o, a->body, &node, armKey);
            }
            qkBufPuts(&node, "}");
        }
        qkBufPuts(&node, "]");
        break;
    case E_BLOCK:
        qkBufPuts(&node, ", \"block\": ");
        writeBlock(o, e->block, &node, keyJoin(key, "", "/block"));
        break;
    default: break;
    }
    static const char* const kinds[] = {"value", "value", "value", "value", "name", "call", "field", "index", "op",
                                        "op", "op", "if", "match", "lambda", "array", "block", "case"};
    startNode(o, id, kinds[e->kind], key, e->line);
    typeField(o, e->type);
    qkBufAdd(&o->nodes, node.data ? node.data : "", node.len);
    qkBufPuts(&o->nodes, "}");
    qkBufFree(&node);
    return id;
}

static void writeInput(Out* o, const Expr* e, QkBuf* b, const char* key) {
    e = visible(e);
    if (e->kind == E_LAMBDA && e->sugar) e = e->sugar;
    switch (e->kind) {
    case E_INT: case E_FLOAT: case E_TEXT: case E_BOOL: {
        static const char* const of[] = {"Int", "Float", "Text", "Bool"};
        qkBufPuts(b, "{\"value\": ");
        jsonString(b, e->kind == E_BOOL ? (e->i ? "true" : "false") : e->raw);
        qkBufPuts(b, ", \"of\": ");
        jsonString(b, of[e->kind]);
        qkBufPuts(b, "}");
        return;
    }
    case E_CASE:
        qkBufPrintf(b, "{\"case\": \"%s.%s\"}", e->decl->name, e->name);
        return;
    case E_NAME:
        if (e->binding) {
            qkBufPrintf(b, "{\"node\": %d}", e->binding->nodeId);
        } else { /* a function given where a lambda goes */
            qkBufPuts(b, "{\"fn\": ");
            jsonString(b, e->name);
            qkBufPuts(b, "}");
        }
        return;
    default:
        qkBufPrintf(b, "{\"node\": %d}", writeNode(o, e, key));
    }
}

/* A statement's key: fn/name for the named ones (let, var, for), else its place: prefix/@index. */
static const char* stmtKey(Out* o, const Stmt* s, const char* prefix, int i) {
    if (s->kind == S_LET || s->kind == S_VAR || s->kind == S_FOR) return keyJoin(o->fn, "/", s->name);
    return keyIndex(prefix, "/@", i);
}

static void writeBlock(Out* o, const Block* bl, QkBuf* b, const char* prefix) {
    int* ids = qkNew(sizeof(int) * (size_t)(bl->count ? bl->count : 1));
    for (int i = 0; i < bl->count; i++) ids[i] = writeStmt(o, bl->stmts[i], stmtKey(o, bl->stmts[i], prefix, i));
    qkBufPuts(b, "{\"stmts\": [");
    for (int i = 0; i < bl->count; i++) qkBufPrintf(b, "%s%d", i ? ", " : "", ids[i]);
    qkBufPuts(b, "]");
    if (bl->closingComment) {
        qkBufPuts(b, ", \"closing\": ");
        jsonString(b, bl->closingComment);
    }
    qkBufPuts(b, "}");
}

static int writeStmt(Out* o, const Stmt* s, const char* key) {
    QkBuf node = {0};
    int id = o->nextId++;
    const char* kind;
    const char* in = key; /* inputs: key.0, key.1... */
    switch (s->kind) {
    case S_EXPR: /* a "do" node holds the statement's place in the order; the expression is its input */
        kind = "do";
        inputs(o, &node, &((Stmt*)s)->value, 1, in);
        break;
    case S_LET: case S_VAR:
        s->binding->nodeId = id;
        kind = s->kind == S_LET ? "let" : "var";
        qkBufPuts(&node, ", \"name\": ");
        jsonString(&node, s->name);
        if (s->typeAnn) {
            qkBufPuts(&node, ", \"annotation\": ");
            typeString(s->typeAnn, &node);
        }
        inputs(o, &node, &((Stmt*)s)->value, 1, in);
        break;
    case S_ASSIGN: {
        kind = "set";
        qkBufPuts(&node, ", \"op\": ");
        jsonString(&node, qkTokName((TokKind)s->op));
        Expr* two[2] = {s->target, s->value};
        inputs(o, &node, two, 2, in);
        break;
    }
    case S_FOR:
        kind = "for";
        s->binding->nodeId = id;
        qkBufPuts(&node, ", \"name\": ");
        jsonString(&node, s->name);
        if (s->indexName) {
            int ix = o->nextId++;
            s->indexBinding->nodeId = ix;
            qkBufPrintf(&node, ", \"index\": {\"id\": %d, \"name\": ", ix);
            jsonString(&node, s->indexName);
            qkBufPuts(&node, "}");
        }
        inputs(o, &node, &((Stmt*)s)->iter, 1, in);
        qkBufPuts(&node, ", \"body\": ");
        writeBlock(o, s->body, &node, key);
        break;
    case S_WHILE:
        kind = "while";
        inputs(o, &node, &((Stmt*)s)->value, 1, in);
        qkBufPuts(&node, ", \"body\": ");
        writeBlock(o, s->body, &node, key);
        break;
    case S_RETURN:
        kind = "return";
        if (s->value) inputs(o, &node, &((Stmt*)s)->value, 1, in);
        break;
    default: kind = s->kind == S_BREAK ? "break" : "continue";
    }
    notes(&node, s->leading, s->trailing, s->blankBefore);
    startNode(o, id, kind, key, s->line);
    qkBufAdd(&o->nodes, node.data ? node.data : "", node.len);
    qkBufPuts(&o->nodes, "}");
    qkBufFree(&node);
    return id;
}

static void writeParams(QkBuf* out, const Decl* d, Out* o) {
    qkBufPuts(out, ", \"params\": [");
    for (int k = 0; k < d->paramCount; k++) {
        int pid = o ? o->nextId++ : k + 1;
        if (d->params[k].binding) d->params[k].binding->nodeId = pid;
        qkBufPrintf(out, "%s{\"id\": %d, \"name\": ", k ? ", " : "", pid);
        jsonString(out, d->params[k].name);
        qkBufPuts(out, ", \"type\": ");
        typeString(d->params[k].type, out);
        qkBufPuts(out, ", \"key\": ");
        jsonString(out, keyJoin(d->name, "/", d->params[k].name));
        qkBufPuts(out, "}");
    }
    qkBufPuts(out, "]");
    if (d->ret) {
        qkBufPuts(out, ", \"returns\": ");
        typeString(d->ret, out);
    }
}

void qkGraphExportWith(const Program* p, QkBuf* out, const char* layoutJson) {
    const J* layout = NULL;
    if (layoutJson) {
        JParser jp = {layoutJson, layoutJson + strlen(layoutJson), false};
        layout = jvalue(&jp);
        if (jp.failed || layout->kind != J_OBJ) layout = NULL;
    }
    qkBufPuts(out, "{\"quark\": 1, \"decls\": [\n");
    int written = 0;
    for (int i = 0; i < p->count; i++) {
        const Decl* d = p->decls[i];
        if (d->fromPrelude) continue; /* the host's: in the catalog, not in the program */
        qkBufPuts(out, written++ ? ",\n  {\"kind\": " : "  {\"kind\": ");
        const char* kind = d->kind == D_FN ? (d->isExtern ? "extern fn" : "fn")
                           : d->kind == D_TYPE ? (d->isExtern ? "extern type" : "type") : "enum";
        jsonString(out, kind);
        qkBufPuts(out, ", \"name\": ");
        jsonString(out, d->name);
        notes(out, d->leading, d->trailing, false);
        if (d->kind == D_ENUM) {
            qkBufPuts(out, ", \"cases\": [");
            for (int k = 0; k < d->caseCount; k++) {
                if (k) qkBufPuts(out, ", ");
                jsonString(out, d->cases[k]);
            }
            qkBufPuts(out, "]}");
            continue;
        }
        if (d->kind == D_TYPE) {
            if (!d->isExtern) {
                qkBufPuts(out, ", \"fields\": [");
                for (int k = 0; k < d->paramCount; k++) {
                    qkBufPuts(out, k ? ", {\"name\": " : "{\"name\": ");
                    jsonString(out, d->params[k].name);
                    qkBufPuts(out, ", \"type\": ");
                    typeString(d->params[k].type, out);
                    qkBufPuts(out, "}");
                }
                qkBufPuts(out, "]");
            }
            qkBufPuts(out, "}");
            continue;
        }
        if (d->isExtern) {
            writeParams(out, d, NULL);
            qkBufPuts(out, "}");
            continue;
        }
        Out o = {{0}, 1, d->name, 0, layout};
        writeParams(out, d, &o);
        QkBuf body = {0};
        if (d->body->kind == E_BLOCK) {
            qkBufPuts(&body, ", \"form\": \"block\", \"body\": ");
            writeBlock(&o, d->body->block, &body, d->name);
        } else {
            qkBufPuts(&body, ", \"form\": \"expr\", \"result\": ");
            writeInput(&o, d->body, &body, keyJoin(d->name, "", "/="));
        }
        qkBufAdd(out, body.data, body.len);
        qkBufFree(&body);
        qkBufPuts(out, ",\n   \"nodes\": [\n");
        qkBufAdd(out, o.nodes.data ? o.nodes.data : "", o.nodes.len);
        qkBufPuts(out, "]}");
        qkBufFree(&o.nodes);
    }
    qkBufPuts(out, "]");
    if (p->closingComment) {
        qkBufPuts(out, ",\n \"closing\": ");
        jsonString(out, p->closingComment);
    }
    qkBufPuts(out, "}\n");
}

void qkGraphExport(const Program* p, QkBuf* out) { qkGraphExportWith(p, out, NULL); }

/* The key of the statement a line belongs to (the last one starting at or before it, in the function that
 * holds it): how an error in a graph's text points at a node. */
static void keyInBlock(Out* o, const Block* bl, const char* prefix, int line, const char** best, int* bestLine) {
    for (int i = 0; i < bl->count; i++) {
        const Stmt* s = bl->stmts[i];
        const char* key = stmtKey(o, s, prefix, i);
        if (s->line <= line && s->line >= *bestLine) {
            *best = key;
            *bestLine = s->line;
        }
        if (s->body) keyInBlock(o, s->body, key, line, best, bestLine);
    }
}

bool qkKeyAtLine(const Program* p, int line, char* out, size_t size) {
    const char* best = NULL;
    int bestLine = 0;
    for (int i = 0; i < p->count; i++) {
        const Decl* d = p->decls[i];
        if (d->fromPrelude || d->line > line) continue;
        if (d->line >= bestLine) {
            best = d->name;
            bestLine = d->line;
        }
        if (d->kind == D_FN && d->body && d->body->kind == E_BLOCK) {
            Out o = {{0}, 1, d->name, 0, NULL};
            keyInBlock(&o, d->body->block, d->name, line, &best, &bestLine);
        }
    }
    if (!best) return false;
    snprintf(out, size, "%s", best);
    return true;
}

/* ---- graph to syntax tree ------------------------------------------------------------------------ */

typedef struct In {
    QkSource* src;
    J** byId;           /* this function's nodes */
    const char** names; /* the name a binding node carries */
    int maxId;
    bool failed;
} In;

static void inFail(In* in, const char* what, int id) {
    if (!in->failed) qkError(in->src, 0, 0, "the graph is not valid: %s (node %d)", what, id);
    in->failed = true;
}

static TypeExpr* parseTypeText(const char* s) {
    TypeExpr* t = qkNew(sizeof *t);
    if (!s) s = "Int";
    if (s[0] == '[') {
        size_t n = strlen(s);
        t->elem = parseTypeText(qkDup(s + 1, n >= 2 ? n - 2 : 0));
    } else {
        t->name = s;
    }
    return t;
}

static Expr* buildNode(In* in, int id);
static Block* buildBlock(In* in, const J* b);
static Stmt* buildStmt(In* in, int id);

static Expr* zero(void) { return qkNewExpr(E_INT, 0, 0); }

static Expr* literal(const char* raw, const char* of) {
    Expr* e;
    if (!strcmp(of, "Bool")) {
        e = qkNewExpr(E_BOOL, 0, 0);
        e->i = !strcmp(raw, "true");
        return e;
    }
    if (!strcmp(of, "Text")) {
        e = qkNewExpr(E_TEXT, 0, 0);
        e->raw = raw;
        QkBuf b = {0};
        size_t n = strlen(raw);
        const char* s = raw + 1;
        const char* end = raw + (n > 1 ? n - 1 : n);
        while (s < end) {
            char c = *s++;
            if (c == '\\' && s < end) {
                char k = *s++;
                c = k == 'n' ? '\n' : k == 't' ? '\t' : k;
            }
            qkBufByte(&b, (uint8_t)c);
        }
        e->text = qkDup(b.data ? b.data : "", b.len);
        e->textLen = (int)b.len;
        qkBufFree(&b);
        return e;
    }
    e = qkNewExpr(!strcmp(of, "Float") ? E_FLOAT : E_INT, 0, 0);
    e->raw = raw;
    char clean[128];
    int n = 0;
    for (const char* p = raw; *p && n < 127; p++)
        if (*p != '_') clean[n++] = *p;
    clean[n] = '\0';
    if (e->kind == E_INT) e->i = strtoll(clean, NULL, 10);
    else e->f = strtod(clean, NULL);
    return e;
}

static Expr* buildInput(In* in, const J* input) {
    if (!input || input->kind != J_OBJ) {
        inFail(in, "an input that is not an object", 0);
        return zero();
    }
    const char* raw = jstr(input, "value");
    if (raw) return literal(raw, jstr(input, "of") ? jstr(input, "of") : "Int");
    const char* cs = jstr(input, "case");
    if (cs) {
        const char* dot = strchr(cs, '.');
        Expr* type = qkNewExpr(E_NAME, 0, 0);
        type->name = qkDup(cs, dot ? (size_t)(dot - cs) : strlen(cs));
        Expr* f = qkNewExpr(E_FIELD, 0, 0);
        f->a = type;
        f->name = dot ? dot + 1 : "";
        return f;
    }
    const char* fn = jstr(input, "fn");
    if (fn) {
        Expr* e = qkNewExpr(E_NAME, 0, 0);
        e->name = fn;
        return e;
    }
    int id = jint(input, "node", -1);
    if (id < 0 || id > in->maxId) {
        inFail(in, "a wire from a node that does not exist", id);
        return zero();
    }
    if (in->names[id]) { /* a named node: the wire is its name */
        Expr* e = qkNewExpr(E_NAME, 0, 0);
        e->name = in->names[id];
        return e;
    }
    return buildNode(in, id);
}

static Expr** buildInputs(In* in, const J* node, int* count) {
    J* list = jget(node, "inputs");
    int n = list && list->kind == J_ARR ? list->count : 0;
    Expr** out = qkNew(sizeof(Expr*) * (size_t)(n ? n : 1));
    for (int i = 0; i < n; i++) out[i] = buildInput(in, list->items[i]);
    *count = n;
    return out;
}

static Expr* buildNode(In* in, int id) {
    J* node = id >= 0 && id <= in->maxId ? in->byId[id] : NULL;
    if (!node) {
        inFail(in, "a missing node", id);
        return zero();
    }
    const char* kind = jstr(node, "kind");
    if (!kind) kind = "";
    if (!strcmp(kind, "lambda")) { /* the parameters are named before the body is read */
        Expr* e = qkNewExpr(E_LAMBDA, 0, 0);
        J* ps = jget(node, "params");
        int pn = ps ? ps->count : 0;
        e->params = qkNew(sizeof(Param) * (size_t)(pn ? pn : 1));
        e->paramCount = pn;
        for (int i = 0; i < pn; i++) {
            e->params[i].name = jstr(ps->items[i], "name");
            int pid = jint(ps->items[i], "id", -1);
            if (pid >= 0 && pid <= in->maxId) in->names[pid] = e->params[i].name;
        }
        J* list = jget(node, "inputs");
        e->body = buildInput(in, list && list->count ? list->items[0] : NULL);
        return e;
    }
    int n;
    Expr** args = buildInputs(in, node, &n);
    if (!strcmp(kind, "call")) {
        Expr* e = qkNewExpr(E_CALL, 0, 0);
        e->name = jstr(node, "fn");
        J* m = jget(node, "method");
        e->method = m && m->kind == J_BOOL && m->b;
        e->args = args;
        e->argCount = e->argCap = n;
        e->argNames = qkNew(sizeof(char*) * (size_t)(n ? n : 1));
        J* names = jget(node, "names");
        for (int i = 0; names && i < names->count && i < n; i++)
            if (names->items[i]->kind == J_STR && names->items[i]->str[0]) e->argNames[i] = names->items[i]->str;
        if (!e->name) inFail(in, "a call without a function", id);
        return e;
    }
    if (!strcmp(kind, "op")) {
        const char* op = jstr(node, "op");
        if (!op) op = "+";
        if (!strcmp(op, "neg") || !strcmp(op, "not")) {
            Expr* e = qkNewExpr(E_UNARY, 0, 0);
            e->op = !strcmp(op, "neg") ? T_MINUS : T_NOT;
            e->a = n > 0 ? args[0] : zero();
            return e;
        }
        Expr* e = qkNewExpr(!strcmp(op, "..") ? E_RANGE : E_BINARY, 0, 0);
        e->op = -1;
        for (int k = T_AND; k <= T_GE; k++)
            if (!strcmp(qkTokName((TokKind)k), op)) e->op = k;
        if (e->op < 0) inFail(in, "an unknown operator", id);
        e->a = n > 0 ? args[0] : zero();
        e->b = n > 1 ? args[1] : zero();
        return e;
    }
    if (!strcmp(kind, "field")) {
        Expr* e = qkNewExpr(E_FIELD, 0, 0);
        e->name = jstr(node, "field");
        e->a = n > 0 ? args[0] : zero();
        return e;
    }
    if (!strcmp(kind, "index")) {
        Expr* e = qkNewExpr(E_INDEX, 0, 0);
        e->a = n > 0 ? args[0] : zero();
        e->b = n > 1 ? args[1] : zero();
        return e;
    }
    if (!strcmp(kind, "array")) {
        Expr* e = qkNewExpr(E_ARRAY, 0, 0);
        e->args = args;
        e->argCount = e->argCap = n;
        e->argNames = qkNew(sizeof(char*) * (size_t)(n ? n : 1));
        return e;
    }
    if (!strcmp(kind, "if")) {
        Expr* e = qkNewExpr(E_IF, 0, 0);
        e->a = n > 0 ? args[0] : zero();
        e->thenBlock = buildBlock(in, jget(node, "then"));
        int elseIf = jint(node, "elseIf", -1);
        if (elseIf >= 0) {
            e->elseExpr = buildNode(in, elseIf);
        } else if (jget(node, "else")) {
            e->elseExpr = qkNewExpr(E_BLOCK, 0, 0);
            e->elseExpr->block = buildBlock(in, jget(node, "else"));
        }
        return e;
    }
    if (!strcmp(kind, "match")) {
        Expr* e = qkNewExpr(E_MATCH, 0, 0);
        e->a = n > 0 ? args[0] : zero();
        J* arms = jget(node, "arms");
        for (int i = 0; arms && i < arms->count; i++) {
            J* a = arms->items[i];
            Arm arm = {0};
            const char* pat = jstr(a, "pattern");
            if (pat && strcmp(pat, "_")) {
                if (pat[0] == '-' || (pat[0] >= '0' && pat[0] <= '9')) arm.intPattern = literal(pat, "Int");
                else arm.pattern = pat;
            }
            arm.comment = jstr(a, "comment");
            if (jget(a, "block")) {
                arm.body = qkNewExpr(E_BLOCK, 0, 0);
                arm.body->block = buildBlock(in, jget(a, "block"));
            } else {
                arm.body = buildInput(in, jget(a, "value"));
            }
            QK_PUSH(e->arms, e->armCount, e->armCap, arm);
        }
        return e;
    }
    if (!strcmp(kind, "block")) {
        Expr* e = qkNewExpr(E_BLOCK, 0, 0);
        e->block = buildBlock(in, jget(node, "block"));
        return e;
    }
    inFail(in, "an unknown kind of node", id);
    return zero();
}

static Block* buildBlock(In* in, const J* b) {
    Block* bl = qkNew(sizeof *bl);
    J* stmts = jget(b, "stmts");
    for (int i = 0; stmts && i < stmts->count; i++) {
        Stmt* s = buildStmt(in, (int)stmts->items[i]->num);
        QK_PUSH(bl->stmts, bl->count, bl->cap, s);
    }
    bl->closingComment = jstr(b, "closing");
    return bl;
}

static Stmt* buildStmt(In* in, int id) {
    J* node = id >= 0 && id <= in->maxId ? in->byId[id] : NULL;
    Stmt* s = qkNew(sizeof *s);
    if (!node) {
        inFail(in, "a missing statement", id);
        s->kind = S_BREAK;
        return s;
    }
    const char* kind = jstr(node, "kind");
    if (!kind) kind = "";
    s->leading = jstr(node, "comment");
    s->trailing = jstr(node, "trailing");
    J* blank = jget(node, "blank");
    s->blankBefore = blank && blank->kind == J_BOOL && blank->b;
    J* list = jget(node, "inputs");
    J* first = list && list->kind == J_ARR && list->count ? list->items[0] : NULL;
    if (!strcmp(kind, "let") || !strcmp(kind, "var")) {
        s->kind = kind[0] == 'l' ? S_LET : S_VAR;
        s->name = jstr(node, "name");
        if (jstr(node, "annotation")) s->typeAnn = parseTypeText(jstr(node, "annotation"));
        s->value = buildInput(in, first);
        in->names[id] = s->name; /* from here on, wires from it are its name */
    } else if (!strcmp(kind, "set")) {
        s->kind = S_ASSIGN;
        const char* op = jstr(node, "op");
        s->op = T_ASSIGN;
        for (int k = T_ASSIGN; k <= T_SLASHEQ; k++)
            if (op && !strcmp(qkTokName((TokKind)k), op)) s->op = k;
        s->target = buildInput(in, first);
        s->value = buildInput(in, list && list->count > 1 ? list->items[1] : NULL);
    } else if (!strcmp(kind, "for")) {
        s->kind = S_FOR;
        s->name = jstr(node, "name");
        in->names[id] = s->name;
        J* index = jget(node, "index");
        if (index) {
            s->indexName = jstr(index, "name");
            int ix = jint(index, "id", -1);
            if (ix >= 0 && ix <= in->maxId) in->names[ix] = s->indexName;
        }
        s->iter = buildInput(in, first);
        s->body = buildBlock(in, jget(node, "body"));
    } else if (!strcmp(kind, "while")) {
        s->kind = S_WHILE;
        s->value = buildInput(in, first);
        s->body = buildBlock(in, jget(node, "body"));
    } else if (!strcmp(kind, "return")) {
        s->kind = S_RETURN;
        if (first) s->value = buildInput(in, first);
    } else if (!strcmp(kind, "break")) {
        s->kind = S_BREAK;
    } else if (!strcmp(kind, "continue")) {
        s->kind = S_CONTINUE;
    } else if (!strcmp(kind, "do")) {
        s->kind = S_EXPR;
        s->value = buildInput(in, first);
    } else {
        inFail(in, "an unknown kind of statement", id);
        s->kind = S_BREAK;
    }
    return s;
}

static int maxIdOf(const J* dj) {
    int maxId = 0;
    J* params = jget(dj, "params");
    for (int k = 0; params && k < params->count; k++)
        if (jint(params->items[k], "id", 0) > maxId) maxId = jint(params->items[k], "id", 0);
    J* nodes = jget(dj, "nodes");
    for (int k = 0; nodes && k < nodes->count; k++) {
        J* nd = nodes->items[k];
        if (jint(nd, "id", 0) > maxId) maxId = jint(nd, "id", 0);
        J* ix = jget(nd, "index");
        if (ix && jint(ix, "id", 0) > maxId) maxId = jint(ix, "id", 0);
        J* ps = jget(nd, "params");
        for (int q = 0; ps && q < ps->count; q++)
            if (jint(ps->items[q], "id", 0) > maxId) maxId = jint(ps->items[q], "id", 0);
    }
    return maxId;
}

bool qkGraphImport(const char* json, size_t len, const char* path, Program* out) {
    memset(out, 0, sizeof *out);
    QkSource* src = qkNew(sizeof *src);
    src->path = path;
    out->src = *src;
    JParser jp = {json, json + len, false};
    J* root = jvalue(&jp);
    J* decls = jget(root, "decls");
    if (jp.failed || !decls || decls->kind != J_ARR) {
        qkError(src, 0, 0, "this is not a Quark graph (JSON with \"decls\")");
        return false;
    }
    bool ok = true;
    for (int i = 0; i < decls->count; i++) {
        J* dj = decls->items[i];
        Decl* d = qkNew(sizeof *d);
        const char* kind = jstr(dj, "kind");
        d->name = jstr(dj, "name");
        d->leading = jstr(dj, "comment");
        d->trailing = jstr(dj, "trailing");
        d->index = i;
        if (!kind || !d->name) {
            qkError(src, 0, 0, "declaration %d has no kind or name", i + 1);
            return false;
        }
        if (!strcmp(kind, "enum")) {
            d->kind = D_ENUM;
            J* cases = jget(dj, "cases");
            for (int k = 0; cases && k < cases->count; k++)
                if (cases->items[k]->kind == J_STR) QK_PUSH(d->cases, d->caseCount, d->caseCap, cases->items[k]->str);
        } else if (!strcmp(kind, "extern type")) {
            d->kind = D_TYPE;
            d->isExtern = true;
        } else if (!strcmp(kind, "extern fn")) {
            d->kind = D_FN;
            d->isExtern = true;
            J* params = jget(dj, "params");
            for (int k = 0; params && k < params->count; k++) {
                Param prm = {jstr(params->items[k], "name"), parseTypeText(jstr(params->items[k], "type")), 0, 0, NULL};
                QK_PUSH(d->params, d->paramCount, d->paramCap, prm);
            }
            if (jstr(dj, "returns")) d->ret = parseTypeText(jstr(dj, "returns"));
        } else if (!strcmp(kind, "type")) {
            d->kind = D_TYPE;
            J* fields = jget(dj, "fields");
            for (int k = 0; fields && k < fields->count; k++) {
                Param f = {jstr(fields->items[k], "name"), parseTypeText(jstr(fields->items[k], "type")), 0, 0, NULL};
                QK_PUSH(d->params, d->paramCount, d->paramCap, f);
            }
        } else {
            d->kind = D_FN;
            In in = {src, NULL, NULL, maxIdOf(dj), false};
            in.byId = qkNew(sizeof(J*) * (size_t)(in.maxId + 1));
            in.names = qkNew(sizeof(char*) * (size_t)(in.maxId + 1));
            J* nodes = jget(dj, "nodes");
            for (int k = 0; nodes && k < nodes->count; k++) {
                int nid = jint(nodes->items[k], "id", -1);
                if (nid >= 0) in.byId[nid] = nodes->items[k];
            }
            J* params = jget(dj, "params");
            for (int k = 0; params && k < params->count; k++) {
                J* pj = params->items[k];
                Param prm = {jstr(pj, "name"), parseTypeText(jstr(pj, "type")), 0, 0, NULL};
                QK_PUSH(d->params, d->paramCount, d->paramCap, prm);
                int pid = jint(pj, "id", -1);
                if (pid >= 0) in.names[pid] = prm.name;
            }
            if (jstr(dj, "returns")) d->ret = parseTypeText(jstr(dj, "returns"));
            const char* form = jstr(dj, "form");
            if (form && !strcmp(form, "expr")) {
                d->body = buildInput(&in, jget(dj, "result"));
            } else {
                d->body = qkNewExpr(E_BLOCK, 0, 0);
                d->body->block = buildBlock(&in, jget(dj, "body"));
            }
            ok &= !in.failed;
        }
        QK_PUSH(out->decls, out->count, out->cap, d);
    }
    out->closingComment = jstr(root, "closing");
    return ok;
}
