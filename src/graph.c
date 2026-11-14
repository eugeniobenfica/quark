/* Quark — programs as node graphs, and back.
 *
 * Every expression that is not a plain value is a node; its inputs are wires from other nodes or values typed
 * into the pin. Every let, var, parameter and loop variable is a named node, and a use of the name is a wire
 * from it. Statements run in the order of their block's list: that order is the execution wire. Literals,
 * enum cases and function names are pin values, never nodes. Comments and blank lines ride on the nodes, so
 * text -> graph -> text gives back the formatter's text exactly.
 *
 * {"quark": 1, "decls": [
 *   {"kind": "fn", "name": "square", "params": [{"id": 1, "name": "x", "type": "Int"}], "returns": "Int",
 *    "form": "expr", "result": {"node": 4}, "nodes": [{"id": 4, "kind": "op", "op": "+", "inputs": [...]}, ...]},
 *   {"kind": "type", "name": "Point", "fields": [{"name": "x", "type": "Float"}]},
 *   {"kind": "enum", "name": "Color", "cases": ["Red", "Green"]}]}
 *
 * Inputs: {"node": id} | {"value": "12", "of": "Int"} | {"case": "Color.Red"} | {"fn": "square"}.
 * Blocks: {"stmts": [ids], "closing": "comment"}. A node's "key" (fn/name, for named nodes) is stable while
 * the name is: an editor keeps positions in a side file under it. */
#include "qk.h"

#include <stdio.h>
#include <stdlib.h>

/* ---- reading JSON -------------------------------------------------------------------------------- */

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
            else if (k == 'r') c = '\r';
            else if (k == 'b') c = '\b';
            else if (k == 'f') c = '\f';
            else if (k == 'u' && jp->p + 4 <= jp->end) { /* a code point, as UTF-8 (a surrogate pair is one) */
                char hex[5] = {jp->p[0], jp->p[1], jp->p[2], jp->p[3], 0};
                unsigned long u = strtoul(hex, NULL, 16);
                jp->p += 4;
                if (u >= 0xD800 && u < 0xDC00 && jp->p + 6 <= jp->end && jp->p[0] == '\\' && jp->p[1] == 'u') {
                    char low[5] = {jp->p[2], jp->p[3], jp->p[4], jp->p[5], 0};
                    unsigned long l = strtoul(low, NULL, 16);
                    if (l >= 0xDC00 && l < 0xE000) {
                        u = 0x10000 + ((u - 0xD800) << 10) + (l - 0xDC00);
                        jp->p += 6;
                    }
                }
                if (u < 0x80) {
                    qkBufByte(&b, (uint8_t)u);
                } else if (u < 0x800) {
                    qkBufByte(&b, (uint8_t)(0xC0 | (u >> 6)));
                    qkBufByte(&b, (uint8_t)(0x80 | (u & 63)));
                } else if (u < 0x10000) {
                    qkBufByte(&b, (uint8_t)(0xE0 | (u >> 12)));
                    qkBufByte(&b, (uint8_t)(0x80 | ((u >> 6) & 63)));
                    qkBufByte(&b, (uint8_t)(0x80 | (u & 63)));
                } else {
                    qkBufByte(&b, (uint8_t)(0xF0 | (u >> 18)));
                    qkBufByte(&b, (uint8_t)(0x80 | ((u >> 12) & 63)));
                    qkBufByte(&b, (uint8_t)(0x80 | ((u >> 6) & 63)));
                    qkBufByte(&b, (uint8_t)(0x80 | (u & 63)));
                }
                continue;
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

J* qkJsonGet(const J* o, const char* key) {
    if (!o || o->kind != J_OBJ) return NULL;
    for (int i = 0; i < o->count; i++)
        if (!strcmp(o->keys[i], key)) return o->items[i];
    return NULL;
}
static J* jget(const J* o, const char* key) { return qkJsonGet(o, key); }

J* qkJsonParse(const char* text, size_t len) {
    JParser jp = {text, text + len, false};
    J* v = jvalue(&jp);
    return jp.failed ? NULL : v;
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
    const Decl* decl; /* the function: which names its lets use more than once */
} Out;

void qkJsonString(QkBuf* b, const char* s) {
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

static void jsonString(QkBuf* b, const char* s) { qkJsonString(b, s); }

/* the node a name's uses wire to: a lambda's captured copy wires to the name it copies */
static int wireOf(const Binding* b) {
    while (b->capturedFrom) b = b->capturedFrom;
    return b->nodeId;
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

/* A pattern: "pattern" as text (Circle, 3, 80..90, "\"quit\"", _), and for a case its names, each a node the
 * wires from them come from. */
static void writePattern(Out* o, const Arm* a, QkBuf* node) {
    qkBufPuts(node, "\"pattern\": ");
    if (a->pattern) {
        jsonString(node, a->pattern);
    } else {
        QkBuf p = {0};
        qkFormatPattern(a, &p);
        jsonString(node, p.data);
        qkBufFree(&p);
    }
    if (a->bindCount || (a->pattern && !strcmp(a->pattern, "(,)"))) {
        qkBufPuts(node, ", \"binds\": [");
        for (int f = 0; f < a->bindCount; f++) {
            if (a->subs && a->subs[f]) { /* a pattern inside: its own pattern and names */
                qkBufPuts(node, f ? ", {" : "{");
                writePattern(o, a->subs[f], node);
                qkBufPuts(node, "}");
                continue;
            }
            int bid = o->nextId++;
            if (a->bindings && a->bindings[f]) a->bindings[f]->nodeId = bid;
            qkBufPrintf(node, "%s{\"id\": %d, \"name\": ", f ? ", " : "", bid);
            jsonString(node, a->binds[f]);
            qkBufPuts(node, "}");
        }
        qkBufPuts(node, "]");
    }
}

/* An expression as a node; returns its id. Its inputs are written (and numbered) first. */
static int writeNode(Out* o, const Expr* e, const char* key) {
    e = visible(e);
    int id = o->nextId++;
    QkBuf node = {0};
    Expr* pair[2];
    switch (e->kind) {
    case E_WITH:
        qkBufPuts(&node, ", \"names\": [");
        for (int i = 0; i < e->argCount; i++) {
            if (i) qkBufPuts(&node, ", ");
            jsonString(&node, e->argNames[i]);
        }
        qkBufPuts(&node, "]");
        {
            Expr** all = qkNew(sizeof(Expr*) * (size_t)(e->argCount + 1)); /* the base, then the new values */
            all[0] = e->a;
            for (int i = 0; i < e->argCount; i++) all[i + 1] = e->args[i];
            inputs(o, &node, all, e->argCount + 1, key);
        }
        break;
    case E_INTERP:
        qkBufPuts(&node, ", \"parts\": [");
        for (int i = 0; i <= e->argCount; i++) {
            if (i) qkBufPuts(&node, ", ");
            jsonString(&node, e->rawParts[i]);
        }
        qkBufPuts(&node, "]");
        if (e->formats) { /* "{x:.2}": each value's format, "" for none */
            qkBufPuts(&node, ", \"formats\": [");
            for (int i = 0; i < e->argCount; i++) {
                if (i) qkBufPuts(&node, ", ");
                jsonString(&node, e->formats[i] ? e->formats[i] : "");
            }
            qkBufPuts(&node, "]");
        }
        inputs(o, &node, e->args, e->argCount, key);
        break;
    case E_TRY: inputs(o, &node, &((Expr*)e)->a, 1, key); break;
    case E_MAP: inputs(o, &node, e->args, e->argCount, key); break;
    case E_CALL:
        qkBufPuts(&node, ", \"fn\": ");
        jsonString(&node, e->name);
        if (e->qualifier) {
            qkBufPuts(&node, ", \"qualifier\": ");
            jsonString(&node, e->qualifier);
        }
        if (e->decl && e->decl->kind == D_FN && e->decl->isEffect) qkBufPuts(&node, ", \"effect\": true");
        if (e->method) qkBufPuts(&node, ", \"method\": true");
        bool anyNamed = false;
        for (int i = 0; e->argNames && i < e->argCount; i++) anyNamed |= e->argNames[i] != NULL;
        if (anyNamed) {
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
        jsonString(&node, e->kind == E_UNARY ? (e->op == T_NOT ? "not" : e->op == T_TILDE ? "~" : "neg") : qkTokName((TokKind)e->op));
        if (e->kind == E_RANGE && (!e->a || !e->b)) { /* xs[2..], xs[..3]: the end that is left out */
            qkBufPuts(&node, e->a ? ", \"open\": \"end\"" : ", \"open\": \"start\"");
            pair[0] = e->a ? e->a : e->b;
            inputs(o, &node, pair, e->a || e->b ? 1 : 0, key);
            break;
        }
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
            if (e->params[i].type) {
                qkBufPuts(&node, ", \"type\": ");
                typeString(e->params[i].type, &node);
            }
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
            qkBufPuts(&node, i ? ", {" : "{");
            writePattern(o, a, &node);
            if (a->guard) {
                qkBufPuts(&node, ", \"guard\": ");
                writeInput(o, a->guard, &node, keyJoin(armKey, "", "/if"));
            }
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
    static const char* const kinds[E_KIND_COUNT] = {"value", "value", "value", "value", "name", "call", "field", "index", "op",
                                                    "op", "op", "if", "match", "lambda", "array", "block", "case", "with", "text",
                                                    "try", "map"};
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
        qkBufPrintf(b, "{\"case\": \"%s.%s\"}", e->decl->templateOf ? e->decl->templateOf->name : e->decl->name, e->name);
        return;
    case E_NAME:
        if (e->binding) {
            qkBufPrintf(b, "{\"node\": %d}", wireOf(e->binding));
        } else if (e->decl && e->decl->kind == D_FN) { /* a function given where a lambda goes */
            qkBufPuts(b, "{\"fn\": ");
            jsonString(b, e->name);
            qkBufPuts(b, "}");
        } else { /* a constant, None */
            qkBufPuts(b, "{\"name\": ");
            jsonString(b, e->name);
            qkBufPuts(b, "}");
        }
        return;
    default:
        qkBufPrintf(b, "{\"node\": %d}", writeNode(o, e, key));
    }
}

/* A statement's key: fn/name for the named ones (let, var, for), else its place: prefix/@index. */
static int namedIn(const Block* b, const char* name);

static int namedInExpr(const Expr* e, const char* name) {
    if (!e) return 0;
    int n = namedInExpr(e->a, name) + namedInExpr(e->b, name) + namedInExpr(e->body, name) + namedInExpr(e->elseExpr, name);
    for (int i = 0; i < e->argCount; i++) n += namedInExpr(e->args[i], name);
    for (int i = 0; i < e->armCount; i++) n += namedInExpr(e->arms[i].body, name);
    if (e->thenBlock) n += namedIn(e->thenBlock, name);
    if (e->block) n += namedIn(e->block, name);
    return n;
}

/* How many lets, vars and fors of a function name this */
static int namedIn(const Block* b, const char* name) {
    int n = 0;
    for (int i = 0; b && i < b->count; i++) {
        const Stmt* s = b->stmts[i];
        if ((s->kind == S_LET || s->kind == S_VAR || s->kind == S_FOR) && s->name && !strcmp(s->name, name)) n++;
        n += namedInExpr(s->value, name) + namedInExpr(s->iter, name) + namedInExpr(s->target, name);
        if (s->body) n += namedIn(s->body, name);
        if (s->elseBody) n += namedIn(s->elseBody, name);
    }
    return n;
}

/* A named statement is keyed fn/name; when the function names two (a for i in each of two loops), each is
 * keyed by its place too (main/@3/i). Other statements by their place. */
static const char* stmtKey(Out* o, const Stmt* s, const char* prefix, int i) {
    if ((s->kind == S_LET || s->kind == S_VAR || s->kind == S_FOR) && s->name) {
        bool once = !o->decl || !o->decl->body || o->decl->body->kind != E_BLOCK || namedIn(o->decl->body->block, s->name) <= 1;
        if (once) return keyJoin(o->fn, "/", s->name);
        return keyJoin(keyIndex(prefix, "/@", i), "/", s->name);
    }
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
        if (s->pattern) { /* let Some(x) = value else { ... } */
            kind = "let";
            qkBufPuts(&node, ", ");
            writePattern(o, s->pattern, &node);
            inputs(o, &node, &((Stmt*)s)->value, 1, in);
            if (s->elseBody) {
                qkBufPuts(&node, ", \"else\": ");
                writeBlock(o, s->elseBody, &node, keyJoin(key, "", "/else"));
            }
            break;
        }
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
        if (k || !d->impliedSelf) { /* fn Vec2.length(self): its type is the type's */
            qkBufPuts(out, ", \"type\": ");
            typeString(d->params[k].type, out);
        }
        qkBufPuts(out, ", \"key\": ");
        jsonString(out, keyJoin(d->name, "/", d->params[k].name));
        if (d->params[k].fallback && o) { /* its default: a value, or nodes of its own */
            qkBufPuts(out, ", \"default\": ");
            writeInput(o, d->params[k].fallback, out, keyJoin(d->name, "/", keyJoin(d->params[k].name, "", "=")));
        }
        qkBufPuts(out, "}");
    }
    qkBufPuts(out, "]");
    if (d->ret) {
        qkBufPuts(out, ", \"returns\": ");
        typeString(d->ret, out);
    }
    if (d->isEffect && !d->isTest) qkBufPuts(out, ", \"effect\": true");
    if (d->typeParamCount && !d->impliedTypes) {
        qkBufPuts(out, ", \"typeParams\": [");
        for (int k = 0; k < d->typeParamCount; k++) {
            if (k) qkBufPuts(out, ", ");
            jsonString(out, d->typeParams[k]);
        }
        qkBufPuts(out, "]");
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
        if (d->fromPrelude || d->templateOf || d->module || d->lifted) continue; /* the host's: in the catalog; another file's: in its own graph */
        qkBufPuts(out, written++ ? ",\n  {\"kind\": " : "  {\"kind\": ");
        const char* kind = d->kind == D_FN ? (d->isExtern ? "extern fn" : d->isTest ? "test" : "fn")
                           : d->kind == D_TYPE ? (d->isExtern ? "extern type" : "type") : d->kind == D_CONST ? "const"
                           : d->kind == D_USE ? "use" : "enum";
        jsonString(out, kind);
        qkBufPuts(out, ", \"name\": ");
        jsonString(out, d->name);
        if (d->isPub) qkBufPuts(out, ", \"pub\": true");
        if ((d->kind == D_TYPE || d->kind == D_ENUM) && d->typeParamCount) { /* type Pair<A, B> */
            qkBufPuts(out, ", \"typeParams\": [");
            for (int k = 0; k < d->typeParamCount; k++) {
                if (k) qkBufPuts(out, ", ");
                jsonString(out, d->typeParams[k]);
            }
            qkBufPuts(out, "]");
        }
        notes(out, d->leading, d->trailing, false);
        if (d->kind == D_USE) { /* another file: its declarations are its own graph's */
            if (d->alias) {
                qkBufPuts(out, ", \"alias\": ");
                jsonString(out, d->alias);
            }
            qkBufPuts(out, "}");
            continue;
        }
        if (d->kind == D_ENUM) {
            qkBufPuts(out, ", \"cases\": [");
            for (int k = 0; k < d->caseCount; k++) {
                if (k) qkBufPuts(out, ", ");
                int n = d->caseFieldCount ? d->caseFieldCount[k] : 0;
                if (!n) {
                    jsonString(out, d->cases[k]);
                    continue;
                }
                qkBufPuts(out, "{\"name\": "); /* a case with data: its fields */
                jsonString(out, d->cases[k]);
                qkBufPuts(out, ", \"fields\": [");
                for (int f = 0; f < n; f++) {
                    qkBufPuts(out, f ? ", {\"name\": " : "{\"name\": ");
                    jsonString(out, d->caseFields[k][f].name);
                    qkBufPuts(out, ", \"type\": ");
                    typeString(d->caseFields[k][f].type, out);
                    qkBufPuts(out, "}");
                }
                qkBufPuts(out, "]}");
            }
            qkBufPuts(out, "]}");
            continue;
        }
        if (d->kind == D_CONST) { /* a constant: its value's nodes */
            Out o = {{0}, 1, d->name, 0, layout, d};
            if (d->ret) {
                qkBufPuts(out, ", \"annotation\": ");
                typeString(d->ret, out);
            }
            QkBuf value = {0};
            qkBufPuts(&value, ", \"result\": ");
            writeInput(&o, d->body, &value, keyJoin(d->name, "", "/="));
            qkBufAdd(out, value.data, value.len);
            qkBufFree(&value);
            qkBufPuts(out, ",\n   \"nodes\": [\n");
            qkBufAdd(out, o.nodes.data ? o.nodes.data : "", o.nodes.len);
            qkBufPuts(out, "]}");
            qkBufFree(&o.nodes);
            continue;
        }
        if (d->kind == D_TYPE) {
            if (d->isInterface) { /* its functions, as they are written */
                qkBufPuts(out, ", \"interface\": true, \"functions\": [");
                for (int k = 0; k < d->paramCount; k++) {
                    QkBuf m = {0};
                    qkFormatMember(&d->params[k], &m);
                    if (k) qkBufPuts(out, ", ");
                    jsonString(out, m.data);
                    qkBufFree(&m);
                }
                qkBufPuts(out, "]");
            } else if (!d->isExtern) {
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
            Out o = {{0}, 1, d->name, 0, layout, d};
            writeParams(out, d, &o);
            if (o.nodeCount) { /* defaults with nodes */
                qkBufPuts(out, ",\n   \"nodes\": [\n");
                qkBufAdd(out, o.nodes.data, o.nodes.len);
                qkBufPuts(out, "]");
            }
            qkBufFree(&o.nodes);
            qkBufPuts(out, "}");
            continue;
        }
        Out o = {{0}, 1, d->name, 0, layout, d};
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
            Out o = {{0}, 1, d->name, 0, NULL, d};
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
    char* building;     /* nodes being built: one met again is wired into itself */
} In;

static void inFail(In* in, const char* what, int id) {
    if (!in->failed) qkError(in->src, 0, 0, "the graph is not valid: %s (node %d)", what, id);
    in->failed = true;
}

static TypeExpr* parseTypeText(const char* s) { return qkParseTypeText(s); }

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
                c = k == 'n' ? '\n' : k == 't' ? '\t' : k == 'r' ? '\r' : k;
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
    if (!fn) fn = jstr(input, "name"); /* a constant, None */
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

/* "pattern" and "binds" (see writePattern) back into an arm */
static void readPattern(In* in, const J* a, Arm* arm) {
    const char* pat = jstr(a, "pattern");
    if (pat && !strcmp(pat, "(,)")) arm->pattern = pat;
    else if (pat && strcmp(pat, "_")) {
        const char* dots = strstr(pat, "..");
        if (pat[0] == '"') {
            arm->textPattern = literal(pat, "Text");
        } else if (dots) { /* 80..90, 90.., ..60 */
            arm->isRange = true;
            if (dots > pat) arm->intPattern = literal(qkDup(pat, (size_t)(dots - pat)), "Int");
            if (dots[2]) arm->intHigh = literal(dots + 2, "Int");
        } else if (pat[0] == '-' || (pat[0] >= '0' && pat[0] <= '9')) {
            arm->intPattern = literal(pat, "Int");
        } else {
            arm->pattern = pat;
        }
    }
    J* binds = jget(a, "binds");
    if (binds && binds->count) { /* Circle(r): r names a node the arm's wires come from */
        arm->bindCount = binds->count;
        arm->binds = qkNew(sizeof(char*) * (size_t)binds->count);
        for (int f = 0; f < binds->count; f++) {
            if (jget(binds->items[f], "pattern")) { /* a pattern inside */
                if (!arm->subs) arm->subs = qkNew(sizeof(Arm*) * (size_t)binds->count);
                arm->subs[f] = qkNew(sizeof(Arm));
                readPattern(in, binds->items[f], arm->subs[f]);
                arm->binds[f] = "_";
                continue;
            }
            arm->binds[f] = jstr(binds->items[f], "name") ? jstr(binds->items[f], "name") : "_";
            int bid = jint(binds->items[f], "id", -1);
            if (bid >= 0 && bid <= in->maxId) in->names[bid] = arm->binds[f];
        }
    }
}

static Expr* buildNodeInner(In* in, int id);

static Expr* buildNode(In* in, int id) {
    if (id >= 0 && id <= in->maxId && in->building) {
        if (in->building[id]) {
            inFail(in, "a node wired into itself (its value uses it)", id);
            return zero();
        }
        in->building[id] = 1;
        Expr* e = buildNodeInner(in, id);
        in->building[id] = 0;
        return e;
    }
    return buildNodeInner(in, id);
}

static Expr* buildNodeInner(In* in, int id) {
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
            if (jstr(ps->items[i], "type")) e->params[i].type = parseTypeText(jstr(ps->items[i], "type"));
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
        e->qualifier = jstr(node, "qualifier");
        return e;
    }
    if (!strcmp(kind, "with")) { /* inputs: the base, then the new values, named */
        Expr* e = qkNewExpr(E_WITH, 0, 0);
        e->a = n > 0 ? args[0] : zero();
        e->args = args + (n > 0 ? 1 : 0);
        e->argCount = e->argCap = n > 0 ? n - 1 : 0;
        e->argNames = qkNew(sizeof(char*) * (size_t)(n ? n : 1));
        J* names = jget(node, "names");
        for (int i = 0; i < e->argCount; i++)
            e->argNames[i] = names && i < names->count && names->items[i]->kind == J_STR ? names->items[i]->str : "x";
        return e;
    }
    if (!strcmp(kind, "text")) { /* "lap {n}": the pieces as written, a value between each two */
        Expr* e = qkNewExpr(E_INTERP, 0, 0);
        J* parts = jget(node, "parts");
        e->args = args;
        e->argCount = e->argCap = n;
        e->argNames = qkNew(sizeof(char*) * (size_t)(n ? n : 1));
        e->parts = qkNew(sizeof(char*) * (size_t)(n + 1));
        e->rawParts = qkNew(sizeof(char*) * (size_t)(n + 1));
        J* formats = jget(node, "formats");
        if (formats && formats->kind == J_ARR) {
            e->formats = qkNew(sizeof(char*) * (size_t)(n + 1));
            for (int i = 0; i < n && i < formats->count; i++)
                if (formats->items[i]->kind == J_STR && formats->items[i]->str[0]) e->formats[i] = formats->items[i]->str;
        }
        for (int i = 0; i <= n; i++) {
            const char* raw = parts && i < parts->count && parts->items[i]->kind == J_STR ? parts->items[i]->str : "";
            e->rawParts[i] = raw;
            Expr* lit = literal(qkDup("\"", 1), "Text");
            QkBuf quoted = {0};
            qkBufPrintf(&quoted, "\"%s\"", raw);
            lit = literal(qkDup(quoted.data, quoted.len), "Text");
            qkBufFree(&quoted);
            e->parts[i] = lit->text;
        }
        return e;
    }
    if (!strcmp(kind, "try")) {
        Expr* e = qkNewExpr(E_TRY, 0, 0);
        e->a = n > 0 ? args[0] : zero();
        return e;
    }
    if (!strcmp(kind, "map")) {
        Expr* e = qkNewExpr(E_MAP, 0, 0);
        e->args = args;
        e->argCount = e->argCap = n;
        e->argNames = qkNew(sizeof(char*) * (size_t)(n ? n : 1));
        return e;
    }
    if (!strcmp(kind, "op")) {
        const char* op = jstr(node, "op");
        if (!op) op = "+";
        const char* open = jstr(node, "open");
        if (open && !strcmp(op, "..")) { /* a range without its start or its end */
            Expr* e = qkNewExpr(E_RANGE, 0, 0);
            e->op = T_DOTDOT;
            if (!strcmp(open, "end")) e->a = n > 0 ? args[0] : zero();
            else e->b = n > 0 ? args[0] : zero();
            return e;
        }
        if (!strcmp(op, "neg") || !strcmp(op, "not") || !strcmp(op, "~")) {
            Expr* e = qkNewExpr(E_UNARY, 0, 0);
            e->op = !strcmp(op, "neg") ? T_MINUS : !strcmp(op, "~") ? T_TILDE : T_NOT;
            e->a = n > 0 ? args[0] : zero();
            return e;
        }
        Expr* e = qkNewExpr(!strcmp(op, "..") ? E_RANGE : E_BINARY, 0, 0);
        e->op = -1;
        for (int k = T_AND; k <= T_SHR; k++)
            if (k != T_TILDE && !strcmp(qkTokName((TokKind)k), op)) e->op = k;
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
            readPattern(in, a, &arm);
            arm.comment = jstr(a, "comment");
            if (jget(a, "guard")) arm.guard = buildInput(in, jget(a, "guard"));
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
    if (!strcmp(kind, "let") && jstr(node, "pattern")) { /* let Some(x) = value else { ... } */
        s->kind = S_LET;
        s->pattern = qkNew(sizeof(Arm));
        readPattern(in, node, s->pattern);
        s->value = buildInput(in, first);
        if (jget(node, "else")) s->elseBody = buildBlock(in, jget(node, "else"));
    } else if (!strcmp(kind, "let") || !strcmp(kind, "var")) {
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

/* the highest id among a pattern's binds, the patterns inside them too */
static int maxBindId(const J* binds) {
    int maxId = 0;
    for (int f = 0; binds && f < binds->count; f++) {
        if (jint(binds->items[f], "id", 0) > maxId) maxId = jint(binds->items[f], "id", 0);
        int inner = maxBindId(jget(binds->items[f], "binds"));
        if (inner > maxId) maxId = inner;
    }
    return maxId;
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
        int own = maxBindId(jget(nd, "binds")); /* let Some(x) = ... else, inner patterns too */
        if (own > maxId) maxId = own;
        J* ps = jget(nd, "params");
        for (int q = 0; ps && q < ps->count; q++)
            if (jint(ps->items[q], "id", 0) > maxId) maxId = jint(ps->items[q], "id", 0);
        J* arms = jget(nd, "arms");
        for (int q = 0; arms && q < arms->count; q++) {
            int b = maxBindId(jget(arms->items[q], "binds"));
            if (b > maxId) maxId = b;
        }
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
        J* pub = jget(dj, "pub");
        d->isPub = pub && pub->kind == J_BOOL && pub->b;
        J* tps = jget(dj, "typeParams");
        if (tps && tps->kind == J_ARR && tps->count) {
            d->typeParams = qkNew(sizeof(char*) * (size_t)tps->count);
            for (int k = 0; k < tps->count; k++) d->typeParams[k] = tps->items[k]->kind == J_STR ? tps->items[k]->str : "T";
            d->typeParamCount = tps->count;
        }
        if (!strcmp(kind, "use")) {
            d->kind = D_USE;
            d->alias = jstr(dj, "alias");
        } else if (!strcmp(kind, "enum")) {
            d->kind = D_ENUM;
            J* cases = jget(dj, "cases");
            d->caseFields = qkNew(sizeof(Param*) * 64);
            d->caseFieldCount = qkNew(sizeof(int) * 64);
            for (int k = 0; cases && k < cases->count && k < 64; k++) {
                J* cj = cases->items[k];
                if (cj->kind == J_STR) {
                    QK_PUSH(d->cases, d->caseCount, d->caseCap, cj->str);
                    continue;
                }
                const char* name = jstr(cj, "name"); /* a case with data */
                int at = d->caseCount;
                QK_PUSH(d->cases, d->caseCount, d->caseCap, name ? name : "Case");
                J* fields = jget(cj, "fields");
                int n = fields ? fields->count : 0;
                d->caseFields[at] = qkNew(sizeof(Param) * (size_t)(n ? n : 1));
                d->caseFieldCount[at] = n;
                for (int f = 0; f < n; f++)
                    d->caseFields[at][f] = (Param){jstr(fields->items[f], "name"), parseTypeText(jstr(fields->items[f], "type")), 0, 0, NULL, NULL};
            }
        } else if (!strcmp(kind, "extern type")) {
            d->kind = D_TYPE;
            d->isExtern = true;
        } else if (!strcmp(kind, "type")) {
            d->kind = D_TYPE;
            J* functions = jget(dj, "functions");
            for (int k = 0; functions && k < functions->count; k++) { /* an interface */
                Param f = qkParseMemberText(functions->items[k]->kind == J_STR ? functions->items[k]->str : NULL);
                QK_PUSH(d->params, d->paramCount, d->paramCap, f);
                d->isInterface = true;
            }
            J* fields = jget(dj, "fields");
            for (int k = 0; fields && k < fields->count; k++) {
                Param f = {jstr(fields->items[k], "name"), parseTypeText(jstr(fields->items[k], "type")), 0, 0, NULL};
                QK_PUSH(d->params, d->paramCount, d->paramCap, f);
            }
        } else { /* fn, extern fn, let: nodes of their own */
            bool isConst = !strcmp(kind, "const");
            d->kind = isConst ? D_CONST : D_FN;
            d->isExtern = !strcmp(kind, "extern fn");
            J* effect = jget(dj, "effect");
            d->isEffect = effect && effect->kind == J_BOOL && effect->b;
            if (!strcmp(kind, "test")) { /* test "name": its name as text */
                d->isTest = d->isEffect = true;
                Expr* title = literal(strlen(d->name) > 5 ? d->name + 5 : "\"\"", "Text");
                d->testName = title->text;
            }
            In in = {src, NULL, NULL, maxIdOf(dj), false};
            in.byId = qkNew(sizeof(J*) * (size_t)(in.maxId + 1));
            in.names = qkNew(sizeof(char*) * (size_t)(in.maxId + 1));
            in.building = qkNew((size_t)in.maxId + 1);
            J* nodes = jget(dj, "nodes");
            for (int k = 0; nodes && k < nodes->count; k++) {
                int nid = jint(nodes->items[k], "id", -1);
                if (nid >= 0 && nid <= in.maxId) in.byId[nid] = nodes->items[k];
            }
            J* params = jget(dj, "params");
            for (int k = 0; params && k < params->count; k++) {
                J* pj = params->items[k];
                Param prm = {jstr(pj, "name"), jstr(pj, "type") ? parseTypeText(jstr(pj, "type")) : NULL, 0, 0, NULL};
                int pid = jint(pj, "id", -1);
                if (pid >= 0 && pid <= in.maxId) in.names[pid] = prm.name;
                if (jget(pj, "default")) prm.fallback = buildInput(&in, jget(pj, "default"));
                QK_PUSH(d->params, d->paramCount, d->paramCap, prm);
            }
            if (jstr(dj, "returns")) d->ret = parseTypeText(jstr(dj, "returns"));
            if (jstr(dj, "annotation")) d->ret = parseTypeText(jstr(dj, "annotation"));
            const char* form = jstr(dj, "form");
            if (d->isExtern) {
                /* the host's: no body */
            } else if (isConst || (form && !strcmp(form, "expr"))) {
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
