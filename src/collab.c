/* Quark — graphs side by side: what changed between two versions of a program, and three-way merges, by node key.
 *
 * A graph is read into its declarations, each a head (the declaration without its nodes) and its nodes by key. The
 * numbers that wire nodes together are only a file's own ({"node": 12}), so they are replaced by keys first: every
 * "id", every wire ("node", "elseIf") and every block's "stmts" list then names keys, and two versions compare
 * node by node. Parameters, lambda parameters and a pattern's names keep their keys too (owner#name).
 *
 * Keys of named statements (let, var, for: main/total) stay while their name does; other statements are keyed by
 * their place (main/@3), which an inserted statement shifts. So before comparing, the second version's statements
 * are lined up with the first's, block by block: the same named ones, then the same statements (their content, by
 * names rather than keys: a longest common subsequence), then statements of the same kind left between those. The
 * second version takes the first's keys for them (and their nodes inside), and a new statement whose place-key the
 * first already uses gets a fresh one (main/@0+). A diff then reports nodes added, removed and changed (which
 * inputs or values) and statements moved; a merge takes each node from the side that changed it, merges every
 * block's statement order, and reports a conflict, by key, where both sides changed the same thing differently
 * (the result keeps ours there, so it is always a whole program). */
#include "qk.h"

#include <stdio.h>
#include <stdlib.h>

/* ---- keys to numbers ----------------------------------------------------------------------------- */

typedef struct Map {
    const char** keys;
    int* vals;
    int cap, count;
} Map;

static unsigned hashKey(const char* s) {
    unsigned h = 2166136261u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

static void mapPut(Map* m, const char* key, int val);

static void mapGrow(Map* m) {
    Map old = *m;
    m->cap = old.cap ? old.cap * 2 : 64;
    m->keys = qkNew(sizeof(char*) * (size_t)m->cap);
    m->vals = qkNew(sizeof(int) * (size_t)m->cap);
    m->count = 0;
    for (int i = 0; i < old.cap; i++)
        if (old.keys[i]) mapPut(m, old.keys[i], old.vals[i]);
}

static void mapPut(Map* m, const char* key, int val) {
    if ((m->count + 1) * 2 > m->cap) mapGrow(m);
    unsigned i = hashKey(key) & (unsigned)(m->cap - 1);
    while (m->keys[i] && strcmp(m->keys[i], key)) i = (i + 1) & (unsigned)(m->cap - 1);
    if (!m->keys[i]) {
        m->keys[i] = key;
        m->count++;
    }
    m->vals[i] = val;
}

static int mapGet(const Map* m, const char* key) {
    if (!m->cap || !key) return -1;
    unsigned i = hashKey(key) & (unsigned)(m->cap - 1);
    while (m->keys[i]) {
        if (!strcmp(m->keys[i], key)) return m->vals[i];
        i = (i + 1) & (unsigned)(m->cap - 1);
    }
    return -1;
}

/* ---- JSON values --------------------------------------------------------------------------------- */

static J* jnew(JKind kind) {
    J* v = qkNew(sizeof *v);
    v->kind = kind;
    return v;
}

static J* jtext(const char* s) {
    J* v = jnew(J_STR);
    v->str = s;
    return v;
}

static const char* jstrOf(const J* o, const char* key) {
    J* v = qkJsonGet(o, key);
    return v && v->kind == J_STR ? v->str : NULL;
}

static void jpush(J* v, const char* key, J* item) {
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
}

static void jdel(J* o, const char* key) {
    int n = 0;
    for (int i = 0; i < o->count; i++) {
        if (!strcmp(o->keys[i], key)) continue;
        o->keys[n] = o->keys[i];
        o->items[n++] = o->items[i];
    }
    o->count = n;
}

static J* jcopy(const J* v) {
    J* c = jnew(v->kind);
    c->num = v->num;
    c->b = v->b;
    c->str = v->str;
    for (int i = 0; i < v->count; i++) jpush(c, v->keys ? v->keys[i] : NULL, jcopy(v->items[i]));
    return c;
}

enum { W_SORTED = 1, W_NO_STMTS = 2 };

static int byKeyName(const void* a, const void* b) { return strcmp(**(const char* const* const*)a, **(const char* const* const*)b); }

/* JSON text: with sorted fields, to compare (an editor may write fields in its own order); without the blocks'
 * statement lists, to compare a node apart from what its blocks hold (they merge on their own). */
static void jwrite(const J* v, QkBuf* b, int flags) {
    if (!v) {
        qkBufPuts(b, "null");
        return;
    }
    switch (v->kind) {
    case J_NULL: qkBufPuts(b, "null"); break;
    case J_BOOL: qkBufPuts(b, v->b ? "true" : "false"); break;
    case J_NUM: qkBufPrintf(b, "%.15g", v->num); break;
    case J_STR: qkJsonString(b, v->str); break;
    case J_ARR:
        qkBufByte(b, '[');
        for (int i = 0; i < v->count; i++) {
            if (i) qkBufPuts(b, ", ");
            jwrite(v->items[i], b, flags);
        }
        qkBufByte(b, ']');
        break;
    case J_OBJ: {
        const char*** order = malloc(sizeof(char**) * (size_t)(v->count ? v->count : 1));
        for (int i = 0; i < v->count; i++) order[i] = &v->keys[i];
        if (flags & W_SORTED) qsort(order, (size_t)v->count, sizeof *order, byKeyName);
        qkBufByte(b, '{');
        bool first = true;
        for (int k = 0; k < v->count; k++) {
            int i = (int)(order[k] - v->keys);
            if ((flags & W_NO_STMTS) && !strcmp(v->keys[i], "stmts")) continue;
            if (!first) qkBufPuts(b, ", ");
            first = false;
            qkJsonString(b, v->keys[i]);
            qkBufPuts(b, ": ");
            jwrite(v->items[i], b, flags);
        }
        qkBufByte(b, '}');
        free(order);
        break;
    }
    }
}

static const char* jtextOf(const J* v, int flags) {
    if (!v) return NULL;
    QkBuf b = {0};
    jwrite(v, &b, flags);
    const char* s = qkDup(b.data, b.len);
    qkBufFree(&b);
    return s;
}

static bool same(const char* a, const char* b) { return a == b || (a && b && !strcmp(a, b)); }

/* ---- a graph, by keys ---------------------------------------------------------------------------- */

typedef struct GNode {
    const char* key; /* lined up with the other version's keys */
    const char* own; /* as its own graph keys it */
    J* obj;          /* ids, wires and statement lists hold keys; no line, pos or type (derived) */
    int line;
} GNode;

typedef struct GDecl {
    const char* name;
    J* head; /* the declaration without its nodes */
    GNode* nodes;
    int count, cap;
    Map byKey;            /* node key -> index */
    Map ents;             /* a parameter's or a pattern's name: key -> index in entNames */
    const char** entNames;
    int entCount, entCap;
    Map parent;           /* key -> the node it is part of (an input, a statement of its block); -2: the head */
} GDecl;

typedef struct Graph {
    GDecl** decls;
    int count, cap;
    Map byName;
    const char* closing;
} Graph;

static GNode* nodeAt(const GDecl* d, const char* key) {
    int i = mapGet(&d->byKey, key);
    return i >= 0 ? &d->nodes[i] : NULL;
}

static J* objAt(const GDecl* d, const char* owner) {
    if (!owner) return d->head;
    GNode* n = nodeAt(d, owner);
    return n ? n->obj : NULL;
}

static bool isBinding(const J* obj) {
    const char* kind = jstrOf(obj, "kind");
    return kind && (!strcmp(kind, "var") || !strcmp(kind, "for") || (!strcmp(kind, "let") && jstrOf(obj, "name")));
}

/* the nodes a value wires from: inputs ("node", "elseIf") and, with stmts, its blocks' statements */
typedef struct Ref {
    const char* key;
    bool stmt; /* a statement of a block, not an input */
} Ref;

typedef struct Refs {
    Ref* items;
    int count, cap;
} Refs;

static void refsOf(const J* v, bool withStmts, Refs* r) {
    if (!v) return;
    for (int i = 0; i < v->count; i++) {
        const char* f = v->kind == J_OBJ ? v->keys[i] : NULL;
        J* item = v->items[i];
        if (f && (!strcmp(f, "node") || !strcmp(f, "elseIf")) && item->kind == J_STR) {
            QK_PUSH(r->items, r->count, r->cap, ((Ref){item->str, false}));
        } else if (f && !strcmp(f, "stmts") && item->kind == J_ARR) {
            for (int k = 0; withStmts && k < item->count; k++)
                if (item->items[k]->kind == J_STR) QK_PUSH(r->items, r->count, r->cap, ((Ref){item->items[k]->str, true}));
        } else {
            refsOf(item, withStmts, r);
        }
    }
}

/* the parameters and pattern names inside a node (not the node itself): key -> name */
static void entsOf(GDecl* d, const J* v, bool top) {
    if (v->kind == J_OBJ && !top) {
        const char* id = jstrOf(v, "id");
        if (id) {
            const char* name = jstrOf(v, "name");
            mapPut(&d->ents, id, d->entCount);
            QK_PUSH(d->entNames, d->entCount, d->entCap, name ? name : "_");
        }
    }
    for (int i = 0; i < v->count; i++)
        if (v->items[i]->kind == J_OBJ || v->items[i]->kind == J_ARR) entsOf(d, v->items[i], false);
}

static void indexDecl(GDecl* d) {
    memset(&d->byKey, 0, sizeof d->byKey);
    memset(&d->ents, 0, sizeof d->ents);
    memset(&d->parent, 0, sizeof d->parent);
    d->entCount = 0;
    for (int i = 0; i < d->count; i++) {
        d->nodes[i].key = jstrOf(d->nodes[i].obj, "id");
        if (!d->nodes[i].key) d->nodes[i].key = "?";
        mapPut(&d->byKey, d->nodes[i].key, i);
    }
    entsOf(d, d->head, true);
    for (int i = 0; i < d->count; i++) entsOf(d, d->nodes[i].obj, true);
    for (int i = -1; i < d->count; i++) {
        Refs r = {0};
        refsOf(i < 0 ? d->head : d->nodes[i].obj, true, &r);
        for (int k = 0; k < r.count; k++) {
            GNode* t = nodeAt(d, r.items[k].key);
            if (t && (r.items[k].stmt || !isBinding(t->obj))) mapPut(&d->parent, r.items[k].key, i < 0 ? -2 : i);
        }
    }
}

/* the blocks a value holds ("body", "then", "arms.2.block"...): any object with a "stmts" list */
typedef struct BlockAt {
    const char* path;
    J* block;
} BlockAt;

typedef struct Blocks {
    BlockAt* items;
    int count, cap;
} Blocks;

static void blocksOf(J* v, const char* path, Blocks* out) {
    for (int i = 0; i < v->count; i++) {
        J* item = v->items[i];
        if (item->kind != J_OBJ && item->kind != J_ARR) continue;
        const char* sub;
        QkBuf p = {0};
        if (v->kind == J_OBJ) qkBufPrintf(&p, "%s%s%s", path, *path ? "." : "", v->keys[i]);
        else qkBufPrintf(&p, "%s%s%d", path, *path ? "." : "", i);
        sub = qkDup(p.data, p.len);
        qkBufFree(&p);
        J* stmts = item->kind == J_OBJ ? qkJsonGet(item, "stmts") : NULL;
        if (stmts && stmts->kind == J_ARR) {
            QK_PUSH(out->items, out->count, out->cap, ((BlockAt){sub, item}));
        } else if (v->kind != J_OBJ || (strcmp(v->keys[i], "inputs") && strcmp(v->keys[i], "params"))) {
            blocksOf(item, sub, out);
        }
    }
}

static J* blockAt(J* obj, const char* path) {
    if (!obj) return NULL;
    Blocks bs = {0};
    blocksOf(obj, "", &bs);
    for (int i = 0; i < bs.count; i++)
        if (!strcmp(bs.items[i].path, path)) return qkJsonGet(bs.items[i].block, "stmts");
    return NULL;
}

/* A statement list's keys. */
static const char** listOf(J* stmts, int* n) {
    *n = 0;
    if (!stmts) return NULL;
    const char** keys = qkNew(sizeof(char*) * (size_t)(stmts->count ? stmts->count : 1));
    for (int i = 0; i < stmts->count; i++)
        if (stmts->items[i]->kind == J_STR) keys[(*n)++] = stmts->items[i]->str;
    return keys;
}

static void setList(J* stmts, const char** keys, int n) {
    stmts->count = 0;
    for (int i = 0; i < n; i++) jpush(stmts, NULL, jtext(keys[i]));
}

/* ---- reading a graph ----------------------------------------------------------------------------- */

static int maxIdIn(const J* v) {
    int m = 0;
    for (int i = 0; i < v->count; i++) {
        const J* item = v->items[i];
        const char* f = v->kind == J_OBJ ? v->keys[i] : NULL;
        if (f && !strcmp(f, "id") && item->kind == J_NUM && item->num > m) m = (int)item->num;
        if (item->kind == J_OBJ || item->kind == J_ARR) {
            int k = maxIdIn(item);
            if (k > m) m = k;
        }
    }
    return m;
}

static const char* idKey(const char** keys, int maxId, double num) {
    int id = (int)num;
    if (id >= 0 && id <= maxId && keys[id]) return keys[id];
    char s[32];
    snprintf(s, sizeof s, "?%d", id);
    return qkDup(s, strlen(s));
}

/* the parameters and names inside a node take keys of their own: owner#name (#name#2 for a second one) */
static void nameEnts(const J* v, const char* owner, bool top, const char** keys, int maxId, Map* used) {
    if (v->kind == J_OBJ && !top) {
        J* id = qkJsonGet(v, "id");
        if (id && id->kind == J_NUM && (int)id->num >= 0 && (int)id->num <= maxId) {
            const char* name = jstrOf(v, "name");
            QkBuf k = {0};
            qkBufPrintf(&k, "%s#%s", owner, name ? name : "_");
            for (int n = 2; mapGet(used, k.data) >= 0; n++) {
                k.len = 0;
                qkBufPrintf(&k, "%s#%s#%d", owner, name ? name : "_", n);
            }
            const char* key = qkDup(k.data, k.len);
            qkBufFree(&k);
            mapPut(used, key, 1);
            keys[(int)id->num] = key;
        }
    }
    for (int i = 0; i < v->count; i++)
        if (v->items[i]->kind == J_OBJ || v->items[i]->kind == J_ARR) nameEnts(v->items[i], owner, false, keys, maxId, used);
}

static void keysForIds(J* v, const char** keys, int maxId) {
    for (int i = 0; i < v->count; i++) {
        J* item = v->items[i];
        const char* f = v->kind == J_OBJ ? v->keys[i] : NULL;
        if (f && (!strcmp(f, "id") || !strcmp(f, "node") || !strcmp(f, "elseIf")) && item->kind == J_NUM) {
            item->str = idKey(keys, maxId, item->num);
            item->kind = J_STR;
        } else if (f && !strcmp(f, "stmts") && item->kind == J_ARR) {
            for (int k = 0; k < item->count; k++)
                if (item->items[k]->kind == J_NUM) {
                    item->items[k]->str = idKey(keys, maxId, item->items[k]->num);
                    item->items[k]->kind = J_STR;
                }
        } else if (item->kind == J_OBJ || item->kind == J_ARR) {
            keysForIds(item, keys, maxId);
        }
    }
}

static GDecl* readDecl(J* dj, const char* name) {
    GDecl* d = qkNew(sizeof *d);
    d->name = name;
    J* nodes = qkJsonGet(dj, "nodes");
    jdel(dj, "nodes");
    d->head = dj;
    int maxId = maxIdIn(dj);
    if (nodes && nodes->kind == J_ARR) {
        int m = maxIdIn(nodes);
        if (m > maxId) maxId = m;
    }
    const char** keys = qkNew(sizeof(char*) * (size_t)(maxId + 1));
    Map used = {0};
    J* params = qkJsonGet(dj, "params");
    for (int k = 0; params && params->kind == J_ARR && k < params->count; k++) {
        J* p = params->items[k];
        J* id = qkJsonGet(p, "id");
        if (!id || id->kind != J_NUM || (int)id->num < 0 || (int)id->num > maxId) continue;
        const char* key = jstrOf(p, "key");
        if (!key) {
            QkBuf b = {0};
            qkBufPrintf(&b, "%s/%s", name, jstrOf(p, "name") ? jstrOf(p, "name") : "_");
            key = qkDup(b.data, b.len);
            qkBufFree(&b);
        }
        keys[(int)id->num] = key;
        mapPut(&used, key, 1);
        jdel(p, "key"); /* the key is its id now */
    }
    for (int k = 0; nodes && nodes->kind == J_ARR && k < nodes->count; k++) {
        J* n = nodes->items[k];
        J* id = qkJsonGet(n, "id");
        const char* key = jstrOf(n, "key");
        if (!key || !id || id->kind != J_NUM || (int)id->num < 0 || (int)id->num > maxId) continue;
        keys[(int)id->num] = key;
        mapPut(&used, key, 1);
    }
    for (int k = 0; nodes && nodes->kind == J_ARR && k < nodes->count; k++) {
        J* n = nodes->items[k];
        const char* key = jstrOf(n, "key");
        if (key) nameEnts(n, key, true, keys, maxId, &used);
    }
    keysForIds(dj, keys, maxId);
    for (int k = 0; nodes && nodes->kind == J_ARR && k < nodes->count; k++) {
        J* n = nodes->items[k];
        if (n->kind != J_OBJ) continue;
        keysForIds(n, keys, maxId);
        J* line = qkJsonGet(n, "line");
        GNode g = {NULL, jstrOf(n, "key"), n, line && line->kind == J_NUM ? (int)line->num : 0};
        jdel(n, "key");
        jdel(n, "line");
        jdel(n, "pos");
        jdel(n, "type");
        if (!jstrOf(n, "id")) continue;
        QK_PUSH(d->nodes, d->count, d->cap, g);
    }
    indexDecl(d);
    for (int i = 0; i < d->count; i++)
        if (!d->nodes[i].own) d->nodes[i].own = d->nodes[i].key;
    return d;
}

static bool readGraph(const char* json, size_t len, const char* what, Graph* g) {
    memset(g, 0, sizeof *g);
    J* root = qkJsonParse(json, len);
    J* decls = root ? qkJsonGet(root, "decls") : NULL;
    if (!decls || decls->kind != J_ARR) {
        qkError(NULL, 0, 0, "%s is not a Quark graph (JSON with \"decls\")", what);
        return false;
    }
    for (int i = 0; i < decls->count; i++) {
        J* dj = decls->items[i];
        const char* name = jstrOf(dj, "name");
        if (dj->kind != J_OBJ || !name) {
            qkError(NULL, 0, 0, "%s: declaration %d has no name", what, i + 1);
            return false;
        }
        if (mapGet(&g->byName, name) >= 0) { /* the same name twice: the second is name#2 */
            QkBuf b = {0};
            for (int n = 2; !b.len || mapGet(&g->byName, b.data) >= 0; n++) {
                b.len = 0;
                qkBufPrintf(&b, "%s#%d", name, n);
            }
            name = qkDup(b.data, b.len);
            qkBufFree(&b);
        }
        GDecl* d = readDecl(dj, name);
        mapPut(&g->byName, name, g->count);
        QK_PUSH(g->decls, g->count, g->cap, d);
    }
    g->closing = jstrOf(root, "closing");
    return true;
}

static GDecl* declNamed(const Graph* g, const char* name) {
    int i = mapGet(&g->byName, name);
    return i >= 0 ? g->decls[i] : NULL;
}

/* ---- content without keys: what lines statements up ---------------------------------------------- */

static const char* nameOf(const GDecl* d, const char* key) {
    GNode* n = nodeAt(d, key);
    if (n) return jstrOf(n->obj, "name") ? jstrOf(n->obj, "name") : "?";
    int e = mapGet(&d->ents, key);
    return e >= 0 ? d->entNames[e] : "?";
}

static void content(const GDecl* d, const J* v, QkBuf* b, int depth);

static void contentOf(const GDecl* d, const char* key, QkBuf* b, int depth) {
    GNode* n = nodeAt(d, key);
    if (!n || depth > 200) {
        qkBufPuts(b, "?");
        return;
    }
    content(d, n->obj, b, depth + 1);
}

static void content(const GDecl* d, const J* v, QkBuf* b, int depth) {
    if (v->kind != J_OBJ && v->kind != J_ARR) {
        jwrite(v, b, 0);
        return;
    }
    if (v->kind == J_ARR) {
        qkBufByte(b, '[');
        for (int i = 0; i < v->count; i++) {
            if (i) qkBufByte(b, ',');
            content(d, v->items[i], b, depth);
        }
        qkBufByte(b, ']');
        return;
    }
    const char*** order = malloc(sizeof(char**) * (size_t)(v->count ? v->count : 1));
    for (int i = 0; i < v->count; i++) order[i] = &v->keys[i];
    qsort(order, (size_t)v->count, sizeof *order, byKeyName);
    qkBufByte(b, '{');
    for (int k = 0; k < v->count; k++) {
        int i = (int)(order[k] - v->keys);
        const char* f = v->keys[i];
        const J* item = v->items[i];
        if (!strcmp(f, "id")) continue;
        qkBufPrintf(b, "%s:", f);
        if ((!strcmp(f, "node") || !strcmp(f, "elseIf")) && item->kind == J_STR) {
            GNode* t = nodeAt(d, item->str);
            if (t && !isBinding(t->obj)) contentOf(d, item->str, b, depth);
            else qkBufPrintf(b, "@%s", nameOf(d, item->str)); /* a use: by the name it uses */
        } else if (!strcmp(f, "stmts") && item->kind == J_ARR) {
            qkBufByte(b, '[');
            for (int s = 0; s < item->count; s++) {
                if (s) qkBufByte(b, ',');
                if (item->items[s]->kind == J_STR) contentOf(d, item->items[s]->str, b, depth);
            }
            qkBufByte(b, ']');
        } else {
            content(d, item, b, depth);
        }
        qkBufByte(b, ';');
    }
    qkBufByte(b, '}');
    free(order);
}

static const char* contentText(const GDecl* d, const char* key) {
    QkBuf b = {0};
    contentOf(d, key, &b, 0);
    const char* s = qkDup(b.data, b.len);
    qkBufFree(&b);
    return s;
}

/* ---- lining up a version's keys with another's --------------------------------------------------- */

/* The longest common subsequence of a and b (by text): match[i] is the b index a[i] pairs with, or -1. */
static void commonOrder(const char** a, int n, const char** b, int m, int* match) {
    int* t = calloc((size_t)(n + 1) * (size_t)(m + 1), sizeof(int));
    for (int i = n - 1; i >= 0; i--)
        for (int j = m - 1; j >= 0; j--)
            t[i * (m + 1) + j] = same(a[i], b[j]) ? t[(i + 1) * (m + 1) + j + 1] + 1
                               : t[(i + 1) * (m + 1) + j] > t[i * (m + 1) + j + 1] ? t[(i + 1) * (m + 1) + j] : t[i * (m + 1) + j + 1];
    for (int i = 0; i < n; i++) match[i] = -1;
    int i = 0, j = 0;
    while (i < n && j < m) {
        if (same(a[i], b[j])) {
            match[i++] = j++;
        } else if (t[(i + 1) * (m + 1) + j] >= t[i * (m + 1) + j + 1]) {
            i++;
        } else {
            j++;
        }
    }
    free(t);
}

typedef struct Rename {
    const char* from;
    const char* to;
} Rename;

typedef struct Renames {
    Rename* items;
    int count, cap;
} Renames;

static void addRename(Renames* r, const char* from, const char* to) { QK_PUSH(r->items, r->count, r->cap, ((Rename){from, to})); }

/* a key under a renamed one (main/@4 -> main/@3: main/@4.0, main/@4/@1, main/@4.0#x) */
static const char* renamed(const Renames* r, const char* s) {
    int best = -1;
    size_t bestLen = 0;
    for (int i = 0; i < r->count; i++) {
        size_t n = strlen(r->items[i].from);
        if (n >= bestLen && !strncmp(s, r->items[i].from, n) && (!s[n] || s[n] == '.' || s[n] == '/' || s[n] == '#')) {
            best = i;
            bestLen = n;
        }
    }
    if (best < 0) return s;
    QkBuf b = {0};
    qkBufPrintf(&b, "%s%s", r->items[best].to, s + bestLen);
    const char* t = qkDup(b.data, b.len);
    qkBufFree(&b);
    return t;
}

static void renameIn(J* v, const Renames* r) {
    for (int i = 0; i < v->count; i++) {
        J* item = v->items[i];
        const char* f = v->kind == J_OBJ ? v->keys[i] : NULL;
        if (f && (!strcmp(f, "id") || !strcmp(f, "node") || !strcmp(f, "elseIf")) && item->kind == J_STR) {
            item->str = renamed(r, item->str);
        } else if (f && !strcmp(f, "stmts") && item->kind == J_ARR) {
            for (int k = 0; k < item->count; k++)
                if (item->items[k]->kind == J_STR) item->items[k]->str = renamed(r, item->items[k]->str);
        } else if (item->kind == J_OBJ || item->kind == J_ARR) {
            renameIn(item, r);
        }
    }
}

static void applyRenames(GDecl* d, const Renames* r) {
    if (!r->count) return;
    renameIn(d->head, r);
    for (int i = 0; i < d->count; i++) renameIn(d->nodes[i].obj, r);
    indexDecl(d);
}

static bool hasKey(const GDecl* d, const char* key) { return mapGet(&d->byKey, key) >= 0 || mapGet(&d->ents, key) >= 0; }

typedef struct Pending {
    const char* owner; /* NULL: the head */
    const char* path;
} Pending;

typedef struct Queue {
    Pending* items;
    int count, cap;
} Queue;

/* the blocks of a node and of the expressions it is made of (a lambda's block, an if's branches) */
static void enqueueFrom(const GDecl* a, J* obj, const char* owner, Queue* q, int depth) {
    if (!obj || depth > 200) return;
    Blocks bs = {0};
    blocksOf(obj, "", &bs);
    for (int i = 0; i < bs.count; i++) {
        QK_PUSH(q->items, q->count, q->cap, ((Pending){owner, bs.items[i].path}));
    }
    Refs r = {0};
    refsOf(obj, false, &r);
    for (int k = 0; k < r.count; k++) {
        GNode* t = nodeAt(a, r.items[k].key);
        if (t && !isBinding(t->obj)) enqueueFrom(a, t->obj, t->key, q, depth + 1);
    }
}

static const char* kindOf(const GDecl* d, const char* key) {
    GNode* n = nodeAt(d, key);
    return n ? jstrOf(n->obj, "kind") : NULL;
}

/* where each statement is: key -> its block's "owner:path" in names (names may be NULL: which statements there are) */
static void statementsIn(const GDecl* d, Map* where, const char*** names, int* count, int* cap) {
    for (int i = -1; i < d->count; i++) {
        J* obj = i < 0 ? d->head : d->nodes[i].obj;
        Blocks bs = {0};
        blocksOf(obj, "", &bs);
        for (int k = 0; k < bs.count; k++) {
            QkBuf b = {0};
            qkBufPrintf(&b, "%s:%s", i < 0 ? d->name : d->nodes[i].key, bs.items[k].path);
            const char* id = qkDup(b.data, b.len);
            qkBufFree(&b);
            int n;
            const char** list = listOf(qkJsonGet(bs.items[k].block, "stmts"), &n);
            for (int s = 0; s < n; s++) {
                mapPut(where, list[s], names ? *count : 0);
                if (names) QK_PUSH(*names, *count, *cap, id);
            }
        }
    }
}

/* b's statements take a's keys where they are the same statements (see the top of the file) */
static void lineUp(const GDecl* a, GDecl* b) {
    Queue q = {0};
    enqueueFrom(a, a->head, NULL, &q, 0);
    for (int at = 0; at < q.count; at++) {
        const char* owner = q.items[at].owner;
        int n, m;
        const char** la = listOf(blockAt(objAt(a, owner), q.items[at].path), &n);
        const char** lb = listOf(blockAt(objAt(b, owner), q.items[at].path), &m);
        if (!la || !lb) continue;
        int* pairOfB = qkNew(sizeof(int) * (size_t)(m + 1));
        bool* pairedA = qkNew(sizeof(bool) * (size_t)(n + 1));
        for (int j = 0; j < m; j++) pairOfB[j] = -1;
        /* 1. named statements keep their keys */
        for (int j = 0; j < m; j++) {
            GNode* nb = nodeAt(b, lb[j]);
            if (!nb || !jstrOf(nb->obj, "name")) continue;
            for (int i = 0; i < n; i++)
                if (!pairedA[i] && !strcmp(la[i], lb[j])) {
                    pairOfB[j] = i;
                    pairedA[i] = true;
                }
        }
        /* 2. the same statements, in order */
        const char** ca = qkNew(sizeof(char*) * (size_t)(n + 1));
        const char** cb = qkNew(sizeof(char*) * (size_t)(m + 1));
        int* ia = qkNew(sizeof(int) * (size_t)(n + 1));
        int* ib = qkNew(sizeof(int) * (size_t)(m + 1));
        int cn = 0, cm = 0;
        for (int i = 0; i < n; i++)
            if (!pairedA[i]) {
                ia[cn] = i;
                ca[cn++] = contentText(a, la[i]);
            }
        for (int j = 0; j < m; j++)
            if (pairOfB[j] < 0) {
                ib[cm] = j;
                cb[cm++] = contentText(b, lb[j]);
            }
        int* match = qkNew(sizeof(int) * (size_t)(cn + 1));
        commonOrder(ca, cn, cb, cm, match);
        for (int i = 0; i < cn; i++)
            if (match[i] >= 0) {
                pairOfB[ib[match[i]]] = ia[i];
                pairedA[ia[i]] = true;
            }
        /* 3. between those, statements of the same kind (and name): the same statement, changed */
        int lastA = -1;
        for (int j = 0; j < m; j++) {
            if (pairOfB[j] >= 0) {
                lastA = pairOfB[j];
                continue;
            }
            int nextA = n;
            for (int k = j + 1; k < m; k++)
                if (pairOfB[k] >= 0) {
                    nextA = pairOfB[k];
                    break;
                }
            for (int i = lastA + 1; i < nextA && i < n; i++) {
                if (pairedA[i] || !same(kindOf(a, la[i]), kindOf(b, lb[j]))) continue;
                GNode* na = nodeAt(a, la[i]);
                GNode* nb = nodeAt(b, lb[j]);
                if (!na || !nb || !same(jstrOf(na->obj, "name"), jstrOf(nb->obj, "name"))) continue;
                pairOfB[j] = i;
                pairedA[i] = true;
                lastA = i;
                break;
            }
        }
        /* b takes a's keys; a new statement whose key a uses for another takes a fresh one */
        Renames r = {0};
        for (int j = 0; j < m; j++) {
            if (pairOfB[j] >= 0) {
                if (strcmp(la[pairOfB[j]], lb[j])) addRename(&r, lb[j], la[pairOfB[j]]);
            } else if (hasKey(a, lb[j])) {
                QkBuf k = {0};
                qkBufPrintf(&k, "%s+", lb[j]);
                while (hasKey(a, k.data) || hasKey(b, k.data)) qkBufByte(&k, '+');
                addRename(&r, lb[j], qkDup(k.data, k.len));
                qkBufFree(&k);
            }
        }
        applyRenames(b, &r);
        for (int i = 0; i < n; i++) {
            GNode* na = nodeAt(a, la[i]);
            if (na) enqueueFrom(a, na->obj, na->key, &q, 0);
        }
    }
    /* 4. statements moved, in their block or to another: the same statement, not lined up yet */
    Map inA = {0}, inB = {0};
    statementsIn(a, &inA, NULL, NULL, NULL);
    statementsIn(b, &inB, NULL, NULL, NULL);
    const char** onlyA = qkNew(sizeof(char*) * (size_t)(a->count + 1));
    const char** contentA = qkNew(sizeof(char*) * (size_t)(a->count + 1));
    int nA = 0;
    for (int i = 0; i < a->count; i++)
        if (mapGet(&inA, a->nodes[i].key) >= 0 && !nodeAt(b, a->nodes[i].key)) {
            onlyA[nA] = a->nodes[i].key;
            contentA[nA++] = contentText(a, a->nodes[i].key);
        }
    Renames r = {0};
    for (int j = 0; j < b->count && nA; j++) {
        const char* key = b->nodes[j].key;
        if (mapGet(&inB, key) < 0 || nodeAt(a, key)) continue;
        const char* text = contentText(b, key);
        for (int i = 0; i < nA; i++)
            if (onlyA[i] && !strcmp(contentA[i], text)) {
                addRename(&r, key, onlyA[i]);
                onlyA[i] = NULL;
                break;
            }
    }
    applyRenames(b, &r);
}

/* ---- what changed -------------------------------------------------------------------------------- */

typedef struct Field {
    const char* name; /* "inputs.1", "returns" */
    const J* was;
    const J* now;
} Field;

typedef struct Change {
    const char* change; /* added, removed, changed, moved */
    const char* key;    /* the node's (a new one's own key in b) */
    const char* now;    /* moved: its own key in b, when it differs */
    const char* kind;
    const char* decl;
    const char* summary;
    bool isDecl;
    bool quiet;         /* part of an added or removed node: in the JSON, not the text */
    int line, order;
    Field* fields;
    int fieldCount, fieldCap;
    const char* from; /* moved to another block: which */
    const char* to;
} Change;

typedef struct Changes {
    Change* items;
    int count, cap;
} Changes;

/* a node in a few words: call print, op +, let total */
static const char* summaryOf(const GDecl* d, const J* obj, int depth) {
    const char* kind = jstrOf(obj, "kind");
    if (!kind) return "node";
    if (!strcmp(kind, "do") && depth < 4) { /* a statement of an expression: the expression */
        J* in = qkJsonGet(obj, "inputs");
        const char* key = in && in->kind == J_ARR && in->count ? jstrOf(in->items[0], "node") : NULL;
        GNode* n = key ? nodeAt(d, key) : NULL;
        if (n) return summaryOf(d, n->obj, depth + 1);
    }
    const char* what = jstrOf(obj, "fn");
    if (!what) what = jstrOf(obj, "op");
    if (!what) what = jstrOf(obj, "name");
    if (!what) what = jstrOf(obj, "field");
    if (!what) what = jstrOf(obj, "pattern");
    QkBuf b = {0};
    qkBufPrintf(&b, what ? "%s %s" : "%s", kind, what);
    const char* s = qkDup(b.data, b.len);
    qkBufFree(&b);
    return s;
}

static void addField(Change* c, const char* name, const J* was, const J* now) {
    Field f = {name, was, now};
    QK_PUSH(c->fields, c->fieldCount, c->fieldCap, f);
}

/* which fields differ: inputs one by one, the rest whole (a block without its statements: they move on their own) */
static void fieldsChanged(Change* c, const J* a, const J* b) {
    const char** names = NULL;
    int n = 0, cap = 0;
    for (int pass = 0; pass < 2; pass++) {
        const J* o = pass ? b : a;
        for (int i = 0; i < o->count; i++) {
            bool seen = false;
            for (int k = 0; k < n; k++) seen |= !strcmp(names[k], o->keys[i]);
            if (!seen) QK_PUSH(names, n, cap, o->keys[i]);
        }
    }
    for (int k = 0; k < n; k++) {
        const J* va = qkJsonGet(a, names[k]);
        const J* vb = qkJsonGet(b, names[k]);
        if (same(jtextOf(va, W_SORTED | W_NO_STMTS), jtextOf(vb, W_SORTED | W_NO_STMTS))) continue;
        if (!strcmp(names[k], "inputs") && va && vb && va->kind == J_ARR && vb->kind == J_ARR) {
            int most = va->count > vb->count ? va->count : vb->count;
            for (int i = 0; i < most; i++) {
                const J* ia = i < va->count ? va->items[i] : NULL;
                const J* ib = i < vb->count ? vb->items[i] : NULL;
                if (same(jtextOf(ia, W_SORTED), jtextOf(ib, W_SORTED))) continue;
                QkBuf f = {0};
                qkBufPrintf(&f, "inputs.%d", i);
                addField(c, qkDup(f.data, f.len), ia, ib);
                qkBufFree(&f);
            }
            continue;
        }
        addField(c, names[k], va, vb);
    }
}

static Change* addChange(Changes* cs, const char* change, const char* key, const char* decl) {
    Change c = {0};
    c.change = change;
    c.key = key;
    c.decl = decl;
    c.order = cs->count;
    QK_PUSH(cs->items, cs->count, cs->cap, c);
    return &cs->items[cs->count - 1];
}

static const char* declKind(const GDecl* d) {
    const char* k = jstrOf(d->head, "kind");
    return k ? k : "decl";
}

static void diffDecl(GDecl* a, GDecl* b, Changes* cs) {
    lineUp(a, b);
    const char* ha = jtextOf(a->head, W_SORTED | W_NO_STMTS);
    const char* hb = jtextOf(b->head, W_SORTED | W_NO_STMTS);
    if (!same(ha, hb)) {
        Change* c = addChange(cs, "changed", a->name, a->name);
        c->kind = declKind(b);
        c->isDecl = true;
        fieldsChanged(c, a->head, b->head);
    }
    for (int i = 0; i < a->count; i++) {
        GNode* na = &a->nodes[i];
        GNode* nb = nodeAt(b, na->key);
        if (!nb) {
            Change* c = addChange(cs, "removed", na->key, a->name);
            c->kind = jstrOf(na->obj, "kind");
            c->summary = summaryOf(a, na->obj, 0);
            c->line = na->line;
            int p = mapGet(&a->parent, na->key);
            c->quiet = p >= 0 && !nodeAt(b, a->nodes[p].key);
            continue;
        }
        if (same(jtextOf(na->obj, W_SORTED | W_NO_STMTS), jtextOf(nb->obj, W_SORTED | W_NO_STMTS))) continue;
        Change* c = addChange(cs, "changed", na->key, a->name);
        c->kind = jstrOf(nb->obj, "kind");
        c->summary = summaryOf(b, nb->obj, 0);
        c->line = na->line;
        fieldsChanged(c, na->obj, nb->obj);
    }
    for (int i = 0; i < b->count; i++) {
        GNode* nb = &b->nodes[i];
        if (nodeAt(a, nb->key)) continue;
        Change* c = addChange(cs, "added", nb->own, a->name);
        c->kind = jstrOf(nb->obj, "kind");
        c->summary = summaryOf(b, nb->obj, 0);
        c->line = nb->line;
        int p = mapGet(&b->parent, nb->key);
        c->quiet = p >= 0 && !nodeAt(a, b->nodes[p].key);
    }
    /* statements moved: to another block, or out of their order in the same one */
    Map whereA = {0}, whereB = {0};
    const char** blockA = NULL;
    const char** blockB = NULL;
    int countA = 0, capA = 0, countB = 0, capB = 0;
    statementsIn(a, &whereA, &blockA, &countA, &capA);
    statementsIn(b, &whereB, &blockB, &countB, &capB);
    for (int i = -1; i < a->count; i++) {
        J* objA = i < 0 ? a->head : a->nodes[i].obj;
        const char* owner = i < 0 ? NULL : a->nodes[i].key;
        J* objB = objAt(b, owner);
        Blocks bs = {0};
        blocksOf(objA, "", &bs);
        for (int k = 0; k < bs.count; k++) {
            int n, m;
            const char** la = listOf(qkJsonGet(bs.items[k].block, "stmts"), &n);
            const char** lb = listOf(blockAt(objB, bs.items[k].path), &m);
            /* the statements in both, in each one's order */
            const char** sa = qkNew(sizeof(char*) * (size_t)(n + 1));
            const char** sb = qkNew(sizeof(char*) * (size_t)(m + 1));
            int sn = 0, sm = 0;
            for (int s = 0; s < n; s++) {
                int wb = mapGet(&whereB, la[s]);
                int wa = mapGet(&whereA, la[s]);
                if (wb < 0) continue;
                if (strcmp(blockA[wa], blockB[wb])) { /* to another block */
                    Change* c = addChange(cs, "moved", la[s], a->name);
                    GNode* nb = nodeAt(b, la[s]);
                    c->kind = nb ? jstrOf(nb->obj, "kind") : NULL;
                    c->summary = nb ? summaryOf(b, nb->obj, 0) : NULL;
                    c->line = nodeAt(a, la[s]) ? nodeAt(a, la[s])->line : 0;
                    c->from = blockA[wa];
                    c->to = blockB[wb];
                    if (nb && strcmp(nb->own, la[s])) c->now = nb->own;
                    continue;
                }
                sa[sn++] = la[s];
            }
            for (int s = 0; s < m; s++)
                for (int t = 0; t < sn; t++)
                    if (!strcmp(lb[s], sa[t])) sb[sm++] = lb[s];
            int* match = qkNew(sizeof(int) * (size_t)(sn + 1));
            commonOrder(sa, sn, sb, sm, match);
            for (int s = 0; s < sn; s++) {
                if (match[s] >= 0) continue;
                Change* c = addChange(cs, "moved", sa[s], a->name);
                GNode* nb = nodeAt(b, sa[s]);
                c->kind = nb ? jstrOf(nb->obj, "kind") : NULL;
                c->summary = nb ? summaryOf(b, nb->obj, 0) : NULL;
                c->line = nodeAt(a, sa[s]) ? nodeAt(a, sa[s])->line : 0;
                if (nb && strcmp(nb->own, sa[s])) c->now = nb->own;
            }
        }
    }
}

static int byPlace(const void* x, const void* y) {
    const Change* a = x;
    const Change* b = y;
    if (a->isDecl != b->isDecl) return a->isDecl ? -1 : 1;
    if (a->line != b->line) return a->line - b->line;
    return a->order - b->order;
}

/* an input or a value, as text: 12, "hi", main/total, Color.Red */
static void valueText(const J* v, QkBuf* b) {
    if (!v) {
        qkBufPuts(b, "nothing");
        return;
    }
    if (v->kind == J_OBJ) {
        static const char* const plain[] = {"value", "node", "case", "fn", "name"};
        for (int i = 0; i < 5; i++)
            if (jstrOf(v, plain[i]) && v->count <= 2) {
                qkBufPuts(b, jstrOf(v, plain[i]));
                return;
            }
    }
    jwrite(v, b, W_SORTED | W_NO_STMTS);
}

static void writeChangesText(const Changes* cs, QkBuf* out) {
    for (int i = 0; i < cs->count; i++) {
        const Change* c = &cs->items[i];
        if (c->quiet) continue;
        qkBufPrintf(out, "%-8s ", c->change);
        if (c->isDecl) qkBufPrintf(out, "%s %s", c->kind, c->key);
        else qkBufPuts(out, c->key);
        if (c->now) qkBufPrintf(out, " (now %s)", c->now);
        if (c->summary || c->fieldCount || c->from) qkBufPuts(out, ": ");
        if (c->fieldCount) {
            for (int f = 0; f < c->fieldCount; f++) {
                const char* name = c->fields[f].name;
                if (!strncmp(name, "inputs.", 7)) qkBufPrintf(out, "%sinput %s: ", f ? "; " : "", name + 7);
                else qkBufPrintf(out, "%s%s: ", f ? "; " : "", name);
                valueText(c->fields[f].was, out);
                qkBufPuts(out, " -> ");
                valueText(c->fields[f].now, out);
            }
        } else if (c->summary) {
            qkBufPuts(out, c->summary);
        }
        if (c->from) qkBufPrintf(out, "%sfrom %s to %s", c->summary ? ", " : "", c->from, c->to);
        qkBufByte(out, '\n');
    }
}

static void writeChangesJson(const Changes* cs, QkBuf* out) {
    qkBufPuts(out, "{\"changes\": [");
    for (int i = 0; i < cs->count; i++) {
        const Change* c = &cs->items[i];
        qkBufPuts(out, i ? ",\n  {\"change\": " : "\n  {\"change\": ");
        qkJsonString(out, c->change);
        qkBufPuts(out, ", \"key\": ");
        qkJsonString(out, c->key);
        if (c->kind) {
            qkBufPuts(out, ", \"kind\": ");
            qkJsonString(out, c->kind);
        }
        if (c->isDecl) qkBufPuts(out, ", \"decl\": true");
        else {
            qkBufPuts(out, ", \"in\": ");
            qkJsonString(out, c->decl);
        }
        if (c->line) qkBufPrintf(out, ", \"line\": %d", c->line);
        if (c->now) {
            qkBufPuts(out, ", \"now\": ");
            qkJsonString(out, c->now);
        }
        if (c->from) {
            qkBufPuts(out, ", \"from\": ");
            qkJsonString(out, c->from);
            qkBufPuts(out, ", \"to\": ");
            qkJsonString(out, c->to);
        }
        if (c->fieldCount) {
            qkBufPuts(out, ", \"fields\": [");
            for (int f = 0; f < c->fieldCount; f++) {
                qkBufPuts(out, f ? ", {\"field\": " : "{\"field\": ");
                qkJsonString(out, c->fields[f].name);
                qkBufPuts(out, ", \"was\": ");
                jwrite(c->fields[f].was, out, W_NO_STMTS);
                qkBufPuts(out, ", \"now\": ");
                jwrite(c->fields[f].now, out, W_NO_STMTS);
                qkBufPuts(out, "}");
            }
            qkBufPuts(out, "]");
        }
        qkBufPuts(out, "}");
    }
    qkBufPuts(out, cs->count ? "\n]}\n" : "]}\n");
}

int qkGraphDiff(const char* a, size_t aLen, const char* b, size_t bLen, bool json, QkBuf* out) {
    Graph ga, gb;
    if (!readGraph(a, aLen, "the first graph", &ga) || !readGraph(b, bLen, "the second graph", &gb)) return -1;
    Changes cs = {0};
    for (int i = 0; i < ga.count; i++) {
        GDecl* da = ga.decls[i];
        GDecl* db = declNamed(&gb, da->name);
        int first = cs.count;
        if (!db) {
            Change* c = addChange(&cs, "removed", da->name, da->name);
            c->kind = declKind(da);
            c->isDecl = true;
        } else {
            diffDecl(da, db, &cs);
        }
        qsort(cs.items + first, (size_t)(cs.count - first), sizeof(Change), byPlace);
    }
    for (int i = 0; i < gb.count; i++)
        if (!declNamed(&ga, gb.decls[i]->name)) {
            Change* c = addChange(&cs, "added", gb.decls[i]->name, gb.decls[i]->name);
            c->kind = declKind(gb.decls[i]);
            c->isDecl = true;
        }
    if (!same(ga.closing, gb.closing)) {
        Change* c = addChange(&cs, "changed", "closing", "");
        c->kind = "comment";
        c->isDecl = true;
        addField(c, "closing", ga.closing ? jtext(ga.closing) : NULL, gb.closing ? jtext(gb.closing) : NULL);
    }
    if (json) writeChangesJson(&cs, out);
    else writeChangesText(&cs, out);
    return cs.count;
}

/* ---- three-way merges ---------------------------------------------------------------------------- */

typedef struct Conflict {
    const char* key;
    const char* what;
} Conflict;

typedef struct Conflicts {
    Conflict* items;
    int count, cap;
    Map seen;
} Conflicts;

static void conflict(Conflicts* cs, const char* key, const char* what) {
    QkBuf b = {0};
    qkBufPrintf(&b, "%s\n%s", key, what);
    if (mapGet(&cs->seen, b.data) >= 0) {
        qkBufFree(&b);
        return;
    }
    mapPut(&cs->seen, qkDup(b.data, b.len), 1);
    qkBufFree(&b);
    QK_PUSH(cs->items, cs->count, cs->cap, ((Conflict){key, what}));
}

/* Which version to take: 0 base, 1 ours, 2 theirs; -1 a conflict (and *what says which) */
static int pick(const char* base, const char* ours, const char* theirs, const char** what) {
    if (same(ours, theirs)) return 1;
    if (same(base, ours)) return 2;
    if (same(base, theirs)) return 1;
    *what = !base ? "added on both sides, differently" : !ours || !theirs ? "changed on one side, removed on the other"
                                                                           : "changed on both sides";
    return -1;
}

static bool inList(const char** list, int n, const char* key) {
    for (int i = 0; i < n; i++)
        if (!strcmp(list[i], key)) return true;
    return false;
}

static bool sameList(const char** a, int n, const char** b, int m) {
    if (n != m) return false;
    for (int i = 0; i < n; i++)
        if (strcmp(a[i], b[i])) return false;
    return true;
}

/* each side's new statements, after the last statement both keep before them (NULL: at the start) */
typedef struct Add {
    const char* anchor;
    const char* key;
} Add;

typedef struct Adds {
    Add* items;
    int count, cap;
} Adds;

static void addsOf(const char** side, int n, const char** base, int nb, const char** kept, int nk, Adds* out) {
    const char* anchor = NULL;
    for (int i = 0; i < n; i++) {
        if (inList(kept, nk, side[i])) {
            anchor = side[i];
        } else if (!inList(base, nb, side[i])) {
            QK_PUSH(out->items, out->count, out->cap, ((Add){anchor, side[i]}));
        }
    }
}

static void emitAdds(const Adds* a, const char* anchor, const char*** out, int* n, int* cap) {
    for (int i = 0; i < a->count; i++)
        if (same(a->items[i].anchor, anchor) && !inList(*out, *n, a->items[i].key)) QK_PUSH(*out, *n, *cap, a->items[i].key);
}

static bool addsAt(const Adds* a, const char* anchor, const char*** keys, int* n) {
    *n = 0;
    *keys = qkNew(sizeof(char*) * (size_t)(a->count + 1));
    for (int i = 0; i < a->count; i++)
        if (same(a->items[i].anchor, anchor)) (*keys)[(*n)++] = a->items[i].key;
    return *n > 0;
}

/* A block's statements, merged: what each side removed is gone, each side's new ones come after the statement
 * before them, and a side's new order wins over an unchanged one. */
static const char** mergeList(const char** b, int nb, const char** o, int no, const char** t, int nt, int* n, const char* where,
                              bool statements, Conflicts* cs) {
    if (sameList(o, no, t, nt) || sameList(b, nb, t, nt)) {
        *n = no;
        return o;
    }
    if (sameList(b, nb, o, no)) {
        *n = nt;
        return t;
    }
    const char** kept = qkNew(sizeof(char*) * (size_t)(nb + 1));
    int nk = 0;
    for (int i = 0; i < nb; i++)
        if (inList(o, no, b[i]) && inList(t, nt, b[i])) kept[nk++] = b[i];
    const char** ko = qkNew(sizeof(char*) * (size_t)(nk + 1));
    const char** kt = qkNew(sizeof(char*) * (size_t)(nk + 1));
    int nko = 0, nkt = 0;
    for (int i = 0; i < no; i++)
        if (inList(kept, nk, o[i])) ko[nko++] = o[i];
    for (int i = 0; i < nt; i++)
        if (inList(kept, nk, t[i])) kt[nkt++] = t[i];
    const char** order = ko;
    if (sameList(ko, nko, kept, nk)) order = kt;
    else if (!sameList(kt, nkt, kept, nk) && !sameList(ko, nko, kt, nkt))
        conflict(cs, where, statements ? "statements reordered on both sides" : "declarations reordered on both sides");
    Adds ao = {0}, at = {0};
    addsOf(o, no, b, nb, kept, nk, &ao);
    addsOf(t, nt, b, nb, kept, nk, &at);
    const char** out = NULL;
    int count = 0, cap = 0;
    for (int i = -1; i < nk; i++) {
        const char* anchor = i < 0 ? NULL : order[i];
        if (anchor) QK_PUSH(out, count, cap, anchor);
        const char** x;
        const char** y;
        int nx, ny;
        /* both sides add statements at one place: which go first is a choice (declarations' order is not) */
        if (statements && addsAt(&ao, anchor, &x, &nx) && addsAt(&at, anchor, &y, &ny) && !sameList(x, nx, y, ny)) {
            QkBuf w = {0};
            if (anchor) qkBufPrintf(&w, "statements added after %s on both sides", anchor);
            else qkBufPuts(&w, "statements added at the start on both sides");
            conflict(cs, where, qkDup(w.data, w.len));
            qkBufFree(&w);
        }
        emitAdds(&ao, anchor, &out, &count, &cap);
        emitAdds(&at, anchor, &out, &count, &cap);
    }
    *n = count;
    return out;
}

/* the whole declaration as text (heads and nodes by key), to see whether a side changed it at all */
static int byGNodeKey(const void* x, const void* y) { return strcmp(((const GNode*)x)->key, ((const GNode*)y)->key); }

static const char* declText(const GDecl* d) {
    if (!d) return NULL;
    QkBuf b = {0};
    jwrite(d->head, &b, W_SORTED);
    GNode* sorted = malloc(sizeof(GNode) * (size_t)(d->count ? d->count : 1));
    memcpy(sorted, d->nodes, sizeof(GNode) * (size_t)d->count);
    qsort(sorted, (size_t)d->count, sizeof(GNode), byGNodeKey);
    for (int i = 0; i < d->count; i++) {
        qkBufByte(&b, '\n');
        jwrite(sorted[i].obj, &b, W_SORTED);
    }
    free(sorted);
    const char* s = qkDup(b.data, b.len);
    qkBufFree(&b);
    return s;
}

static const char* ownKey(const GDecl* d, const char* key) {
    GNode* n = d ? nodeAt(d, key) : NULL;
    return n ? n->own : key;
}

/* the block a statement is in: its owner (NULL: the head), its path and list */
static bool blockOfStatement(const GDecl* d, const char* key, const char** owner, const char** path, const char*** list, int* n) {
    for (int i = -1; i < d->count; i++) {
        J* obj = i < 0 ? d->head : d->nodes[i].obj;
        Blocks bs = {0};
        blocksOf(obj, "", &bs);
        for (int k = 0; k < bs.count; k++) {
            *list = listOf(qkJsonGet(bs.items[k].block, "stmts"), n);
            if (inList(*list, *n, key)) {
                *owner = i < 0 ? NULL : d->nodes[i].key;
                *path = bs.items[k].path;
                return true;
            }
        }
    }
    return false;
}

static GDecl* mergeDecl(GDecl* b, GDecl* o, GDecl* t, Conflicts* cs) {
    lineUp(b, o);
    lineUp(b, t);
    GDecl* m = qkNew(sizeof *m);
    m->name = b->name;
    const char* what = NULL;
    int side = pick(jtextOf(b->head, W_SORTED | W_NO_STMTS), jtextOf(o->head, W_SORTED | W_NO_STMTS),
                    jtextOf(t->head, W_SORTED | W_NO_STMTS), &what);
    if (side < 0) conflict(cs, b->name, what);
    GDecl* sides[3] = {b, o, t};
    m->head = jcopy(sides[side < 0 ? 1 : side]->head);
    /* every node: from the side that changed it */
    Map seen = {0};
    for (int s = 0; s < 3; s++)
        for (int i = 0; i < sides[s]->count; i++) {
            const char* key = sides[s]->nodes[i].key;
            if (mapGet(&seen, key) >= 0) continue;
            mapPut(&seen, key, 1);
            GNode* n[3] = {nodeAt(b, key), nodeAt(o, key), nodeAt(t, key)};
            const char* texts[3];
            for (int k = 0; k < 3; k++) texts[k] = n[k] ? jtextOf(n[k]->obj, W_SORTED | W_NO_STMTS) : NULL;
            int from = pick(texts[0], texts[1], texts[2], &what);
            if (from < 0) {
                conflict(cs, ownKey(o, key), what);
                from = 1;
            }
            if (!n[from]) continue;
            GNode g = {key, key, jcopy(n[from]->obj), n[from]->line};
            QK_PUSH(m->nodes, m->count, m->cap, g);
        }
    indexDecl(m);
    /* every block: its statements merged */
    for (int i = -1; i < m->count; i++) {
        const char* owner = i < 0 ? NULL : m->nodes[i].key;
        J* obj = i < 0 ? m->head : m->nodes[i].obj;
        Blocks bs = {0};
        blocksOf(obj, "", &bs);
        for (int k = 0; k < bs.count; k++) {
            int nb, no, nt, n;
            const char** lb = listOf(blockAt(objAt(b, owner), bs.items[k].path), &nb);
            const char** lo = listOf(blockAt(objAt(o, owner), bs.items[k].path), &no);
            const char** lt = listOf(blockAt(objAt(t, owner), bs.items[k].path), &nt);
            const char** merged = mergeList(lb, nb, lo, no, lt, nt, &n, owner ? ownKey(o, owner) : b->name, true, cs);
            setList(qkJsonGet(bs.items[k].block, "stmts"), merged, n);
        }
    }
    indexDecl(m);
    /* a node one side removed and the other still uses (or keeps in a block): kept, and a conflict */
    Map inB = {0}, inO = {0}, inT = {0};
    statementsIn(b, &inB, NULL, NULL, NULL);
    statementsIn(o, &inO, NULL, NULL, NULL);
    statementsIn(t, &inT, NULL, NULL, NULL);
    for (bool again = true; again;) {
        again = false;
        for (int i = -1; i < m->count && !again; i++) {
            Refs r = {0};
            refsOf(i < 0 ? m->head : m->nodes[i].obj, true, &r);
            for (int k = 0; k < r.count; k++) {
                const char* key = r.items[k].key;
                if (hasKey(m, key)) continue;
                GDecl* from = nodeAt(o, key) ? o : nodeAt(t, key) ? t : nodeAt(b, key) ? b : NULL;
                conflict(cs, ownKey(o, key), "removed on one side, still used on the other");
                if (!from) continue;
                GNode* src = nodeAt(from, key);
                GNode g = {key, key, jcopy(src->obj), src->line};
                QK_PUSH(m->nodes, m->count, m->cap, g);
                indexDecl(m);
                again = true;
                break;
            }
        }
        /* a kept statement goes back in its block, after the statement before it there */
        Map inM = {0};
        statementsIn(m, &inM, NULL, NULL, NULL);
        for (int i = 0; i < m->count; i++) {
            const char* key = m->nodes[i].key;
            const char *owner, *path;
            const char** list;
            int n, mn;
            if (mapGet(&inM, key) >= 0) continue;
            GDecl* from = mapGet(&inO, key) >= 0 ? o : mapGet(&inT, key) >= 0 ? t : mapGet(&inB, key) >= 0 ? b : NULL;
            if (!from) continue;
            if (!blockOfStatement(from, key, &owner, &path, &list, &n)) continue;
            J* stmts = blockAt(objAt(m, owner), path);
            if (!stmts) continue;
            int at = 0;
            const char** cur = listOf(stmts, &mn);
            for (int s = 0; s < n && strcmp(list[s], key); s++)
                for (int c = 0; c < mn; c++)
                    if (!strcmp(cur[c], list[s])) at = c + 1;
            const char** grown = qkNew(sizeof(char*) * (size_t)(mn + 1));
            memcpy(grown, cur, sizeof(char*) * (size_t)at);
            grown[at] = key;
            memcpy(grown + at + 1, cur + at, sizeof(char*) * (size_t)(mn - at));
            setList(stmts, grown, mn + 1);
            again = true;
            break;
        }
        if (again) indexDecl(m);
    }
    return m;
}

/* A declaration as graph JSON again: keys back to numbers. */
static void writeNumbered(const J* v, QkBuf* b, const Map* num) {
    switch (v->kind) {
    case J_ARR:
        qkBufByte(b, '[');
        for (int i = 0; i < v->count; i++) {
            if (i) qkBufPuts(b, ", ");
            writeNumbered(v->items[i], b, num);
        }
        qkBufByte(b, ']');
        return;
    case J_OBJ:
        qkBufByte(b, '{');
        for (int i = 0; i < v->count; i++) {
            const char* f = v->keys[i];
            const J* item = v->items[i];
            if (i) qkBufPuts(b, ", ");
            qkJsonString(b, f);
            qkBufPuts(b, ": ");
            if ((!strcmp(f, "id") || !strcmp(f, "node") || !strcmp(f, "elseIf")) && item->kind == J_STR) {
                qkBufPrintf(b, "%d", mapGet(num, item->str));
            } else if (!strcmp(f, "stmts") && item->kind == J_ARR) {
                qkBufByte(b, '[');
                for (int k = 0; k < item->count; k++)
                    qkBufPrintf(b, "%s%d", k ? ", " : "", item->items[k]->kind == J_STR ? mapGet(num, item->items[k]->str) : -1);
                qkBufByte(b, ']');
            } else {
                writeNumbered(item, b, num);
            }
        }
        qkBufByte(b, '}');
        return;
    default: jwrite(v, b, 0);
    }
}

static void numberIds(const J* v, Map* num, int* next) {
    for (int i = 0; i < v->count; i++) {
        const J* item = v->items[i];
        if (v->kind == J_OBJ && !strcmp(v->keys[i], "id") && item->kind == J_STR && mapGet(num, item->str) < 0) mapPut(num, item->str, (*next)++);
        if (item->kind == J_OBJ || item->kind == J_ARR) numberIds(item, num, next);
    }
}

static void writeDecl(const GDecl* d, QkBuf* out, bool first) {
    Map num = {0};
    int next = 1;
    numberIds(d->head, &num, &next);
    for (int i = 0; i < d->count; i++) numberIds(d->nodes[i].obj, &num, &next);
    qkBufPuts(out, first ? "  " : ",\n  ");
    QkBuf h = {0}; /* the head, with its nodes after it */
    writeNumbered(d->head, &h, &num);
    if (d->count) {
        h.len--; /* its closing brace */
        qkBufPuts(&h, ",\n   \"nodes\": [");
        for (int i = 0; i < d->count; i++) {
            qkBufPuts(&h, i ? ",\n    " : "\n    ");
            writeNumbered(d->nodes[i].obj, &h, &num);
        }
        qkBufPuts(&h, "]}");
    }
    qkBufAdd(out, h.data, h.len);
    qkBufFree(&h);
}

int qkGraphMerge(const char* base, size_t baseLen, const char* ours, size_t oursLen, const char* theirs, size_t theirsLen,
                 QkBuf* merged, bool json, QkBuf* conflicts) {
    Graph gb, go, gt;
    if (!readGraph(base, baseLen, "the base graph", &gb) || !readGraph(ours, oursLen, "our graph", &go) ||
        !readGraph(theirs, theirsLen, "their graph", &gt))
        return -1;
    Conflicts cs = {0};
    const char** names[3];
    int counts[3];
    Graph* gs[3] = {&gb, &go, &gt};
    for (int s = 0; s < 3; s++) {
        names[s] = qkNew(sizeof(char*) * (size_t)(gs[s]->count + 1));
        counts[s] = gs[s]->count;
        for (int i = 0; i < gs[s]->count; i++) names[s][i] = gs[s]->decls[i]->name;
    }
    int n;
    const char** order = mergeList(names[0], counts[0], names[1], counts[1], names[2], counts[2], &n, "the program", false, &cs);
    qkBufPuts(merged, "{\"quark\": 1, \"decls\": [\n");
    int written = 0;
    for (int i = 0; i < n; i++) {
        GDecl* db = declNamed(&gb, order[i]);
        GDecl* dO = declNamed(&go, order[i]);
        GDecl* dt = declNamed(&gt, order[i]);
        GDecl* take;
        if (db && dO && dt) {
            take = mergeDecl(db, dO, dt, &cs);
        } else {
            const char* what = NULL;
            int side = pick(declText(db), declText(dO), declText(dt), &what);
            if (side < 0) {
                conflict(&cs, order[i], what);
                side = 1;
            }
            take = side == 0 ? db : side == 1 ? dO : dt;
            if (!take) continue;
        }
        writeDecl(take, merged, !written++);
    }
    qkBufPuts(merged, "]");
    const char* what = NULL;
    int side = pick(gb.closing, go.closing, gt.closing, &what);
    if (side < 0) conflict(&cs, "closing", "the closing comment changed on both sides");
    const char* closing = side == 0 ? gb.closing : side == 2 ? gt.closing : go.closing;
    if (closing) {
        qkBufPuts(merged, ",\n \"closing\": ");
        qkJsonString(merged, closing);
    }
    qkBufPuts(merged, "}\n");
    if (json) {
        qkBufPuts(conflicts, "[");
        for (int i = 0; i < cs.count; i++) {
            qkBufPuts(conflicts, i ? ",\n {\"key\": " : "\n {\"key\": ");
            qkJsonString(conflicts, cs.items[i].key);
            qkBufPuts(conflicts, ", \"what\": ");
            qkJsonString(conflicts, cs.items[i].what);
            qkBufPuts(conflicts, "}");
        }
        qkBufPuts(conflicts, cs.count ? "\n]\n" : "]\n");
    } else {
        for (int i = 0; i < cs.count; i++) qkBufPrintf(conflicts, "conflict %s: %s\n", cs.items[i].key, cs.items[i].what);
    }
    return cs.count;
}
