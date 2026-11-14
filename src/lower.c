/* Quark — lowering: the checked tree to a small register IR. Every value is a few 8-byte slots (an Int is
 * one, a record of three Floats three, an array two: its address and length), so records live in registers, never
 * in memory. Loops are fused: map, filter and the like do not build arrays; they add code inside the one
 * loop of their source (a range or an array), and sum, fold, any... end it. Lambdas are inlined. */
#include "qk.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct Val {
    int n;
    int* r;
} Val;

typedef struct Lower Lower;
typedef void (*Sink)(Lower* L, Val elem, int index, void* ctx);

struct Lower {
    Program* prog;
    IrProgram* out;
    IrFn* fn;
    int* fnIndex;     /* decl index -> IR function */
    int depth;        /* loop depth, for weights */
    int loopBreak;    /* the innermost loop the code is in */
    int loopContinue;
    struct DeferAt {  /* the defers of the blocks being lowered, in the order written */
        Block* body;
        int inlineDepth; /* the function they belong to (inlined ones too) */
        int loopLevel;   /* the loops they are inside */
    } * defers;
    int deferCount, deferCap;
    int loopLevel;    /* how many of the source's loops the code is in */
    int userBreak;    /* where break / continue in the source go */
    int userContinue;
    Expr* preRange;   /* a range whose bounds are already in registers */
    int preA, preB;
    Expr* preArray;   /* an array already in registers */
    Val preArrayVal;
    struct InlineFrame {
        Decl* d;
        int end;    /* where its returns jump */
        Val result; /* where they leave their value */
    } inlining[32]; /* functions being inlined, innermost last */
    Decl* current;     /* the function being lowered */
    int inl;           /* the inlined calls the code being lowered is in (an InlineCall), 0: none */
    int* externIndex;  /* decl index -> the program's host function list, or -1 */
    int inlineDepth;
    struct Helper {
        const Type* t; /* print a value of this type: one function, which may call itself */
        int index;
        bool byValue;  /* its slots as the arguments (else the address of a box holding them) */
    } * helpers;
    int helperCount, helperCap;
    const Type* printing[64]; /* types being printed inline: one met again is printed by a call (enum Json) */
    int printingCount;
};
typedef struct InlineFrame InlineFrame;

/* ---- emitting ------------------------------------------------------------------------------------ */

static int newReg(Lower* L, char kind) {
    IrFn* f = L->fn;
    if (f->vregCount == f->vregCap) {
        int cap = f->vregCap ? f->vregCap * 2 : 64;
        char* kinds = qkNew((size_t)cap);
        double* weight = qkNew(sizeof(double) * (size_t)cap);
        if (f->vregCount) {
            memcpy(kinds, f->kinds, (size_t)f->vregCount);
            memcpy(weight, f->weight, sizeof(double) * (size_t)f->vregCount);
        }
        f->kinds = kinds;
        f->weight = weight;
        f->vregCap = cap;
    }
    f->kinds[f->vregCount] = kind;
    return f->vregCount++;
}

static void use(Lower* L, int reg) {
    if (reg < 0) return;
    int d = L->depth > 5 ? 5 : L->depth;
    L->fn->weight[reg] += pow(8.0, d);
}

static IrIns* emit(Lower* L, IrOp op, int dst, int a, int b) {
    IrIns ins = {0};
    ins.op = (uint8_t)op;
    ins.dst = dst;
    ins.a = a;
    ins.b = b;
    ins.c = -1;
    ins.inl = L->inl;
    use(L, dst);
    use(L, a);
    use(L, b);
    QK_PUSH(L->fn->code, L->fn->count, L->fn->cap, ins);
    return &L->fn->code[L->fn->count - 1];
}

static int iconst(Lower* L, int64_t v) {
    int r = newReg(L, 'I');
    emit(L, IR_CONST, r, -1, -1)->imm = v;
    return r;
}

static int fconst(Lower* L, double v) {
    int r = newReg(L, 'F');
    int64_t bits;
    memcpy(&bits, &v, 8);
    emit(L, IR_CONST, r, -1, -1)->imm = bits;
    return r;
}

static int op2(Lower* L, IrOp op, int a, int b, char kind) {
    int r = newReg(L, kind);
    emit(L, op, r, a, b);
    return r;
}

static int cmp(Lower* L, int cond, int a, int b) {
    int r = newReg(L, 'I');
    emit(L, L->fn->kinds[a] == 'F' ? IR_FCMP : IR_CMP, r, a, b)->cond = (uint8_t)cond;
    return r;
}

static void mov(Lower* L, int dst, int src) {
    if (dst != src) emit(L, IR_MOV, dst, src, -1);
}

static int label(Lower* L) { return L->fn->labelCount++; }
static void place(Lower* L, int l) { emit(L, IR_LABEL, -1, -1, -1)->imm = l; }
static void jmp(Lower* L, int l) { emit(L, IR_JMP, -1, -1, -1)->imm = l; }
static void jz(Lower* L, int r, int l) { emit(L, IR_JZ, -1, r, -1)->imm = l; }
static void jnz(Lower* L, int r, int l) { emit(L, IR_JNZ, -1, r, -1)->imm = l; }

static int* regList(int n) { return qkNew(sizeof(int) * (size_t)(n ? n : 1)); }

static Val valOf(Lower* L, const Type* t) {
    Val v = {qkTypeSlots(t), NULL};
    v.r = regList(v.n);
    char kinds[256];
    qkTypeSlotKinds(t, kinds);
    for (int i = 0; i < v.n; i++) v.r[i] = newReg(L, kinds[i]);
    return v;
}

static Val one(int r) {
    Val v = {1, regList(1)};
    v.r[0] = r;
    return v;
}

static Val copyVal(Lower* L, Val v) {
    Val c = {v.n, regList(v.n)};
    for (int i = 0; i < v.n; i++) {
        c.r[i] = newReg(L, L->fn->kinds[v.r[i]]);
        mov(L, c.r[i], v.r[i]);
    }
    return c;
}

static void movVal(Lower* L, Val dst, Val src) {
    for (int i = 0; i < dst.n && i < src.n; i++) {
        if (L->fn->kinds[dst.r[i]] == 'F' && L->fn->kinds[src.r[i]] == 'I') emit(L, IR_I2F, dst.r[i], src.r[i], -1);
        else mov(L, dst.r[i], src.r[i]);
    }
}

static int rtcall(Lower* L, RtId id, int* args, int n, char retKind, int line) {
    IrIns* ins = emit(L, IR_RTCALL, -1, -1, -1);
    ins->imm = id;
    ins->line = line;
    ins->args = regList(n);
    memcpy(ins->args, args, sizeof(int) * (size_t)n);
    ins->argCount = n;
    for (int i = 0; i < n; i++) use(L, args[i]);
    if (n > L->fn->maxOut) L->fn->maxOut = n; /* the fifth argument on goes on the stack */
    if (retKind == 'V') return -1;
    int r = newReg(L, retKind);
    ins = &L->fn->code[L->fn->count - 1];
    ins->rets = regList(1);
    ins->rets[0] = r;
    ins->retCount = 1;
    use(L, r);
    return r;
}

static void fuel(Lower* L, int line) {
    if (L->out->fuel) emit(L, IR_FUEL, -1, -1, -1)->line = line;
}

static int dataText(Lower* L, const char* s, int n) {
    int at = (int)L->out->data.len;
    qkBufAdd(&L->out->data, s, (size_t)n);
    qkBufByte(&L->out->data, 0);
    while (L->out->data.len % 8) qkBufByte(&L->out->data, 0);
    return at;
}

static int dataAddress(Lower* L, int offset) {
    int r = newReg(L, 'I');
    IrIns* ins = emit(L, IR_CONST, r, -1, -1);
    ins->imm = offset;
    ins->c = 1;
    return r;
}

static void printLiteral(Lower* L, const char* s) {
    int args[2] = {dataAddress(L, dataText(L, s, (int)strlen(s))), iconst(L, (int64_t)strlen(s))};
    rtcall(L, RT_PRINT_TEXT, args, 2, 'V', 0);
}

/* ---- memory -------------------------------------------------------------------------------------- */

static int elemBytes(const Type* arrayType) { return qkTypeSlots(arrayType->elem) * 8; }

static Val loadElem(Lower* L, Val arr, int index, const Type* elem) {
    Val v = valOf(L, elem);
    for (int j = 0; j < v.n; j++) {
        IrIns* ins = emit(L, IR_LOAD, v.r[j], arr.r[0], index);
        ins->imm = v.n * 8;
        ins->c = j * 8;
    }
    return v;
}

static void storeSlots(Lower* L, int base, int index, int stride, int offset, Val v) {
    for (int j = 0; j < v.n; j++) {
        IrIns* ins = emit(L, IR_STORE, v.r[j], base, index);
        ins->imm = stride;
        ins->c = offset + j * 8;
    }
}

static void bounds(Lower* L, int index, int len, int code, int line) {
    IrIns* ins = emit(L, IR_BOUNDS, -1, index, len);
    ins->imm = code;
    ins->line = line;
}

static int alloc(Lower* L, int count, int bytesEach, int line) {
    int args[2] = {count, iconst(L, bytesEach)};
    return rtcall(L, RT_ALLOC, args, 2, 'I', line);
}

static Val copyArray(Lower* L, Val arr, int bytesEach, int line) {
    int args[3] = {arr.r[0], arr.r[1], iconst(L, bytesEach)};
    Val c = {2, regList(2)};
    c.r[0] = rtcall(L, RT_COPY, args, 3, 'I', line);
    c.r[1] = newReg(L, 'I');
    mov(L, c.r[1], arr.r[1]);
    return c;
}

/* ---- expressions --------------------------------------------------------------------------------- */

bool qkChecked;

static Val lowerExpr(Lower* L, Expr* e);
static Val lowerBlock(Lower* L, Block* b, Type* t);
static void lowerReturn(Lower* L, Val v);
static int equalVals(Lower* L, Val a, Val b, const Type* t, int line);
typedef void (*EntryFn)(Lower* L, Val key, Val value, void* ctx);
static void forEachEntry(Lower* L, Val m, const Type* t, EntryFn body, void* ctx, int line);
static void lowerStmt(Lower* L, Stmt* s);
static void forEach(Lower* L, Expr* src, Sink sink, void* ctx);

static void typeDesc(const Type* t, QkBuf* out);
static int dataText(Lower* L, const char* s, int n);
static int dataAddress(Lower* L, int offset);
static void storeSlots(Lower* L, int base, int index, int stride, int offset, Val v);
static int alloc(Lower* L, int count, int bytesEach, int line);
static Val lowerExpr(Lower* L, Expr* e);

/* A function kept as a value: its address, and the address of the values it captured (0: none). Those values
 * are copied where the lambda is made, after a description of their types (for a loop's memory). */
static Val fnAddress(Lower* L, Decl* d) {
    Val v = {2, regList(2)};
    v.r[0] = newReg(L, 'I');
    IrIns* ins = emit(L, IR_CONST, v.r[0], -1, -1);
    ins->imm = L->fnIndex[d->index];
    ins->c = 2;
    if (!d->captureCount) {
        v.r[1] = newReg(L, 'I');
        emit(L, IR_CONST, v.r[1], -1, -1)->imm = 0;
        return v;
    }
    int slots = 0;
    QkBuf desc = {0};
    for (int i = 0; i < d->captureCount; i++) {
        slots += qkTypeSlots(d->captures[i].outer->type);
        typeDesc(d->captures[i].outer->type, &desc);
    }
    int count = newReg(L, 'I');
    emit(L, IR_CONST, count, -1, -1)->imm = 1;
    int env = alloc(L, count, 8 * (slots + 1), d->line);
    int zero = newReg(L, 'I');
    emit(L, IR_CONST, zero, -1, -1)->imm = 0;
    storeSlots(L, env, zero, 8, 0, one(dataAddress(L, dataText(L, desc.data ? desc.data : "", (int)desc.len))));
    qkBufFree(&desc);
    for (int i = 0, at = 8; i < d->captureCount; i++) {
        Expr name = {0};
        name.kind = E_NAME;
        name.binding = d->captures[i].outer;
        name.type = d->captures[i].outer->type;
        name.name = d->captures[i].outer->name;
        Val x = lowerExpr(L, &name);
        storeSlots(L, env, zero, 8, at, x);
        at += 8 * x.n;
    }
    v.r[1] = env;
    return v;
}

static Expr* stripImplicit(Expr* e) {
    while (e->kind == E_CALL && e->implicit && e->builtin == B_TOARRAY) e = e->args[0];
    return e;
}

static Binding* rootBinding(Expr* e) {
    while (e->kind == E_FIELD || e->kind == E_INDEX) e = e->a;
    return e->kind == E_NAME ? e->binding : NULL;
}

/* A stored array (or map) must not change when the var (or the caller's var) it came from changes. */
static bool needsCopy(Expr* e) {
    if (e->type->kind != TY_ARRAY && e->type->kind != TY_MAP) return false;
    if (e->kind != E_NAME && e->kind != E_FIELD && e->kind != E_INDEX) return false;
    Binding* b = rootBinding(e);
    return b && (b->mutable || b->isParam);
}

static Val copyMap(Lower* L, Val m, int line) { return one(rtcall(L, RT_MAP_COPY, m.r, 1, 'I', line)); }

static Val copyOf(Lower* L, Val v, const Type* t, int line) {
    return t->kind == TY_MAP ? copyMap(L, v, line) : copyArray(L, v, elemBytes(t), line);
}

static Val lowerStored(Lower* L, Expr* e) {
    Val v = lowerExpr(L, e);
    if (needsCopy(e)) return copyOf(L, v, e->type, e->line);
    return v;
}

/* Made just now, owned by nobody else: a var may change it in place. */
static bool isFresh(Expr* e) {
    if (e->kind == E_ARRAY || e->kind == E_MAP || e->kind == E_RANGE) return true;
    if (e->kind != E_CALL || !e->builtin) return false;
    switch (e->builtin) {
    case B_FILL: case B_MAP: case B_FILTER: case B_TOARRAY: case B_SPLIT: case B_ARGS: case B_WORDS: case B_LINES:
    case B_CHARS: case B_SORT: case B_SORTBY: case B_REVERSE: case B_KEYS: case B_VALUES: case B_PARALLELMAP:
        return true;
    default: return false;
    }
}

static bool isJoin(Expr* e) { return e->kind == E_BINARY && e->op == T_PLUS && e->type && e->type->kind == TY_ARRAY; }

/* What a var starts with, or is set to: its own copy, so changing it changes nothing else. */
static Val lowerOwned(Lower* L, Expr* e) {
    Val v = lowerExpr(L, e);
    Type* t = e->type;
    if ((t->kind == TY_ARRAY || t->kind == TY_MAP) && !isFresh(e) && !isJoin(e)) return copyOf(L, v, t, e->line);
    return v;
}

typedef struct ArrayBuild {
    int ptr, len, cap;
    int bytes;
    bool exact;
    int line;
} ArrayBuild;

static void collectSink(Lower* L, Val elem, int index, void* ctx) {
    ArrayBuild* b = ctx;
    if (b->exact) {
        storeSlots(L, b->ptr, index, b->bytes, 0, elem);
        return;
    }
    int full = cmp(L, C_EQ, b->len, b->cap);
    int ok = label(L);
    jz(L, full, ok);
    int newCap = op2(L, IR_ADD, b->cap, b->cap, 'I');
    int args[4] = {b->ptr, b->cap, newCap, iconst(L, b->bytes)};
    int grown = rtcall(L, RT_GROW, args, 4, 'I', b->line);
    mov(L, b->ptr, grown);
    mov(L, b->cap, newCap);
    place(L, ok);
    storeSlots(L, b->ptr, b->len, b->bytes, 0, elem);
    int one1 = iconst(L, 1);
    emit(L, IR_ADD, b->len, b->len, one1);
}

/* Is the chain from e down to its source maps only (so its length is the source's)? */
static Expr* exactSource(Expr* e) {
    e = stripImplicit(e);
    if (e->kind == E_NAME && e->binding && e->binding->lazy) return exactSource(e->binding->init);
    if (e->kind == E_CALL && e->builtin == B_MAP) return exactSource(e->args[0]);
    if (e->kind == E_CALL && (e->builtin == B_FILTER || e->builtin == B_TAKE || e->builtin == B_SKIP || e->builtin == B_ENTRIES)) return NULL;
    return e;
}

static Val materialize(Lower* L, Expr* src, const Type* elem, int line) {
    ArrayBuild b = {0};
    b.bytes = qkTypeSlots(elem) * 8;
    b.line = line;
    Expr* source = exactSource(src);
    Val out = {2, regList(2)};
    if (source && source->kind == E_RANGE) {
        int a = lowerExpr(L, source->a).r[0];
        int bb = lowerExpr(L, source->b).r[0];
        L->preRange = source;
        L->preA = newReg(L, 'I');
        L->preB = newReg(L, 'I');
        mov(L, L->preA, a);
        mov(L, L->preB, bb);
        int n = op2(L, IR_SUB, L->preB, L->preA, 'I');
        int neg = cmp(L, C_LT, n, iconst(L, 0));
        int fine = label(L);
        jz(L, neg, fine);
        mov(L, n, iconst(L, 0));
        place(L, fine);
        b.exact = true;
        b.len = n;
        b.ptr = alloc(L, n, b.bytes, line);
    } else if (source && source->type->kind == TY_ARRAY) {
        Val arr = lowerExpr(L, source);
        L->preArray = source;
        L->preArrayVal = copyVal(L, arr);
        b.exact = true;
        b.len = L->preArrayVal.r[1];
        b.ptr = alloc(L, b.len, b.bytes, line);
    } else {
        b.exact = false;
        b.len = iconst(L, 0);
        b.cap = iconst(L, 8);
        b.ptr = alloc(L, b.cap, b.bytes, line);
    }
    forEach(L, src, collectSink, &b);
    out.r[0] = b.ptr;
    out.r[1] = newReg(L, 'I');
    mov(L, out.r[1], b.len);
    return out;
}

/* ---- consumers ----------------------------------------------------------------------------------- */

typedef struct Acc {
    Val acc;
    Expr* lambda;
    int has;
    bool isMax;
    int line;
} Acc;

static void bindLambda(Expr* lam, Val* vals, int count) {
    for (int i = 0; i < lam->paramCount && i < count; i++) {
        lam->params[i].binding->regs = vals[i].r;
    }
}

static void meanSink(Lower* L, Val elem, int index, void* ctx) {
    (void)index;
    Acc* a = ctx;
    int x = elem.r[0];
    if (L->fn->kinds[x] != 'F') x = op2(L, IR_I2F, x, -1, 'F');
    emit(L, IR_FADD, a->acc.r[0], a->acc.r[0], x);
    emit(L, IR_ADD, a->has, a->has, iconst(L, 1));
}

static void keyArgs(Lower* L, Val k, int* out);
static void mapSet(Lower* L, Val m, Val k, Val v, int line);

/* countBy: one more for the key the lambda gives */
static void countBySink(Lower* L, Val elem, int index, void* ctx) {
    (void)index;
    Acc* a = ctx;
    bindLambda(a->lambda, &elem, 1);
    Val k = copyVal(L, lowerExpr(L, a->lambda->body));
    int args[3] = {a->acc.r[0], 0, 0};
    keyArgs(L, k, args + 1);
    int p = rtcall(L, RT_MAP_GET, args, 3, 'I', a->line);
    int fresh = label(L), done = label(L);
    jz(L, p, fresh);
    int n = newReg(L, 'I');
    IrIns* ld = emit(L, IR_LOAD, n, p, iconst(L, 0));
    ld->imm = 8;
    ld->c = 0;
    emit(L, IR_ADD, n, n, iconst(L, 1));
    storeSlots(L, p, iconst(L, 0), 0, 0, one(n));
    jmp(L, done);
    place(L, fresh);
    mapSet(L, a->acc, k, one(iconst(L, 1)), a->line);
    place(L, done);
}

static void toSetSink(Lower* L, Val elem, int index, void* ctx) {
    (void)index;
    Acc* a = ctx;
    mapSet(L, a->acc, copyVal(L, elem), (Val){0, NULL}, a->line);
}

/* a | b, a & b, a - b on sets: a new set */
typedef struct SetOp {
    Val out;
    Val other;
    int op;
    int line;
} SetOp;

static void setOpEntry(Lower* L, Val key, Val value, void* ctx) {
    (void)value;
    SetOp* s = ctx;
    if (s->op == T_PIPE) {
        mapSet(L, s->out, key, (Val){0, NULL}, s->line);
        return;
    }
    int args[3] = {s->other.r[0], 0, 0};
    keyArgs(L, key, args + 1);
    int p = rtcall(L, RT_MAP_GET, args, 3, 'I', s->line);
    int skip = label(L);
    if (s->op == T_AMP) jz(L, p, skip); /* in both */
    else jnz(L, p, skip);               /* in the first only */
    mapSet(L, s->out, key, (Val){0, NULL}, s->line);
    place(L, skip);
}

static void sumSink(Lower* L, Val elem, int index, void* ctx) {
    (void)index;
    Acc* a = ctx;
    for (int i = 0; i < a->acc.n; i++)
        emit(L, L->fn->kinds[a->acc.r[i]] == 'F' ? IR_FADD : IR_ADD, a->acc.r[i], a->acc.r[i], elem.r[i]);
}

static void countSink(Lower* L, Val elem, int index, void* ctx) {
    (void)elem;
    (void)index;
    Acc* a = ctx;
    emit(L, IR_ADD, a->acc.r[0], a->acc.r[0], iconst(L, 1));
}

static void foldSink(Lower* L, Val elem, int index, void* ctx) {
    (void)index;
    Acc* a = ctx;
    Val args[2] = {a->acc, elem};
    bindLambda(a->lambda, args, 2);
    Val v = lowerExpr(L, a->lambda->body);
    Val fresh = copyVal(L, v); /* the lambda may read acc while giving the new one */
    movVal(L, a->acc, fresh);
}

static void anySink(Lower* L, Val elem, int index, void* ctx) {
    Acc* a = ctx;
    Val args[2] = {elem, one(index)};
    bindLambda(a->lambda, args, 2);
    int t = lowerExpr(L, a->lambda->body).r[0];
    if (a->isMax) { /* all */
        jnz(L, t, L->loopContinue);
        mov(L, a->acc.r[0], iconst(L, 0));
    } else {
        jz(L, t, L->loopContinue);
        mov(L, a->acc.r[0], iconst(L, 1));
    }
    jmp(L, L->loopBreak);
}

static void extremeSink(Lower* L, Val elem, int index, void* ctx) {
    (void)index;
    Acc* a = ctx;
    int compare = label(L), done = label(L);
    jnz(L, a->has, compare);
    mov(L, a->acc.r[0], elem.r[0]);
    mov(L, a->has, iconst(L, 1));
    jmp(L, done);
    place(L, compare);
    if (L->fn->kinds[a->acc.r[0]] == 'F') {
        emit(L, a->isMax ? IR_FMAX : IR_FMIN, a->acc.r[0], a->acc.r[0], elem.r[0]);
    } else {
        int better = cmp(L, a->isMax ? C_GT : C_LT, elem.r[0], a->acc.r[0]);
        jz(L, better, done);
        mov(L, a->acc.r[0], elem.r[0]);
    }
    place(L, done);
}

static Val zeroOf(Lower* L, const Type* t) {
    Val v = valOf(L, t);
    for (int i = 0; i < v.n; i++) mov(L, v.r[i], L->fn->kinds[v.r[i]] == 'F' ? fconst(L, 0) : iconst(L, 0));
    return v;
}

/* ---- loops: sources and the code inside them ----------------------------------------------------- */

typedef struct Through {
    Expr* lambda;
    Sink sink;
    void* ctx;
    int counter; /* filter, take, skip, a map's entries: the index after it */
    int limit;   /* take, skip: how many */
} Through;

static void mapSink(Lower* L, Val elem, int index, void* ctx) {
    Through* t = ctx;
    Val args[2] = {elem, one(index)};
    bindLambda(t->lambda, args, 2);
    Val v = lowerExpr(L, t->lambda->body);
    t->sink(L, v, index, t->ctx);
}

static void filterSink(Lower* L, Val elem, int index, void* ctx) {
    Through* t = ctx;
    Val args[2] = {elem, one(index)};
    bindLambda(t->lambda, args, 2);
    int keep = lowerExpr(L, t->lambda->body).r[0];
    jz(L, keep, L->loopContinue);
    int at = newReg(L, 'I');
    mov(L, at, t->counter);
    emit(L, IR_ADD, t->counter, t->counter, iconst(L, 1));
    t->sink(L, elem, at, t->ctx);
}

/* take(n): the first n go on; then the loop ends (checked after each one, so nothing more is worked out) */
static void takeSink(Lower* L, Val elem, int index, void* ctx) {
    (void)index;
    Through* t = ctx;
    jnz(L, cmp(L, C_GE, t->counter, t->limit), L->loopBreak);
    int at = newReg(L, 'I');
    mov(L, at, t->counter);
    emit(L, IR_ADD, t->counter, t->counter, iconst(L, 1));
    int after = label(L);
    int savedContinue = L->loopContinue;
    L->loopContinue = after; /* a filter after it skips to the check */
    t->sink(L, elem, at, t->ctx);
    L->loopContinue = savedContinue;
    place(L, after);
    jnz(L, cmp(L, C_GE, t->counter, t->limit), L->loopBreak);
}

/* skip(n): the first n are passed over */
static void skipSink(Lower* L, Val elem, int index, void* ctx) {
    (void)index;
    Through* t = ctx;
    int keep = label(L);
    jnz(L, cmp(L, C_GE, t->counter, t->limit), keep);
    emit(L, IR_ADD, t->counter, t->counter, iconst(L, 1));
    jmp(L, L->loopContinue);
    place(L, keep);
    int at = op2(L, IR_SUB, t->counter, t->limit, 'I');
    emit(L, IR_ADD, t->counter, t->counter, iconst(L, 1));
    t->sink(L, elem, at, t->ctx);
}

/* a map's entry as a loop's value: the tuple (key, value), its slots side by side */
static void entryTupleSink(Lower* L, Val key, Val value, void* ctx) {
    Through* t = ctx;
    Val kv = {key.n + value.n, regList(key.n + value.n)};
    for (int i = 0; i < key.n; i++) kv.r[i] = key.r[i];
    for (int i = 0; i < value.n; i++) kv.r[key.n + i] = value.r[i];
    int at = newReg(L, 'I');
    mov(L, at, t->counter);
    emit(L, IR_ADD, t->counter, t->counter, iconst(L, 1));
    t->sink(L, kv, at, t->ctx);
}

static void loopFrame(Lower* L, int* savedBreak, int* savedContinue, int brk, int cont) {
    *savedBreak = L->loopBreak;
    *savedContinue = L->loopContinue;
    L->loopBreak = brk;
    L->loopContinue = cont;
}

static void forEach(Lower* L, Expr* src, Sink sink, void* ctx) {
    src = stripImplicit(src);
    if (src->kind == E_NAME && src->binding && src->binding->lazy) {
        forEach(L, src->binding->init, sink, ctx);
        return;
    }
    if (src->kind == E_CALL && src->builtin == B_MAP) {
        Through t = {src->args[1], sink, ctx, -1};
        forEach(L, src->args[0], mapSink, &t);
        return;
    }
    if (src->kind == E_CALL && src->builtin == B_FILTER) {
        Through t = {src->args[1], sink, ctx, iconst(L, 0), 0};
        forEach(L, src->args[0], filterSink, &t);
        return;
    }
    if (src->kind == E_CALL && (src->builtin == B_TAKE || src->builtin == B_SKIP)) {
        Through t = {NULL, sink, ctx, newReg(L, 'I'), newReg(L, 'I')};
        mov(L, t.counter, iconst(L, 0));
        mov(L, t.limit, lowerExpr(L, src->args[1]).r[0]);
        int fine = label(L); /* fewer than none is none */
        jz(L, cmp(L, C_LT, t.limit, iconst(L, 0)), fine);
        mov(L, t.limit, iconst(L, 0));
        place(L, fine);
        forEach(L, src->args[0], src->builtin == B_TAKE ? takeSink : skipSink, &t);
        return;
    }
    if (src->kind == E_CALL && src->builtin == B_ENTRIES) {
        Through t = {NULL, sink, ctx, newReg(L, 'I'), 0};
        mov(L, t.counter, iconst(L, 0));
        Val m = lowerExpr(L, src->args[0]);
        forEachEntry(L, m, src->args[0]->type, entryTupleSink, &t, src->line);
        return;
    }
    int top = label(L), next = label(L), done = label(L);
    int savedBreak, savedContinue;
    if (src->kind == E_RANGE) {
        int i, end;
        if (L->preRange == src) {
            i = L->preA;
            end = L->preB;
            L->preRange = NULL;
        } else {
            int a = lowerExpr(L, src->a).r[0];
            int b = lowerExpr(L, src->b).r[0];
            i = newReg(L, 'I');
            end = newReg(L, 'I');
            mov(L, i, a);
            mov(L, end, b);
        }
        int index = iconst(L, 0);
        int test = label(L);
        L->depth++;
        jmp(L, test);
        place(L, top);
        fuel(L, src->line);
        loopFrame(L, &savedBreak, &savedContinue, done, next);
        sink(L, one(i), index, ctx);
        L->loopBreak = savedBreak;
        L->loopContinue = savedContinue;
        place(L, next);
        int k1 = iconst(L, 1);
        emit(L, IR_ADD, i, i, k1);
        emit(L, IR_ADD, index, index, k1);
        place(L, test);
        jnz(L, cmp(L, C_LT, i, end), top);
        L->depth--;
        place(L, done);
        return;
    }
    /* an array */
    Val arr;
    if (L->preArray == src) {
        arr = L->preArrayVal;
        L->preArray = NULL;
    } else {
        arr = copyVal(L, lowerExpr(L, src));
    }
    int k = iconst(L, 0);
    int test = label(L);
    L->depth++;
    jmp(L, test);
    place(L, top);
    fuel(L, src->line);
    Val elem = loadElem(L, arr, k, src->type->elem);
    loopFrame(L, &savedBreak, &savedContinue, done, next);
    sink(L, elem, k, ctx);
    L->loopBreak = savedBreak;
    L->loopContinue = savedContinue;
    place(L, next);
    emit(L, IR_ADD, k, k, iconst(L, 1));
    place(L, test);
    jnz(L, cmp(L, C_LT, k, arr.r[1]), top);
    L->depth--;
    place(L, done);
}

/* ---- enums with data ----------------------------------------------------------------------------- */

/* An enum's slots: its case, then each case's data side by side. Where case k's data starts. */
static int caseOffset(const Decl* d, int k) {
    int at = 1;
    for (int i = 0; i < k; i++)
        for (int f = 0; d->caseFieldCount && f < d->caseFieldCount[i]; f++) at += qkFieldSlots(&d->caseFields[i][f]);
    return at;
}

static int fieldOffset(const Decl* d, int k, int field) {
    int at = caseOffset(d, k);
    for (int f = 0; f < field; f++) at += qkFieldSlots(&d->caseFields[k][f]);
    return at;
}

/* Where field f of a record starts. */
static int recordOffset(const Decl* d, int field) {
    int at = 0;
    for (int f = 0; f < field; f++) at += qkFieldSlots(&d->params[f]);
    return at;
}

/* A boxed field's value: its slots copied into the arena, the field keeps the address. */
static Val boxVal(Lower* L, Val v, int line) {
    int ptr = alloc(L, iconst(L, 1), v.n * 8, line);
    storeSlots(L, ptr, iconst(L, 0), 8, 0, v);
    return one(ptr);
}

static Val loadSlots(Lower* L, int p, const Type* t);

/* A field's value from its slots: as they are, or loaded from its box. */
static Val fieldVal(Lower* L, const Param* prm, int* slots) {
    if (prm->boxed) return loadSlots(L, slots[0], prm->binding->type);
    return (Val){qkTypeSlots(prm->binding->type), slots};
}

/* Puts v in a field's slots (boxing it when the field is boxed). */
static void setField(Lower* L, const Param* prm, int* slots, Val v, int line) {
    Val part = {qkFieldSlots(prm), slots};
    movVal(L, part, prm->boxed ? boxVal(L, v, line) : v);
}

/* Case k of enum t, with its data (fields: one value per field, or NULL). The other cases' slots are left
 * as they come: nothing reads them. */
static Val caseValue(Lower* L, Type* t, int k, Val* fields) {
    Val v = valOf(L, t);
    mov(L, v.r[0], iconst(L, k));
    const Decl* d = t->decl;
    for (int f = 0; fields && d->caseFieldCount && f < d->caseFieldCount[k]; f++)
        setField(L, &d->caseFields[k][f], v.r + fieldOffset(d, k, f), fields[f], 0);
    return v;
}

/* ---- text ---------------------------------------------------------------------------------------- */

static Val literalText(Lower* L, const char* s, int n) {
    Val v = {2, regList(2)};
    v.r[0] = dataAddress(L, dataText(L, s, n));
    v.r[1] = iconst(L, n);
    return v;
}

/* What the runtime makes: [length][bytes] at p, as (bytes, length). */
static Val unpackText(Lower* L, int p) {
    Val v = {2, regList(2)};
    v.r[0] = op2(L, IR_ADD, p, iconst(L, 8), 'I');
    v.r[1] = newReg(L, 'I');
    IrIns* ld = emit(L, IR_LOAD, v.r[1], p, iconst(L, 0));
    ld->imm = 8;
    ld->c = 0;
    return v;
}

static Val joinText(Lower* L, Val a, Val b) {
    int args[4] = {a.r[0], a.r[1], b.r[0], b.r[1]};
    return unpackText(L, rtcall(L, RT_TEXT_JOIN, args, 4, 'I', 0));
}

/* The names of an enum's cases, as a table of (offset, length) in the data block; returns its offset. */
static int caseNameTable(Lower* L, const Decl* d) {
    int64_t* table = qkNew(sizeof(int64_t) * 2 * (size_t)d->caseCount);
    for (int k = 0; k < d->caseCount; k++) {
        table[2 * k] = dataText(L, d->cases[k], (int)strlen(d->cases[k]));
        table[2 * k + 1] = (int64_t)strlen(d->cases[k]);
    }
    int at = (int)L->out->data.len;
    qkBufAdd(&L->out->data, table, sizeof(int64_t) * 2 * (size_t)d->caseCount);
    return at;
}

/* Case tag's name as text. */
static Val caseName(Lower* L, const Decl* d, int tag) {
    int tableAt = dataAddress(L, caseNameTable(L, d));
    int base = dataAddress(L, 0);
    int off = newReg(L, 'I');
    IrIns* ld = emit(L, IR_LOAD, off, tableAt, tag);
    ld->imm = 16;
    ld->c = 0;
    Val v = {2, regList(2)};
    v.r[1] = newReg(L, 'I');
    ld = emit(L, IR_LOAD, v.r[1], tableAt, tag);
    ld->imm = 16;
    ld->c = 8;
    v.r[0] = op2(L, IR_ADD, base, off, 'I');
    return v;
}

/* Any value toText takes, as text. */
static Val textOf(Lower* L, Expr* e) {
    Val v = lowerExpr(L, e);
    switch (e->type->kind) {
    case TY_TEXT: return v;
    case TY_INT: return unpackText(L, rtcall(L, RT_TEXT_OF_INT, v.r, 1, 'I', e->line));
    case TY_FLOAT: return unpackText(L, rtcall(L, RT_TEXT_OF_FLOAT, v.r, 1, 'I', e->line));
    case TY_BOOL: {
        Val r = {2, regList(2)};
        r.r[0] = newReg(L, 'I');
        r.r[1] = newReg(L, 'I');
        Val yes = literalText(L, "true", 4), no = literalText(L, "false", 5);
        int other = label(L), done = label(L);
        jz(L, v.r[0], other);
        movVal(L, r, yes);
        jmp(L, done);
        place(L, other);
        movVal(L, r, no);
        place(L, done);
        return r;
    }
    case TY_ENUM: return caseName(L, e->type->decl, v.r[0]);
    default: return literalText(L, "", 0);
    }
}

/* ---- printing ------------------------------------------------------------------------------------ */

static void printVal(Lower* L, Val v, const Type* t);
static void printBoxed(Lower* L, int ptr, const Type* t);
static void printCall(Lower* L, Val v, const Type* t, bool byValue);

typedef struct PrintCtx {
    const Type* elem;
    int first;
} PrintCtx;

static void printElemSink(Lower* L, Val elem, int index, void* ctx) {
    PrintCtx* p = ctx;
    int skip = label(L);
    jz(L, index, skip);
    printLiteral(L, ", ");
    place(L, skip);
    printVal(L, elem, p->elem);
}

typedef struct PrintMap {
    const Type* t;
    int first;
} PrintMap;

static void printEntry(Lower* L, Val key, Val value, void* ctx) {
    PrintMap* pm = ctx;
    int skip = label(L);
    jnz(L, pm->first, skip);
    printLiteral(L, ", ");
    place(L, skip);
    mov(L, pm->first, iconst(L, 0));
    printVal(L, key, pm->t->key);
    if (qkIsSet(pm->t)) return;
    printLiteral(L, ": ");
    printVal(L, value, pm->t->elem);
}

/* A boxed value printed by a function of its own (its type holds itself: printing it inline would never end). */
static void printCall(Lower* L, Val v, const Type* t, bool byValue) {
    int index = -1;
    for (int i = 0; i < L->helperCount; i++)
        if (L->helpers[i].t == t && L->helpers[i].byValue == byValue) index = L->helpers[i].index;
    if (index < 0) {
        index = L->out->count + L->helperCount;
        struct Helper h = {t, index, byValue};
        QK_PUSH(L->helpers, L->helperCount, L->helperCap, h);
    }
    IrIns* ins = emit(L, IR_CALL, -1, -1, -1);
    ins->imm = index;
    ins->args = regList(v.n);
    memcpy(ins->args, v.r, sizeof(int) * (size_t)v.n);
    ins->argCount = v.n;
    for (int i = 0; i < v.n; i++) use(L, v.r[i]);
    if (L->fn->maxOut < v.n) L->fn->maxOut = v.n;
}

static void printBoxed(Lower* L, int ptr, const Type* t) { printCall(L, one(ptr), t, false); }

static void printValInline(Lower* L, Val v, const Type* t);

static void printVal(Lower* L, Val v, const Type* t) {
    if (t->kind == TY_ENUM || t->kind == TY_RECORD) {
        for (int i = 0; i < L->printingCount; i++)
            if (L->printing[i] == t) { /* inside itself (a Json in a Json's list): a call, not more code */
                printCall(L, v, t, true);
                return;
            }
        if (L->printingCount < 64) L->printing[L->printingCount++] = t;
        printValInline(L, v, t);
        L->printingCount--;
        return;
    }
    printValInline(L, v, t);
}

static void printValInline(Lower* L, Val v, const Type* t) {
    switch (t->kind) {
    case TY_INT: rtcall(L, RT_PRINT_INT, v.r, 1, 'V', 0); break;
    case TY_FLOAT: rtcall(L, RT_PRINT_FLOAT, v.r, 1, 'V', 0); break;
    case TY_BOOL: rtcall(L, RT_PRINT_BOOL, v.r, 1, 'V', 0); break;
    case TY_TEXT: rtcall(L, RT_PRINT_TEXT, v.r, 2, 'V', 0); break;
    case TY_HANDLE: { /* Object#12 */
        const char* name = t->decl->name;
        int args[3] = {dataAddress(L, dataText(L, name, (int)strlen(name))), iconst(L, (int64_t)strlen(name)), v.r[0]};
        rtcall(L, RT_PRINT_HANDLE, args, 3, 'V', 0);
        break;
    }
    case TY_MAP: {
        int empty = label(L), done = label(L);
        bool set = qkIsSet(t);
        jz(L, rtcall(L, RT_MAP_LEN, v.r, 1, 'I', 0), empty);
        printLiteral(L, set ? "Set(" : "[");
        PrintMap pm = {t, newReg(L, 'I')};
        mov(L, pm.first, iconst(L, 1));
        forEachEntry(L, v, t, printEntry, &pm, 0);
        printLiteral(L, set ? ")" : "]");
        jmp(L, done);
        place(L, empty);
        printLiteral(L, set ? "Set()" : "[:]");
        place(L, done);
        break;
    }
    case TY_ENUM: {
        Decl* d = t->decl;
        if (d->resultOf) { /* Ok(the value), or Err(the message) */
            int ok = label(L), done = label(L);
            jnz(L, v.r[0], ok);
            printLiteral(L, "Err(");
            Val err = {qkTypeSlots(d->errorOf), v.r + caseOffset(d, 0)};
            printVal(L, err, d->errorOf);
            printLiteral(L, ")");
            jmp(L, done);
            place(L, ok);
            printLiteral(L, "Ok(");
            Val inner = {qkTypeSlots(d->resultOf), v.r + caseOffset(d, 1)};
            printVal(L, inner, d->resultOf);
            printLiteral(L, ")");
            place(L, done);
            break;
        }
        if (d->optionOf) { /* None, or Some(the value) */
            int some = label(L), done = label(L);
            jnz(L, v.r[0], some);
            printLiteral(L, "None");
            jmp(L, done);
            place(L, some);
            printLiteral(L, "Some(");
            Val inner = {qkTypeSlots(d->optionOf), v.r + caseOffset(d, 1)};
            printVal(L, inner, d->optionOf);
            printLiteral(L, ")");
            place(L, done);
            break;
        }
        QkBuf head = {0};
        qkBufPrintf(&head, "%s.", d->templateOf ? d->templateOf->name : d->name); /* Tree.Leaf, of a Tree<Int> too */
        printLiteral(L, head.data);
        qkBufFree(&head);
        Val name = caseName(L, d, v.r[0]);
        rtcall(L, RT_PRINT_TEXT, name.r, 2, 'V', 0);
        for (int k = 0; qkHasData(t) && k < d->caseCount; k++) { /* the case's data: (radius: 2.0) */
            if (!d->caseFieldCount[k]) continue;
            int skip = label(L);
            jz(L, cmp(L, C_EQ, v.r[0], iconst(L, k)), skip);
            for (int f = 0; f < d->caseFieldCount[k]; f++) {
                Type* ft = d->caseFields[k][f].binding->type;
                QkBuf field = {0};
                qkBufPrintf(&field, "%s%s: ", f ? ", " : "(", d->caseFields[k][f].name);
                printLiteral(L, field.data);
                qkBufFree(&field);
                const Param* prm = &d->caseFields[k][f];
                if (prm->boxed) printBoxed(L, v.r[fieldOffset(d, k, f)], ft);
                else printVal(L, (Val){qkTypeSlots(ft), v.r + fieldOffset(d, k, f)}, ft);
            }
            printLiteral(L, ")");
            place(L, skip);
        }
        break;
    }
    case TY_RECORD: {
        Decl* d = t->decl;
        QkBuf head = {0};
        qkBufPrintf(&head, "%s(", d->isTuple ? "" : d->isEntry ? "Entry" : d->templateOf ? d->templateOf->name : d->name);
        printLiteral(L, head.data);
        qkBufFree(&head);
        int at = 0;
        for (int f = 0; f < d->paramCount; f++) {
            Type* ft = d->params[f].binding->type;
            QkBuf name = {0};
            if (d->isTuple) qkBufPuts(&name, f ? ", " : ""); /* (one, 1) */
            else qkBufPrintf(&name, "%s%s: ", f ? ", " : "", d->params[f].name);
            printLiteral(L, name.data);
            qkBufFree(&name);
            if (d->params[f].boxed) printBoxed(L, v.r[at], ft);
            else printVal(L, (Val){qkTypeSlots(ft), v.r + at}, ft);
            at += qkFieldSlots(&d->params[f]);
        }
        printLiteral(L, ")");
        break;
    }
    case TY_ARRAY: {
        printLiteral(L, "[");
        Val arr = copyVal(L, v);
        int k = iconst(L, 0);
        int top = label(L), done = label(L);
        place(L, top);
        jnz(L, cmp(L, C_GE, k, arr.r[1]), done);
        PrintCtx p = {t->elem, 0};
        printElemSink(L, loadElem(L, arr, k, t->elem), k, &p);
        emit(L, IR_ADD, k, k, iconst(L, 1));
        jmp(L, top);
        place(L, done);
        printLiteral(L, "]");
        break;
    }
    default: break;
    }
}

/* ---- the newer built-ins: helpers ---------------------------------------------------------------- */

/* A loop or an array, as an array. */
static Val arrayOf(Lower* L, Expr* src, int line) {
    Expr* s = stripImplicit(src);
    if (s->type->kind == TY_ARRAY && !(s->kind == E_NAME && s->binding && s->binding->lazy)) return lowerExpr(L, s);
    return materialize(L, src, s->type->elem, line);
}

/* A pointer the runtime gave to [count][element slots...]: the array (address, length). */
static Val unpackArray(Lower* L, int p) { return unpackText(L, p); }

/* Slots the runtime left at p: an optional or a result ([case][the data...]). */
static Val loadSlots(Lower* L, int p, const Type* t) {
    Val r = valOf(L, t);
    for (int i = 0; i < r.n; i++) {
        IrIns* ld = emit(L, IR_LOAD, r.r[i], p, iconst(L, 0));
        ld->imm = 8;
        ld->c = 8 * i;
    }
    return r;
}

static Val callDecl(Lower* L, Decl* d, Val* args, int count, int line);

/* a < b: numbers, texts, Bools, cases (in their order), or through the type's compare. */
static int lessThan(Lower* L, Val a, Val b, const Type* t, int line) {
    switch (t->kind) {
    case TY_TEXT: {
        int args[4] = {a.r[0], a.r[1], b.r[0], b.r[1]};
        return cmp(L, C_LT, rtcall(L, RT_TEXT_COMPARE, args, 4, 'I', line), iconst(L, 0));
    }
    case TY_RECORD: case TY_ENUM: case TY_HANDLE: {
        QkBuf name = {0};
        qkBufPrintf(&name, "%s.compare", t->decl->name);
        Decl* d = qkFindDecl(L->prog, name.data);
        qkBufFree(&name);
        if (d) {
            Val args[2] = {a, b};
            return cmp(L, C_LT, callDecl(L, d, args, 2, line).r[0], iconst(L, 0));
        }
        return cmp(L, C_LT, a.r[0], b.r[0]);
    }
    default: return cmp(L, C_LT, a.r[0], b.r[0]);
    }
}

/* What a sort compares: the element itself, or what the lambda gives for it. */
static Val sortKey(Lower* L, Expr* lambda, Val elem) {
    if (!lambda) return elem;
    bindLambda(lambda, &elem, 1);
    return copyVal(L, lowerExpr(L, lambda->body));
}

/* A sorted copy: a bottom-up merge sort, stable (equal keys keep their order). */
static Val lowerSort(Lower* L, Val arr, const Type* elem, Expr* lambda, const Type* keyType, int descending, int line) {
    int bytes = qkTypeSlots(elem) * 8;
    int n = newReg(L, 'I');
    mov(L, n, arr.r[1]);
    Val first = copyArray(L, arr, bytes, line);
    int src = newReg(L, 'I'), dst = newReg(L, 'I');
    mov(L, src, first.r[0]);
    mov(L, dst, alloc(L, n, bytes, line));
    int width = newReg(L, 'I');
    mov(L, width, iconst(L, 1));
    int outerTop = label(L), outerDone = label(L);
    L->depth++;
    place(L, outerTop);
    jnz(L, cmp(L, C_GE, width, n), outerDone);
    int i = newReg(L, 'I');
    mov(L, i, iconst(L, 0));
    int runTop = label(L), runDone = label(L);
    L->depth++;
    place(L, runTop);
    jnz(L, cmp(L, C_GE, i, n), runDone);
    int mid = newReg(L, 'I'), hi = newReg(L, 'I');
    mov(L, mid, op2(L, IR_ADD, i, width, 'I'));
    int midOk = label(L);
    jz(L, cmp(L, C_GT, mid, n), midOk);
    mov(L, mid, n);
    place(L, midOk);
    mov(L, hi, op2(L, IR_ADD, mid, width, 'I'));
    int hiOk = label(L);
    jz(L, cmp(L, C_GT, hi, n), hiOk);
    mov(L, hi, n);
    place(L, hiOk);
    int l = newReg(L, 'I'), r = newReg(L, 'I'), k = newReg(L, 'I');
    mov(L, l, i);
    mov(L, r, mid);
    mov(L, k, i);
    int mergeTop = label(L), mergeDone = label(L), takeRight = label(L), stored = label(L);
    L->depth++;
    place(L, mergeTop);
    jnz(L, cmp(L, C_GE, k, hi), mergeDone);
    int takeLeft = label(L);
    jnz(L, cmp(L, C_GE, l, mid), takeRight); /* the left run is used up */
    jnz(L, cmp(L, C_GE, r, hi), takeLeft);   /* the right one is */
    {
        Val srcArr = {2, regList(2)};
        srcArr.r[0] = src;
        srcArr.r[1] = n;
        Val left = loadElem(L, srcArr, l, elem);
        Val right = loadElem(L, srcArr, r, elem);
        Val kl = sortKey(L, lambda, left);
        Val kr = sortKey(L, lambda, right);
        if (descending >= 0) { /* descending: left < right puts the right one first (equal keys keep their order) */
            int ascending = label(L);
            jz(L, descending, ascending);
            jnz(L, lessThan(L, kl, kr, keyType, line), takeRight);
            jmp(L, takeLeft);
            place(L, ascending);
        }
        jnz(L, lessThan(L, kr, kl, keyType, line), takeRight); /* right < left: right first; else left (stable) */
    }
    place(L, takeLeft);
    {
        Val srcArr = {2, regList(2)};
        srcArr.r[0] = src;
        srcArr.r[1] = n;
        storeSlots(L, dst, k, bytes, 0, loadElem(L, srcArr, l, elem));
        emit(L, IR_ADD, l, l, iconst(L, 1));
        jmp(L, stored);
    }
    place(L, takeRight);
    {
        Val srcArr = {2, regList(2)};
        srcArr.r[0] = src;
        srcArr.r[1] = n;
        storeSlots(L, dst, k, bytes, 0, loadElem(L, srcArr, r, elem));
        emit(L, IR_ADD, r, r, iconst(L, 1));
    }
    place(L, stored);
    emit(L, IR_ADD, k, k, iconst(L, 1));
    jmp(L, mergeTop);
    L->depth--;
    place(L, mergeDone);
    emit(L, IR_ADD, i, i, op2(L, IR_ADD, width, width, 'I'));
    jmp(L, runTop);
    L->depth--;
    place(L, runDone);
    int t = newReg(L, 'I'); /* the merged runs are the next pass's source */
    mov(L, t, src);
    mov(L, src, dst);
    mov(L, dst, t);
    emit(L, IR_ADD, width, width, width);
    jmp(L, outerTop);
    L->depth--;
    place(L, outerDone);
    Val out = {2, regList(2)};
    out.r[0] = src;
    out.r[1] = n;
    return out;
}

typedef struct Search {
    Val result;   /* what the search gives */
    Val needle;   /* contains / indexOf: the value looked for */
    const Type* elem;
    Expr* lambda; /* first / last / count / minBy / maxBy: the test, or the key */
    Val bestKey;  /* minBy / maxBy */
    int has;
    int builtin;
    int line;
    const Type* keyType;
} Search;

static void searchSink(Lower* L, Val elem, int index, void* ctx) {
    Search* s = ctx;
    switch (s->builtin) {
    case B_CONTAINS: case B_INDEXOF: {
        int same = equalVals(L, elem, s->needle, s->elem, s->line);
        jz(L, same, L->loopContinue);
        mov(L, s->result.r[0], iconst(L, 1));
        if (s->builtin == B_INDEXOF) mov(L, s->result.r[1], index);
        jmp(L, L->loopBreak);
        break;
    }
    case B_FIRST: case B_LAST: { /* the first (last) one, or the first (last) one the lambda takes */
        if (s->lambda) {
            Val args[1] = {elem};
            bindLambda(s->lambda, args, 1);
            jz(L, lowerExpr(L, s->lambda->body).r[0], L->loopContinue);
        }
        mov(L, s->result.r[0], iconst(L, 1));
        Val inner = {s->result.n - 1, s->result.r + 1};
        movVal(L, inner, elem);
        if (s->builtin == B_FIRST) jmp(L, L->loopBreak);
        break;
    }
    case B_COUNTOF: {
        Val args[1] = {elem};
        bindLambda(s->lambda, args, 1);
        jz(L, lowerExpr(L, s->lambda->body).r[0], L->loopContinue);
        emit(L, IR_ADD, s->result.r[0], s->result.r[0], iconst(L, 1));
        break;
    }
    default: { /* minBy, maxBy */
        Val key = sortKey(L, s->lambda, elem);
        int take = label(L), done = label(L);
        jz(L, s->has, take);
        int better = s->builtin == B_MAXBY ? lessThan(L, s->bestKey, key, s->keyType, s->line) : lessThan(L, key, s->bestKey, s->keyType, s->line);
        jz(L, better, done);
        place(L, take);
        movVal(L, s->result, elem);
        movVal(L, s->bestKey, key);
        mov(L, s->has, iconst(L, 1));
        place(L, done);
    }
    }
}

/* ---- maps: a runtime table, one slot (its address); a var's map is its own, so it changes in place ---- */

static int mapKeySlots(const Type* m) { return qkTypeSlots(m->key); }

static void keyArgs(Lower* L, Val k, int* out) {
    out[0] = k.r[0];
    out[1] = k.n > 1 ? k.r[1] : iconst(L, 0);
}

static Val newMap(Lower* L, const Type* t, int line) {
    int kind = t->key->kind == TY_TEXT ? 1 : qkTypeSlots(t->key) == 2 ? 2 : 0; /* text, two slots, one */
    int args[2] = {iconst(L, kind), iconst(L, qkTypeSlots(t->elem) * 8)};
    return one(rtcall(L, RT_MAP_NEW, args, 2, 'I', line));
}

/* Where the value of key k goes (added if new): the caller stores it there. */
static void mapSet(Lower* L, Val m, Val k, Val v, int line) {
    int args[3] = {m.r[0], 0, 0};
    keyArgs(L, k, args + 1);
    int p = rtcall(L, RT_MAP_SET, args, 3, 'I', line);
    storeSlots(L, p, iconst(L, 0), 0, 0, v);
}

/* m[k]: the value, or None */
static Val mapGet(Lower* L, Val m, Val k, const Type* option, int line) {
    int args[3] = {m.r[0], 0, 0};
    keyArgs(L, k, args + 1);
    int p = rtcall(L, RT_MAP_GET, args, 3, 'I', line);
    Val r = valOf(L, option);
    mov(L, r.r[0], cmp(L, C_NE, p, iconst(L, 0)));
    int none = label(L);
    jz(L, r.r[0], none);
    for (int i = 1; i < r.n; i++) {
        IrIns* ld = emit(L, IR_LOAD, r.r[i], p, iconst(L, 0));
        ld->imm = 8;
        ld->c = 8 * (i - 1);
    }
    place(L, none);
    return r;
}

/* Every entry of a map, in the order the keys came: body(key, value) */
static void forEachEntry(Lower* L, Val m, const Type* t, EntryFn body, void* ctx, int line) {
    int map = newReg(L, 'I');
    mov(L, map, m.r[0]);
    int n = rtcall(L, RT_MAP_USED, &map, 1, 'I', line);
    int k = newReg(L, 'I');
    mov(L, k, iconst(L, 0));
    int top = label(L), next = label(L), done = label(L), test = label(L);
    L->depth++;
    jmp(L, test);
    place(L, top);
    fuel(L, line);
    int args[2] = {map, k};
    int p = rtcall(L, RT_MAP_ENTRY, args, 2, 'I', line);
    jz(L, p, next); /* a removed key */
    int ks = mapKeySlots(t);
    Val key = valOf(L, t->key);
    for (int i = 0; i < key.n && i < ks; i++) {
        IrIns* ld = emit(L, IR_LOAD, key.r[i], p, iconst(L, 0));
        ld->imm = 8;
        ld->c = 8 * i;
    }
    Val value = valOf(L, t->elem);
    for (int i = 0; i < value.n; i++) {
        IrIns* ld = emit(L, IR_LOAD, value.r[i], p, iconst(L, 0));
        ld->imm = 8;
        ld->c = 16 + 8 * i;
    }
    int savedBreak, savedContinue;
    savedBreak = L->loopBreak;
    savedContinue = L->loopContinue;
    L->loopBreak = done;
    L->loopContinue = next;
    body(L, key, value, ctx);
    L->loopBreak = savedBreak;
    L->loopContinue = savedContinue;
    place(L, next);
    emit(L, IR_ADD, k, k, iconst(L, 1));
    place(L, test);
    jnz(L, cmp(L, C_LT, k, n), top);
    L->depth--;
    place(L, done);
}

/* ---- calls --------------------------------------------------------------------------------------- */

static Val arrayOf(Lower* L, Expr* src, int line);

/* parallelMap(xs, f): the runtime shares the elements out to threads, each calling f */
static Val lowerParallelMap(Lower* L, Expr* e) {
    Val arr = copyVal(L, arrayOf(L, e->args[0], e->line));
    Val f = lowerExpr(L, e->args[1]);
    Type* ft = e->args[1]->type;
    int args[7] = {f.r[0], f.r[1], arr.r[0], arr.r[1], iconst(L, qkTypeSlots(ft->params[0])), iconst(L, qkTypeSlots(ft->elem)),
                   iconst(L, e->line)};
    Val r = {2, regList(2)};
    r.r[0] = rtcall(L, RT_PARALLEL_MAP, args, 7, 'I', e->line);
    r.r[1] = arr.r[1];
    return r;
}

static Val fnAddress(Lower* L, Decl* d);
static int recordOffset(const Decl* d, int field);

/* A Float made an Int: one that is too big (or not a number) stops the program, never a wrong Int. The check is
 * the backend's: one compare after the conversion, on the value it gives for those. */
static int floatToInt(Lower* L, int x, int line) {
    int r = newReg(L, 'I');
    IrIns* in = emit(L, IR_F2I, r, x, -1);
    in->cond = 1;
    in->line = line;
    return r;
}

static Val lowerBuiltin(Lower* L, Expr* e) {
    if (e->builtin == B_PARALLELMAP) return lowerParallelMap(L, e);
    if (e->builtin == B_INTERFACE) { /* the value, then a closure over it for each of the interface's functions */
        e->binding->regs = copyVal(L, lowerExpr(L, e->args[0])).r;
        Val r = valOf(L, e->type);
        int env = -1; /* one environment for all of them: they all hold just the value (one allocation, not one each) */
        for (int f = 1; f < e->argCount; f++) {
            Decl* fd = e->args[f]->decl;
            bool shared = env >= 0 && fd->captureCount == 1 && fd->captures[0].outer == e->binding;
            Val fv;
            if (shared) {
                fv = (Val){2, regList(2)};
                fv.r[0] = newReg(L, 'I');
                IrIns* ins = emit(L, IR_CONST, fv.r[0], -1, -1);
                ins->imm = L->fnIndex[fd->index];
                ins->c = 2;
                fv.r[1] = env;
            } else {
                fv = fnAddress(L, fd);
                if (fd->captureCount == 1 && fd->captures[0].outer == e->binding) env = fv.r[1];
            }
            Val part = {2, r.r + recordOffset(e->type->decl, f - 1)};
            movVal(L, part, fv);
        }
        return r;
    }
    Expr** a = e->args;
    switch (e->builtin) {
    case B_OK: {
        Val v = lowerStored(L, a[0]);
        return caseValue(L, e->type, 1, &v);
    }
    case B_ERR: {
        Val m = lowerExpr(L, a[0]);
        return caseValue(L, e->type, 0, &m);
    }
    case B_WORDS: case B_LINES: case B_CHARS: case B_UPPER: case B_LOWER: {
        Val t = lowerExpr(L, a[0]);
        RtId id = e->builtin == B_WORDS ? RT_WORDS : e->builtin == B_LINES ? RT_LINES : e->builtin == B_CHARS ? RT_CHARS
                : e->builtin == B_UPPER ? RT_UPPER : RT_LOWER;
        int p = rtcall(L, id, t.r, 2, 'I', e->line);
        return unpackText(L, p); /* a text, or [count][texts]: the same shape */
    }
    case B_STARTSWITH: case B_ENDSWITH: {
        Val t = lowerExpr(L, a[0]), s = lowerExpr(L, a[1]);
        int args[4] = {t.r[0], t.r[1], s.r[0], s.r[1]};
        return one(rtcall(L, e->builtin == B_STARTSWITH ? RT_STARTS_WITH : RT_ENDS_WITH, args, 4, 'I', e->line));
    }
    case B_REPLACE: {
        Val t = lowerExpr(L, a[0]), o = lowerExpr(L, a[1]), n = lowerExpr(L, a[2]);
        int args[6] = {t.r[0], t.r[1], o.r[0], o.r[1], n.r[0], n.r[1]};
        return unpackText(L, rtcall(L, RT_REPLACE, args, 6, 'I', e->line));
    }
    case B_REPEAT: {
        Val t = lowerExpr(L, a[0]);
        int args[3] = {t.r[0], t.r[1], lowerExpr(L, a[1]).r[0]};
        return unpackText(L, rtcall(L, RT_REPEAT, args, 3, 'I', e->line));
    }
    case B_JOIN: {
        Val arr = arrayOf(L, a[0], e->line);
        Val sep = lowerExpr(L, a[1]);
        int args[4] = {arr.r[0], arr.r[1], sep.r[0], sep.r[1]};
        return unpackText(L, rtcall(L, RT_JOIN, args, 4, 'I', e->line));
    }
    case B_CONTAINS: case B_INDEXOF: {
        Type* t = stripImplicit(a[0])->type;
        if (t->kind == TY_TEXT) {
            Val h = lowerExpr(L, a[0]), n = lowerExpr(L, a[1]);
            int args[4] = {h.r[0], h.r[1], n.r[0], n.r[1]};
            int at = rtcall(L, RT_FIND, args, 4, 'I', e->line);
            int found = cmp(L, C_GE, at, iconst(L, 0));
            if (e->builtin == B_CONTAINS) return one(found);
            Val r = valOf(L, e->type);
            mov(L, r.r[0], found);
            mov(L, r.r[1], at);
            return r;
        }
        if (t->kind == TY_MAP) {
            Val m = lowerExpr(L, a[0]), k = lowerExpr(L, a[1]);
            int args[3] = {m.r[0], 0, 0};
            keyArgs(L, k, args + 1);
            return one(cmp(L, C_NE, rtcall(L, RT_MAP_GET, args, 3, 'I', e->line), iconst(L, 0)));
        }
        Search s = {0};
        s.builtin = e->builtin;
        s.elem = qkElemType(t);
        s.line = e->line;
        s.needle = copyVal(L, lowerExpr(L, a[1]));
        s.result = e->builtin == B_CONTAINS ? one(iconst(L, 0)) : valOf(L, e->type);
        mov(L, s.result.r[0], iconst(L, 0));
        forEach(L, a[0], searchSink, &s);
        return s.result;
    }
    case B_FIRST: case B_LAST: case B_COUNTOF: case B_MINBY: case B_MAXBY: {
        Expr* src = a[0];
        Type* elem = qkElemType(stripImplicit(src)->type);
        if (e->builtin == B_LAST && e->argCount == 1) { /* the array's last element */
            Val arr = lowerExpr(L, src);
            Val r = valOf(L, e->type);
            mov(L, r.r[0], cmp(L, C_GT, arr.r[1], iconst(L, 0)));
            int none = label(L);
            jz(L, r.r[0], none);
            Val last = loadElem(L, arr, op2(L, IR_SUB, arr.r[1], iconst(L, 1), 'I'), elem);
            Val inner = {r.n - 1, r.r + 1};
            movVal(L, inner, last);
            place(L, none);
            return r;
        }
        Search s = {0};
        s.builtin = e->builtin;
        s.elem = elem;
        s.line = e->line;
        s.lambda = e->argCount > 1 ? a[1] : NULL;
        if (e->builtin == B_COUNTOF) {
            s.result = one(newReg(L, 'I'));
            mov(L, s.result.r[0], iconst(L, 0));
        } else if (e->builtin == B_FIRST || e->builtin == B_LAST) {
            s.result = valOf(L, e->type);
            mov(L, s.result.r[0], iconst(L, 0));
        } else {
            s.result = valOf(L, elem);
            s.keyType = s.lambda->type;
            s.bestKey = valOf(L, s.keyType);
            s.has = newReg(L, 'I');
            mov(L, s.has, iconst(L, 0));
            for (int i = 0; i < s.result.n; i++) mov(L, s.result.r[i], L->fn->kinds[s.result.r[i]] == 'F' ? fconst(L, 0) : iconst(L, 0));
            for (int i = 0; i < s.bestKey.n; i++) mov(L, s.bestKey.r[i], L->fn->kinds[s.bestKey.r[i]] == 'F' ? fconst(L, 0) : iconst(L, 0));
        }
        forEach(L, src, searchSink, &s);
        if (e->builtin == B_MINBY || e->builtin == B_MAXBY) bounds(L, iconst(L, 0), s.has, PANIC_EMPTY, e->line);
        return s.result;
    }
    case B_SORT: case B_SORTBY: {
        Val arr = lowerExpr(L, a[0]);
        Type* elem = e->type->elem;
        Expr* lambda = e->builtin == B_SORTBY ? a[1] : NULL;
        int base = e->builtin == B_SORTBY ? 2 : 1;
        int descending = e->argCount > base ? copyVal(L, lowerExpr(L, a[base])).r[0] : -1;
        return lowerSort(L, arr, elem, lambda, lambda ? lambda->type : elem, descending, e->line);
    }
    case B_REVERSE: {
        Val arr = copyVal(L, lowerExpr(L, a[0]));
        int bytes = elemBytes(e->type);
        int ptr = alloc(L, arr.r[1], bytes, e->line);
        int k = newReg(L, 'I');
        mov(L, k, iconst(L, 0));
        int top = label(L), done = label(L);
        L->depth++;
        place(L, top);
        jnz(L, cmp(L, C_GE, k, arr.r[1]), done);
        int from = op2(L, IR_SUB, op2(L, IR_SUB, arr.r[1], k, 'I'), iconst(L, 1), 'I');
        storeSlots(L, ptr, k, bytes, 0, loadElem(L, arr, from, e->type->elem));
        emit(L, IR_ADD, k, k, iconst(L, 1));
        jmp(L, top);
        L->depth--;
        place(L, done);
        Val r = {2, regList(2)};
        r.r[0] = ptr;
        r.r[1] = arr.r[1];
        return r;
    }
    case B_KEYS: case B_VALUES: {
        Val m = lowerExpr(L, a[0]);
        return unpackArray(L, rtcall(L, e->builtin == B_KEYS ? RT_MAP_KEYS : RT_MAP_VALUES, m.r, 1, 'I', e->line));
    }
    case B_ADD: { /* s.add(v): in place, s is the var's own */
        Binding* b = a[0]->binding;
        mapSet(L, one(b->regs[0]), lowerExpr(L, a[1]), (Val){0, NULL}, e->line);
        return (Val){0, NULL};
    }
    case B_TOSET: {
        Acc acc = {0};
        acc.acc = newMap(L, e->type, e->line);
        acc.line = e->line;
        forEach(L, a[0], toSetSink, &acc);
        return acc.acc;
    }
    case B_REMOVE: {
        Binding* b = a[0]->binding;
        Val k = lowerExpr(L, a[1]);
        int args[3] = {b->regs[0], 0, 0};
        keyArgs(L, k, args + 1);
        rtcall(L, RT_MAP_REMOVE, args, 3, 'V', e->line);
        return (Val){0, NULL};
    }
    case B_INDEX: return one(lowerExpr(L, a[0]).r[0]);
    case B_PRINTERR: {
        rtcall(L, RT_PRINT_TO, (int[]){iconst(L, 2)}, 1, 'V', e->line);
        for (int i = 0; i < e->argCount; i++) {
            if (i) rtcall(L, RT_PRINT_SEP, NULL, 0, 'V', 0);
            printVal(L, lowerExpr(L, a[i]), a[i]->type);
        }
        rtcall(L, RT_PRINT_END, NULL, 0, 'V', 0);
        rtcall(L, RT_PRINT_TO, (int[]){iconst(L, 1)}, 1, 'V', e->line);
        return (Val){0, NULL};
    }
    case B_EXIT: {
        int code = lowerExpr(L, a[0]).r[0];
        rtcall(L, RT_EXIT, &code, 1, 'V', e->line);
        return (Val){0, NULL};
    }
    case B_ENV: case B_LISTDIR: {
        Val t = lowerExpr(L, a[0]);
        return loadSlots(L, rtcall(L, e->builtin == B_ENV ? RT_ENV : RT_LIST_DIR, t.r, 2, 'I', e->line), e->type);
    }
    case B_FILEEXISTS: return one(rtcall(L, RT_FILE_EXISTS, lowerExpr(L, a[0]).r, 2, 'I', e->line));
    case B_TIME: return one(rtcall(L, RT_TIME, NULL, 0, 'F', e->line));
    case B_ASSERT: {
        int cond = lowerExpr(L, a[0]).r[0];
        int ok = label(L);
        jnz(L, cond, ok);
        Val why = e->argCount > 1 ? lowerExpr(L, a[1]) : literalText(L, "assert failed", 13);
        int args[3] = {why.r[0], why.r[1], iconst(L, e->line | (int64_t)L->inl << 32)};
        rtcall(L, RT_FAIL, args, 3, 'V', e->line);
        place(L, ok);
        return (Val){0, NULL};
    }
    case B_SQRT: return one(op2(L, IR_SQRT, lowerExpr(L, a[0]).r[0], -1, 'F'));
    case B_SIN: case B_COS: case B_TAN: case B_ASIN: case B_ACOS: case B_ATAN: case B_EXP: case B_LOG: {
        static const RtId ids[] = {RT_SIN, RT_COS, RT_TAN, RT_ASIN, RT_ACOS, RT_ATAN, 0, RT_EXP, RT_LOG};
        int x = lowerExpr(L, a[0]).r[0];
        return one(rtcall(L, ids[e->builtin - B_SIN], &x, 1, 'F', e->line));
    }
    case B_ATAN2: case B_POW: {
        int args[2] = {lowerExpr(L, a[0]).r[0], lowerExpr(L, a[1]).r[0]};
        return one(rtcall(L, e->builtin == B_POW ? RT_POW : RT_ATAN2, args, 2, 'F', e->line));
    }
    case B_ABS: {
        int x = lowerExpr(L, a[0]).r[0];
        if (L->fn->kinds[x] == 'F') return one(op2(L, IR_FMAX, x, op2(L, IR_FNEG, x, -1, 'F'), 'F'));
        int r = newReg(L, 'I');
        mov(L, r, x);
        int fine = label(L);
        jz(L, cmp(L, C_LT, x, iconst(L, 0)), fine);
        emit(L, IR_NEG, r, x, -1);
        place(L, fine);
        return one(r);
    }
    case B_FLOOR: case B_CEIL: case B_ROUND: {
        int x = lowerExpr(L, a[0]).r[0];
        if (L->fn->kinds[x] == 'I') return one(x);
        if (e->builtin == B_ROUND) x = op2(L, IR_FADD, x, fconst(L, 0.5), 'F');
        int f = newReg(L, 'F');
        emit(L, IR_ROUND, f, x, -1)->imm = e->builtin == B_CEIL ? 2 : 1;
        return one(floatToInt(L, f, e->line));
    }
    case B_TOFLOAT: {
        int x = lowerExpr(L, a[0]).r[0];
        return one(L->fn->kinds[x] == 'F' ? x : op2(L, IR_I2F, x, -1, 'F'));
    }
    case B_TOINT: {
        int x = lowerExpr(L, a[0]).r[0];
        return one(L->fn->kinds[x] == 'I' ? x : floatToInt(L, x, e->line));
    }
    case B_MIN: case B_MAX: {
        bool isMax = e->builtin == B_MAX;
        if (e->argCount == 1) {
            Acc acc = {0};
            acc.acc = valOf(L, e->type);
            acc.has = iconst(L, 0);
            acc.isMax = isMax;
            mov(L, acc.acc.r[0], L->fn->kinds[acc.acc.r[0]] == 'F' ? fconst(L, 0) : iconst(L, 0));
            forEach(L, a[0], extremeSink, &acc);
            bounds(L, iconst(L, 0), acc.has, PANIC_EMPTY, e->line); /* 0 < has, else empty */
            return acc.acc;
        }
        int x = lowerExpr(L, a[0]).r[0];
        int y = lowerExpr(L, a[1]).r[0];
        if (L->fn->kinds[x] == 'F') return one(op2(L, isMax ? IR_FMAX : IR_FMIN, x, y, 'F'));
        int r = newReg(L, 'I');
        mov(L, r, x);
        int fine = label(L);
        jz(L, cmp(L, isMax ? C_GT : C_LT, y, x), fine);
        mov(L, r, y);
        place(L, fine);
        return one(r);
    }
    case B_CLAMP: {
        int v = lowerExpr(L, a[0]).r[0];
        int lo = lowerExpr(L, a[1]).r[0];
        int hi = lowerExpr(L, a[2]).r[0];
        if (L->fn->kinds[v] == 'F') return one(op2(L, IR_FMAX, lo, op2(L, IR_FMIN, v, hi, 'F'), 'F'));
        int r = newReg(L, 'I');
        mov(L, r, v);
        int notHigh = label(L), done = label(L);
        jz(L, cmp(L, C_GT, r, hi), notHigh);
        mov(L, r, hi);
        place(L, notHigh);
        jz(L, cmp(L, C_LT, r, lo), done);
        mov(L, r, lo);
        place(L, done);
        return one(r);
    }
    case B_LERP: {
        int x = lowerExpr(L, a[0]).r[0];
        int y = lowerExpr(L, a[1]).r[0];
        int t = lowerExpr(L, a[2]).r[0];
        return one(op2(L, IR_FADD, x, op2(L, IR_FMUL, op2(L, IR_FSUB, y, x, 'F'), t, 'F'), 'F'));
    }
    case B_LEN: {
        Expr* src = stripImplicit(a[0]);
        if (src->type->kind == TY_TEXT) return one(lowerExpr(L, src).r[1]);
        if (src->type->kind == TY_MAP) return one(rtcall(L, RT_MAP_LEN, lowerExpr(L, src).r, 1, 'I', e->line));
        if (src->type->kind == TY_ARRAY && !(src->kind == E_NAME && src->binding && src->binding->lazy))
            return one(lowerExpr(L, src).r[1]);
        Acc acc = {0};
        acc.acc = one(iconst(L, 0));
        forEach(L, a[0], countSink, &acc);
        return acc.acc;
    }
    case B_FILL: {
        int n = lowerExpr(L, a[0]).r[0];
        Val v = lowerExpr(L, a[1]);
        int bytes = v.n * 8;
        int count = newReg(L, 'I');
        mov(L, count, n);
        int ptr = alloc(L, count, bytes, e->line);
        int k = iconst(L, 0);
        int top = label(L), done = label(L);
        L->depth++;
        place(L, top);
        jnz(L, cmp(L, C_GE, k, count), done);
        storeSlots(L, ptr, k, bytes, 0, v);
        emit(L, IR_ADD, k, k, iconst(L, 1));
        jmp(L, top);
        L->depth--;
        place(L, done);
        Val r = {2, regList(2)};
        r.r[0] = ptr;
        r.r[1] = count;
        return r;
    }
    case B_MAP: case B_FILTER: case B_TOARRAY:
        return materialize(L, e->builtin == B_TOARRAY ? a[0] : e, e->type->kind == TY_ARRAY ? e->type->elem : e->type->elem, e->line);
    case B_MEAN: {
        Acc acc = {0};
        acc.acc = one(newReg(L, 'F'));
        mov(L, acc.acc.r[0], fconst(L, 0));
        acc.has = newReg(L, 'I');
        mov(L, acc.has, iconst(L, 0));
        forEach(L, a[0], meanSink, &acc);
        bounds(L, iconst(L, 0), acc.has, PANIC_EMPTY, e->line); /* the mean of nothing */
        return one(op2(L, IR_FDIV, acc.acc.r[0], op2(L, IR_I2F, acc.has, -1, 'F'), 'F'));
    }
    case B_COUNTBY: {
        Acc acc = {0};
        acc.acc = newMap(L, e->type, e->line);
        acc.lambda = a[1];
        acc.line = e->line;
        forEach(L, a[0], countBySink, &acc);
        return acc.acc;
    }
    case B_GET: { /* the value at i, or None: one unsigned compare for both ends */
        Val arr = copyVal(L, lowerExpr(L, a[0]));
        int i = lowerExpr(L, a[1]).r[0];
        Val r = valOf(L, e->type);
        for (int k = 0; k < r.n; k++) mov(L, r.r[k], L->fn->kinds[r.r[k]] == 'F' ? fconst(L, 0) : iconst(L, 0));
        int none = label(L);
        jz(L, cmp(L, C_LT, i, arr.r[1]), none);
        jnz(L, cmp(L, C_LT, i, iconst(L, 0)), none);
        Val v = loadElem(L, arr, i, e->type->decl->optionOf);
        mov(L, r.r[0], iconst(L, 1));
        movVal(L, (Val){v.n, r.r + 1}, v);
        place(L, none);
        return r;
    }
    case B_PANIC: {
        Val why = lowerExpr(L, a[0]);
        int args[3] = {why.r[0], why.r[1], iconst(L, e->line | (int64_t)L->inl << 32)};
        rtcall(L, RT_FAIL, args, 3, 'V', e->line);
        return (Val){0, NULL};
    }
    case B_NEVER: /* it never comes back: a value of the type for the code that follows, never used */
        lowerExpr(L, a[0]);
        return zeroOf(L, e->type);
    case B_TAKE: case B_SKIP: case B_ENTRIES: /* a loop where an array goes is made one through toArray */
        return materialize(L, e, e->type->elem, e->line);
    case B_SUM: {
        Acc acc = {0};
        acc.acc = zeroOf(L, e->type);
        forEach(L, a[0], sumSink, &acc);
        return acc.acc;
    }
    case B_FOLD: {
        Acc acc = {0};
        Val init = lowerExpr(L, a[1]);
        acc.acc = valOf(L, e->type);
        movVal(L, acc.acc, init);
        acc.lambda = a[2];
        forEach(L, a[0], foldSink, &acc);
        return acc.acc;
    }
    case B_ANY: case B_ALL: {
        Acc acc = {0};
        acc.acc = one(iconst(L, e->builtin == B_ALL));
        acc.lambda = a[1];
        acc.isMax = e->builtin == B_ALL;
        forEach(L, a[0], anySink, &acc);
        return acc.acc;
    }
    case B_PRINT:
        for (int i = 0; i < e->argCount; i++) {
            if (i) rtcall(L, RT_PRINT_SEP, NULL, 0, 'V', 0);
            Val v = lowerExpr(L, a[i]);
            printVal(L, v, a[i]->type);
        }
        rtcall(L, RT_PRINT_END, NULL, 0, 'V', 0);
        return (Val){0, NULL};
    case B_NOW: return one(rtcall(L, RT_NOW, NULL, 0, 'F', e->line));
    case B_RANDOM: return one(rtcall(L, RT_RANDOM, NULL, 0, 'F', e->line));
    case B_SPAWN: {
        Val f = lowerExpr(L, a[0]);
        int args[2] = {f.r[0], f.r[1]};
        return one(rtcall(L, RT_SPAWN, args, 2, 'I', e->line));
    }
    case B_STEP: {
        int args[2] = {lowerExpr(L, a[0]).r[0], iconst(L, e->line)};
        return one(rtcall(L, RT_STEP, args, 2, 'I', e->line));
    }
    case B_YIELD:
        rtcall(L, RT_YIELD, NULL, 0, 'V', e->line);
        return (Val){0, NULL};
    case B_WAIT: {
        int x = lowerExpr(L, a[0]).r[0];
        rtcall(L, RT_WAIT, &x, 1, 'V', e->line);
        return (Val){0, NULL};
    }
    case B_RANDOMINT: {
        int args[3] = {lowerExpr(L, a[0]).r[0], lowerExpr(L, a[1]).r[0], iconst(L, e->line)};
        return one(rtcall(L, RT_RANDOM_INT, args, 3, 'I', e->line));
    }
    case B_SEEDRANDOM: {
        int x = lowerExpr(L, a[0]).r[0];
        rtcall(L, RT_SEED_RANDOM, &x, 1, 'V', e->line);
        return (Val){0, NULL};
    }
    case B_READFILE: case B_READLINE: case B_PARSEINT: case B_PARSEFLOAT: { /* an optional or a result: its slots */
        int args[2] = {0, 0};
        int n = 0;
        if (e->builtin != B_READLINE) {
            Val t = lowerExpr(L, a[0]);
            args[0] = t.r[0];
            args[1] = t.r[1];
            n = 2;
        }
        RtId id = e->builtin == B_READFILE ? RT_READ_FILE : e->builtin == B_READLINE ? RT_READ_LINE
                : e->builtin == B_PARSEINT ? RT_PARSE_INT : RT_PARSE_FLOAT;
        int p = rtcall(L, id, args, n, 'I', e->line);
        Val r = valOf(L, e->type);
        for (int i = 0; i < r.n; i++) {
            IrIns* ld = emit(L, IR_LOAD, r.r[i], p, iconst(L, 0));
            ld->imm = 8;
            ld->c = 8 * i;
        }
        return r;
    }
    case B_WRITEFILE: {
        Val path = lowerExpr(L, a[0]);
        Val text = lowerExpr(L, a[1]);
        int args[4] = {path.r[0], path.r[1], text.r[0], text.r[1]};
        return one(rtcall(L, RT_WRITE_FILE, args, 4, 'I', e->line));
    }
    case B_TRIM: {
        Val t = lowerExpr(L, a[0]);
        int args[2] = {t.r[0], t.r[1]};
        return unpackText(L, rtcall(L, RT_TRIM, args, 2, 'I', e->line));
    }
    case B_ARGS: case B_SPLIT: { /* an array of texts, from [count][(bytes, length)...] */
        int p;
        if (e->builtin == B_ARGS) {
            p = rtcall(L, RT_ARGS, NULL, 0, 'I', e->line);
        } else {
            Val t = lowerExpr(L, a[0]);
            Val sep = lowerExpr(L, a[1]);
            int args[4] = {t.r[0], t.r[1], sep.r[0], sep.r[1]};
            p = rtcall(L, RT_SPLIT, args, 4, 'I', e->line);
        }
        return unpackText(L, p); /* the same shape: the count, then what follows it */
    }
    case B_SOME: {
        Val v = lowerStored(L, a[0]);
        return caseValue(L, e->type, 1, &v);
    }
    case B_TOTEXT: return textOf(L, a[0]);
    case B_PUSH: { /* xs.push(v): room for one more, v at the end, one longer */
        Binding* b = a[0]->binding;
        Val v = lowerStored(L, a[1]);
        Val arr = {2, b->regs};
        int bytes = qkTypeSlots(a[0]->type->elem) * 8;
        int args[3] = {arr.r[0], arr.r[1], iconst(L, bytes)};
        int ptr = rtcall(L, RT_PUSH, args, 3, 'I', e->line);
        mov(L, arr.r[0], ptr);
        storeSlots(L, arr.r[0], arr.r[1], bytes, 0, v);
        emit(L, IR_ADD, arr.r[1], arr.r[1], iconst(L, 1));
        return (Val){0, NULL};
    }
    }
    return (Val){0, NULL};
}

/* How big a function's body is, in nodes: small ones are inlined. */
static int blockSize(const Block* b);
static int exprSize(const Expr* e) {
    if (!e) return 0;
    int n = 1 + exprSize(e->a) + exprSize(e->b) + exprSize(e->body) + exprSize(e->elseExpr);
    for (int i = 0; i < e->argCount; i++) n += exprSize(e->args[i]);
    for (int i = 0; i < e->armCount; i++) n += exprSize(e->arms[i].body);
    if (e->thenBlock) n += blockSize(e->thenBlock);
    if (e->block) n += blockSize(e->block);
    return n;
}
static int blockSize(const Block* b) {
    int n = 0;
    for (int i = 0; i < b->count; i++) {
        const Stmt* s = b->stmts[i];
        n += 1 + exprSize(s->value) + exprSize(s->target) + exprSize(s->iter) + (s->body ? blockSize(s->body) : 0);
    }
    return n;
}

/* A call to the host: its C function, one argument per slot. */
static Val hostCall(Lower* L, Expr* e, Decl* d, Val* args, int count, int total) {
    int index = L->externIndex[d->index];
    if (index < 0) { /* the first call: the host function joins the program's list */
        index = L->externIndex[d->index] = L->out->externCount++;
        IrExtern* x = &L->out->externs[index];
        x->name = d->name;
        x->argCount = 0;
        for (int i = 0; i < d->paramCount; i++) x->argCount += qkTypeSlots(d->params[i].binding->type);
        x->argKinds = qkNew((size_t)x->argCount + 1);
        for (int i = 0, at = 0; i < d->paramCount; i++) {
            qkTypeSlotKinds(d->params[i].binding->type, x->argKinds + at);
            at += qkTypeSlots(d->params[i].binding->type);
        }
        x->retCount = qkTypeSlots(d->retType);
        x->retKinds = qkNew((size_t)x->retCount + 1);
        qkTypeSlotKinds(d->retType, x->retKinds);
    }
    IrIns* ins = emit(L, IR_HOSTCALL, -1, -1, -1);
    ins->imm = index;
    ins->line = e->line;
    ins->args = regList(total);
    ins->argCount = total;
    for (int i = 0, k = 0; i < count; i++)
        for (int j = 0; j < args[i].n; j++) ins->args[k++] = args[i].r[j];
    Val r = valOf(L, d->retType);
    ins = &L->fn->code[L->fn->count - 1];
    ins->rets = r.r;
    ins->retCount = r.n;
    for (int i = 0; i < ins->argCount; i++) use(L, ins->args[i]);
    for (int i = 0; i < r.n; i++) use(L, r.r[i]);
    /* the outgoing area: the shadow space, the arguments (a hidden pointer first when several results come
     * back), then room for those results */
    int hidden = r.n > 1 ? 1 : 0;
    int slots = (total + hidden > 4 ? total + hidden : 4) + (r.n > 1 ? r.n : 0);
    if (slots > L->fn->maxOut) L->fn->maxOut = slots;
    return r;
}

/* A call with its values lowered already: inlined when small, else a call. */
static Val callDecl(Lower* L, Decl* d, Val* args, int count, int line) {
    int total = 0;
    for (int i = 0; i < count; i++) total += args[i].n;
    IrIns* ins = emit(L, IR_CALL, -1, -1, -1);
    ins->imm = L->fnIndex[d->index];
    ins->line = line;
    ins->args = regList(total);
    ins->argCount = total;
    for (int i = 0, k = 0; i < count; i++)
        for (int j = 0; j < args[i].n; j++) ins->args[k++] = args[i].r[j];
    Val r = valOf(L, d->retType);
    ins = &L->fn->code[L->fn->count - 1];
    ins->rets = r.r;
    ins->retCount = r.n;
    for (int i = 0; i < ins->argCount; i++) use(L, ins->args[i]);
    for (int i = 0; i < r.n; i++) use(L, r.r[i]);
    int slots = total > r.n ? total : r.n;
    if (slots > L->fn->maxOut) L->fn->maxOut = slots;
    return r;
}

static Val lowerCall(Lower* L, Expr* e) {
    if (e->builtin) return lowerBuiltin(L, e);
    if ((e->binding && e->binding->type->kind == TY_FN && !e->binding->lambda) || e->callee) { /* a function kept as a value */
        Val f = copyVal(L, lowerExpr(L, e->callee ? e->callee : &(Expr){.kind = E_NAME, .binding = e->binding, .type = e->binding->type}));
        int addr = f.r[0];
        Type* ft = e->callee ? e->callee->type : e->binding->type;
        int total = 1; /* the captured values' address, after the arguments */
        Val* args = qkNew(sizeof(Val) * (size_t)(e->orderedCount + 1));
        for (int i = 0; i < e->orderedCount; i++) {
            args[i] = lowerExpr(L, e->ordered[i]);
            total += args[i].n;
        }
        args[e->orderedCount] = one(f.r[1]);
        IrIns* ins = emit(L, IR_CALL, -1, addr, -1);
        ins->imm = -1;
        ins->line = e->line;
        ins->args = regList(total);
        ins->argCount = total;
        for (int i = 0, k = 0; i <= e->orderedCount; i++)
            for (int j = 0; j < args[i].n; j++) ins->args[k++] = args[i].r[j];
        Val r = valOf(L, ft->elem);
        ins = &L->fn->code[L->fn->count - 1];
        ins->rets = r.r;
        ins->retCount = r.n;
        for (int i = 0; i < ins->argCount; i++) use(L, ins->args[i]);
        for (int i = 0; i < r.n; i++) use(L, r.r[i]);
        int slots = total > r.n ? total : r.n;
        if (slots > L->fn->maxOut) L->fn->maxOut = slots;
        return r;
    }
    if (e->binding && e->binding->type->kind == TY_FN) { /* f(x), f a function this one was given: its lambda, here */
        Expr* lam = e->binding->lambda;
        Val* vals = qkNew(sizeof(Val) * (size_t)(e->argCount ? e->argCount : 1));
        for (int i = 0; i < e->argCount; i++) vals[i] = copyVal(L, lowerExpr(L, e->args[i]));
        bindLambda(lam, vals, e->argCount);
        return copyVal(L, lowerExpr(L, lam->body));
    }
    Decl* d = e->decl;
    if (d->kind == D_TYPE) {
        Val r = valOf(L, d->type);
        for (int f = 0; f < d->paramCount; f++) /* the fields in order (bound by the checker) */
            setField(L, &d->params[f], r.r + recordOffset(d, f), lowerStored(L, e->ordered[f]), e->line);
        return r;
    }
    if (d->kind == D_ENUM) { /* Shape.Circle(2): a case with its data */
        int n = d->caseFieldCount[e->caseIndex];
        Val* fields = qkNew(sizeof(Val) * (size_t)(n ? n : 1));
        for (int i = 0; i < n; i++) fields[i] = lowerStored(L, e->ordered[i]);
        return caseValue(L, d->type, e->caseIndex, fields);
    }
    /* a function: arguments as a flat list of slots, one value per parameter (defaults filled in) */
    int total = 0;
    int count = e->ordered ? e->orderedCount : e->argCount;
    Expr** values = e->ordered ? e->ordered : e->args;
    Val* args = qkNew(sizeof(Val) * (size_t)(count ? count : 1));
    for (int i = 0; i < count; i++) {
        args[i] = lowerExpr(L, values[i]);
        total += args[i].n;
    }
    if (d->isExtern) return hostCall(L, e, d, args, count, total);
    /* a small function is inlined: no call at all. Parameters never change, so they can be the arguments' own
     * registers; a return becomes a jump to the end. Not into itself (recursion stays a call). */
    bool inside = false;
    for (int i = 0; i < L->inlineDepth; i++) inside |= L->inlining[i].d == d;
    bool small = !qkDebug && !inside && L->inlineDepth < 8 && d != L->current && exprSize(d->body) <= 120; /* debug: real calls */
    if (small || (d->inlineOnly && L->inlineDepth < 32)) {
        int** saved = qkNew(sizeof(int*) * (size_t)(d->paramCount ? d->paramCount : 1));
        for (int i = 0; i < d->paramCount; i++) {
            saved[i] = d->params[i].binding->regs;
            d->params[i].binding->regs = args[i].r;
        }
        int savedInl = L->inl; /* where a panic in it says it came from */
        InlineCall call = {savedInl, d->templateOf ? d->templateOf->name : d->name, e->line};
        if (!L->out->inlineCount) QK_PUSH(L->out->inlines, L->out->inlineCount, L->out->inlineCap, call);
        L->inl = L->out->inlineCount;
        QK_PUSH(L->out->inlines, L->out->inlineCount, L->out->inlineCap, call);
        InlineFrame* fr = &L->inlining[L->inlineDepth++];
        fr->d = d;
        fr->end = label(L);
        fr->result = valOf(L, d->retType);
        int savedBreak = L->userBreak, savedContinue = L->userContinue;
        Val v = d->body->kind == E_BLOCK ? lowerBlock(L, d->body->block, d->retType) : lowerStored(L, d->body);
        if (v.n) movVal(L, fr->result, v);
        place(L, fr->end);
        Val r = fr->result;
        L->userBreak = savedBreak;
        L->userContinue = savedContinue;
        L->inlineDepth--;
        L->inl = savedInl;
        for (int i = 0; i < d->paramCount; i++) d->params[i].binding->regs = saved[i];
        return r;
    }
    IrIns* ins = emit(L, IR_CALL, -1, -1, -1);
    ins->imm = L->fnIndex[d->index];
    ins->line = e->line;
    ins->args = regList(total);
    ins->argCount = total;
    for (int i = 0, k = 0; i < count; i++)
        for (int j = 0; j < args[i].n; j++) ins->args[k++] = args[i].r[j];
    Val r = valOf(L, d->retType);
    ins = &L->fn->code[L->fn->count - 1];
    ins->rets = r.r;
    ins->retCount = r.n;
    for (int i = 0; i < ins->argCount; i++) use(L, ins->args[i]);
    for (int i = 0; i < r.n; i++) use(L, r.r[i]);
    int slots = total > r.n ? total : r.n;
    if (slots > L->fn->maxOut) L->fn->maxOut = slots;
    return r;
}

static Val lowerCall(Lower* L, Expr* e);

/* a == b: 1 or 0; records field by field, texts byte by byte. */
static int equalVals(Lower* L, Val a, Val b, const Type* t, int line) {
    if (t->kind == TY_TEXT) {
        int args[4] = {a.r[0], a.r[1], b.r[0], b.r[1]};
        return cmp(L, C_EQ, rtcall(L, RT_TEXT_COMPARE, args, 4, 'I', line), iconst(L, 0));
    }
    if (t->kind == TY_ENUM && qkHasData(t)) { /* the same case, and the same data */
        const Decl* d = t->decl;
        int all = newReg(L, 'I');
        mov(L, all, cmp(L, C_EQ, a.r[0], b.r[0]));
        int done = label(L);
        jz(L, all, done);
        for (int k = 0; k < d->caseCount; k++) {
            if (!d->caseFieldCount[k]) continue;
            int skip = label(L);
            jz(L, cmp(L, C_EQ, a.r[0], iconst(L, k)), skip);
            for (int f = 0; f < d->caseFieldCount[k]; f++) {
                const Type* ft = d->caseFields[k][f].binding->type;
                int at = fieldOffset(d, k, f), n = qkTypeSlots(ft);
                emit(L, IR_MUL, all, all, equalVals(L, (Val){n, a.r + at}, (Val){n, b.r + at}, ft, line));
            }
            place(L, skip);
        }
        place(L, done);
        return all;
    }
    if (t->kind != TY_RECORD) return cmp(L, C_EQ, a.r[0], b.r[0]);
    int all = newReg(L, 'I');
    mov(L, all, iconst(L, 1));
    for (int f = 0, at = 0; f < t->decl->paramCount; f++) {
        const Type* ft = t->decl->params[f].binding->type;
        int n = qkTypeSlots(ft);
        Val fa = {n, a.r + at}, fb = {n, b.r + at};
        emit(L, IR_MUL, all, all, equalVals(L, fa, fb, ft, line));
        at += n;
    }
    return all;
}

static Val lowerBinary(Lower* L, Expr* e) {
    if (e->decl && !(e->op >= T_EQ && e->op <= T_GE)) return lowerCall(L, e); /* an operator of a type: its function */
    if (e->op == T_AND || e->op == T_OR) {
        int r = newReg(L, 'I');
        mov(L, r, lowerExpr(L, e->a).r[0]);
        int done = label(L);
        if (e->op == T_AND) jz(L, r, done);
        else jnz(L, r, done);
        mov(L, r, lowerExpr(L, e->b).r[0]);
        place(L, done);
        return one(r);
    }
    if (e->decl && e->op >= T_EQ && e->op <= T_GE) { /* a < b through the type's compare */
        static const int conds[] = {C_EQ, C_NE, C_LT, C_LE, C_GT, C_GE};
        int order = lowerCall(L, e).r[0];
        return one(cmp(L, conds[e->op - T_EQ], order, iconst(L, 0)));
    }
    if (e->op == T_QQ) { /* what the optional holds, or the right side (worked out only then) */
        Val opt = copyVal(L, lowerExpr(L, e->a));
        Decl* d = e->a->type->decl;
        Val r = valOf(L, e->type);
        int none = label(L), done = label(L);
        jz(L, opt.r[0], none);
        if (e->type == e->a->type) movVal(L, r, opt); /* a ?? b with b optional too */
        else {
            Val inner = {qkTypeSlots(qkFallibleValue(e->a->type)), opt.r + caseOffset(d, 1)};
            movVal(L, r, inner);
        }
        jmp(L, done);
        place(L, none);
        movVal(L, r, lowerStored(L, e->b));
        place(L, done);
        return r;
    }
    Val a = lowerExpr(L, e->a);
    Val b = lowerExpr(L, e->b);
    Type* ta = e->a->type;

    if (ta->kind == TY_RECORD || (ta->kind == TY_ENUM && qkHasData(ta))) { /* == and != */
        int same = equalVals(L, a, b, ta, e->line);
        return one(e->op == T_EQ ? same : op2(L, IR_NOT, same, -1, 'I'));
    }
    if (ta->kind == TY_ARRAY && e->op == T_PLUS) { /* xs + ys: a new array */
        int args[5] = {a.r[0], a.r[1], b.r[0], b.r[1], iconst(L, elemBytes(ta))};
        Val r = {2, regList(2)};
        r.r[0] = rtcall(L, RT_ARRAY_JOIN, args, 5, 'I', e->line);
        r.r[1] = op2(L, IR_ADD, a.r[1], b.r[1], 'I');
        return r;
    }
    if (ta->kind == TY_TEXT) { /* joined, or compared byte by byte */
        if (e->op == T_PLUS) return joinText(L, a, b);
        static const int conds[] = {C_EQ, C_NE, C_LT, C_LE, C_GT, C_GE};
        int args[4] = {a.r[0], a.r[1], b.r[0], b.r[1]};
        int order = rtcall(L, RT_TEXT_COMPARE, args, 4, 'I', e->line);
        return one(cmp(L, conds[e->op - T_EQ], order, iconst(L, 0)));
    }
    if (qkIsSet(ta)) { /* union: a's values then b's; intersection and difference: a's, kept or not */
        SetOp so = {e->op == T_PIPE ? copyMap(L, a, e->line) : newMap(L, ta, e->line), b, e->op, e->line};
        forEachEntry(L, e->op == T_PIPE ? b : a, ta, setOpEntry, &so, e->line);
        return so.out;
    }
    switch (e->op) {
    case T_AMP: case T_PIPE: case T_CARET: case T_SHL: case T_SHR: {
        IrOp op = e->op == T_AMP ? IR_AND : e->op == T_PIPE ? IR_OR : e->op == T_CARET ? IR_XOR : e->op == T_SHL ? IR_SHL : IR_SAR;
        return one(op2(L, op, a.r[0], b.r[0], 'I'));
    }
    case T_PLUS: case T_MINUS: case T_STAR: case T_SLASH: case T_PERCENT: {
        bool f = e->type->kind == TY_FLOAT;
        IrOp op;
        switch (e->op) {
        case T_PLUS: op = f ? IR_FADD : IR_ADD; break;
        case T_MINUS: op = f ? IR_FSUB : IR_SUB; break;
        case T_STAR: op = f ? IR_FMUL : IR_MUL; break;
        case T_SLASH: op = f ? IR_FDIV : IR_DIV; break;
        default: op = IR_MOD;
        }
        int r = newReg(L, f ? 'F' : 'I');
        IrIns* in = emit(L, op, r, a.r[0], b.r[0]);
        in->line = e->line;
        if (qkChecked && (op == IR_ADD || op == IR_SUB || op == IR_MUL)) in->cond = 1; /* past the limits: a panic */
        return one(r);
    }
    default: {
        static const int conds[] = {C_EQ, C_NE, C_LT, C_LE, C_GT, C_GE};
        int cond = conds[e->op - T_EQ];
        return one(cmp(L, cond, a.r[0], b.r[0]));
    }
    }
}

static Val lowerIf(Lower* L, Expr* e) {
    bool value = e->type->kind != TY_VOID && e->type->kind != TY_ERROR;
    Val r = value ? valOf(L, e->type) : (Val){0, NULL};
    int elseL = label(L), done = label(L);
    jz(L, lowerExpr(L, e->a).r[0], elseL);
    Val t = lowerBlock(L, e->thenBlock, value ? e->type : &qkTypeVoid);
    if (value) movVal(L, r, t);
    jmp(L, done);
    place(L, elseL);
    if (e->elseExpr) {
        Val f = e->elseExpr->kind == E_BLOCK ? lowerBlock(L, e->elseExpr->block, value ? e->type : &qkTypeVoid) : lowerExpr(L, e->elseExpr);
        if (value) movVal(L, r, f);
    }
    place(L, done);
    return r;
}

/* Tests a pattern on v (of type t), jumping to fail when it does not match; its names get their slots. */
static void matchPattern(Lower* L, Arm* a, Val v, const Type* t, int fail, bool checkCase) {
    if (a->textPattern) {
        Val lit = literalText(L, a->textPattern->text, a->textPattern->textLen);
        int args[4] = {v.r[0], v.r[1], lit.r[0], lit.r[1]};
        jnz(L, rtcall(L, RT_TEXT_COMPARE, args, 4, 'I', a->textPattern->line), fail);
        return;
    }
    bool isF = t->kind == TY_FLOAT; /* Box(w, 1): an Int pattern on a Float */
    if (a->isRange) {
        if (a->intPattern) jz(L, cmp(L, C_GE, v.r[0], isF ? fconst(L, (double)a->intPattern->i) : iconst(L, a->intPattern->i)), fail);
        if (a->intHigh) jz(L, cmp(L, C_LT, v.r[0], isF ? fconst(L, (double)a->intHigh->i) : iconst(L, a->intHigh->i)), fail);
        return;
    }
    if (a->intPattern) {
        jz(L, cmp(L, C_EQ, v.r[0], isF ? fconst(L, (double)a->intPattern->i) : iconst(L, a->intPattern->i)), fail);
        return;
    }
    if (!a->pattern) return;
    const Decl* d = t->decl;
    bool record = t->kind == TY_RECORD;
    if (!record && checkCase && d->caseCount > 1) jz(L, cmp(L, C_EQ, v.r[0], iconst(L, a->caseIndex)), fail);
    for (int f = 0; f < a->bindCount; f++) {
        const Param* prm = record ? &d->params[f] : &d->caseFields[a->caseIndex][f];
        int at = record ? recordOffset(d, f) : fieldOffset(d, a->caseIndex, f);
        bool sub = a->subs && a->subs[f];
        if (!sub && !(a->bindings && a->bindings[f])) continue;
        Val fv = fieldVal(L, prm, v.r + at);
        if (sub) matchPattern(L, a->subs[f], fv, prm->binding->type, fail, true);
        else a->bindings[f]->regs = fv.r;
    }
}

static void traceValue(Lower* L, const Binding* b, int line);

static Val lowerMatch(Lower* L, Expr* e) {
    bool value = e->type->kind != TY_VOID && e->type->kind != TY_ERROR;
    Val r = value ? valOf(L, e->type) : (Val){0, NULL};
    Val whole = copyVal(L, lowerExpr(L, e->a)); /* its case, and its data for the arms' names */
    int subject = whole.r[0];
    const Decl* ed = e->a->type->kind == TY_ENUM ? e->a->type->decl : NULL;
    int done = label(L);
    for (int i = 0; i < e->armCount; i++) {
        Arm* a = &e->arms[i];
        int next = label(L);
        bool last = i == e->armCount - 1;
        (void)ed;
        (void)subject;
        bool exhaustiveLast = last && !a->guard && !a->subs; /* checked: every case has an arm without if */
        matchPattern(L, a, whole, e->a->type, next, !exhaustiveLast);
        for (int f = 0; qkDebug && a->bindings && f < a->bindCount; f++) traceValue(L, a->bindings[f], 0); /* the arm's names */
        if (a->guard) jz(L, lowerExpr(L, a->guard).r[0], next);
        Expr* body = a->body;
        Val v = body->kind == E_BLOCK ? lowerBlock(L, body->block, value ? e->type : &qkTypeVoid) : lowerExpr(L, body);
        if (value) movVal(L, r, v);
        jmp(L, done);
        place(L, next);
    }
    place(L, done);
    return r;
}

/* A constant: the value worked out when the program was compiled (an array of numbers or Bools is kept in the
 * program's data once, and never allocated: what uses it copies it before changing it), or its expression. */
static Val lowerConst(Lower* L, Decl* d) {
    Expr* v = d->folded ? d->folded : d->body;
    bool plain = v->kind == E_ARRAY && v->argCount && qkTypeSlots(v->type->elem) == 1;
    for (int i = 0; plain && i < v->argCount; i++) plain = v->args[i]->kind == E_INT || v->args[i]->kind == E_FLOAT || v->args[i]->kind == E_BOOL;
    if (!plain || !d->folded) return lowerExpr(L, v);
    if (d->dataIn != L->out) { /* once per program */
        QkBuf bytes = {0};
        for (int i = 0; i < v->argCount; i++) {
            int64_t slot = v->args[i]->i;
            if (v->args[i]->kind == E_FLOAT) memcpy(&slot, &v->args[i]->f, 8);
            qkBufAdd(&bytes, (const char*)&slot, 8);
        }
        d->dataAt = dataText(L, bytes.data, (int)bytes.len);
        d->dataIn = L->out;
        qkBufFree(&bytes);
    }
    Val r = {2, regList(2)};
    r.r[0] = dataAddress(L, d->dataAt);
    r.r[1] = iconst(L, v->argCount);
    return r;
}

static Val lowerExpr(Lower* L, Expr* e) {
    switch (e->kind) {
    case E_INT: return one(iconst(L, e->i));
    case E_FLOAT: return one(fconst(L, e->f));
    case E_BOOL: return one(iconst(L, e->i));
    case E_CASE: return qkHasData(e->type) ? caseValue(L, e->type, e->caseIndex, NULL) : one(iconst(L, e->caseIndex));
    case E_WITH: { /* a copy, some fields changed */
        Val base = lowerExpr(L, e->a);
        Val r = copyVal(L, base);
        Type* t = e->a->type;
        for (int i = 0; i < e->argCount; i++) {
            Val v = lowerStored(L, e->args[i]);
            int f = 0;
            while (f < t->decl->paramCount - 1 && strcmp(t->decl->params[f].name, e->argNames[i])) f++;
            setField(L, &t->decl->params[f], r.r + recordOffset(t->decl, f), v, e->line);
        }
        return r;
    }
    case E_INTERP: { /* the pieces and the values as text, joined */
        Val acc = literalText(L, e->parts[0], (int)strlen(e->parts[0]));
        bool empty = !e->parts[0][0];
        for (int i = 0; i < e->argCount; i++) {
            const char* f = e->formats ? e->formats[i] : NULL;
            int width = f ? atoi(f) : 0;
            const char* dot = f ? strchr(f, '.') : NULL;
            Val t;
            if (dot) { /* {x:.2}: so many decimals */
                int x = lowerExpr(L, e->args[i]).r[0];
                if (L->fn->kinds[x] == 'I') x = op2(L, IR_I2F, x, -1, 'F');
                int args[2] = {x, iconst(L, atoi(dot + 1))};
                t = unpackText(L, rtcall(L, RT_TEXT_FIXED, args, 2, 'I', e->line));
            } else {
                t = textOf(L, e->args[i]);
            }
            if (width > 0) { /* {x:8}: at least so wide, spaces in front */
                int args[3] = {t.r[0], t.r[1], iconst(L, width)};
                t = unpackText(L, rtcall(L, RT_PAD, args, 3, 'I', e->line));
            }
            acc = empty ? t : joinText(L, acc, t);
            empty = false;
            if (e->parts[i + 1][0]) acc = joinText(L, acc, literalText(L, e->parts[i + 1], (int)strlen(e->parts[i + 1])));
        }
        return acc;
    }
    case E_TEXT: {
        Val v = {2, regList(2)};
        v.r[0] = dataAddress(L, dataText(L, e->text, e->textLen));
        v.r[1] = iconst(L, e->textLen);
        return v;
    }
    case E_TRY: { /* what it holds, or leave the function with its None / Err (main and tests stop) */
        Val whole = copyVal(L, lowerExpr(L, e->a));
        Decl* d = e->a->type->decl;
        int ok = label(L);
        jnz(L, whole.r[0], ok);
        Decl* fn = L->inlineDepth ? L->inlining[L->inlineDepth - 1].d : L->current;
        Val msg = d->resultOf ? (Val){qkTypeSlots(d->errorOf), whole.r + caseOffset(d, 0)} : literalText(L, "no value (None)", 15);
        if (!strcmp(fn->name, "main") || fn->isTest) {
            if (d->resultOf && d->errorOf->kind != TY_TEXT) { /* main stops with the error's text: a case's name, a number */
                Expr held = {0};
                held.kind = E_NAME;
                held.type = d->errorOf;
                Binding tmp = {0};
                tmp.type = d->errorOf;
                tmp.regs = msg.r;
                held.binding = &tmp;
                held.name = "error";
                bool textable = d->errorOf->kind == TY_INT || d->errorOf->kind == TY_FLOAT || d->errorOf->kind == TY_BOOL ||
                                d->errorOf->kind == TY_ENUM;
                msg = textable ? textOf(L, &held) : literalText(L, "an error", 8);
            }
            int args[3] = {msg.r[0], msg.r[1], iconst(L, e->line | (int64_t)L->inl << 32)};
            rtcall(L, RT_FAIL, args, 3, 'V', e->line);
        } else {
            lowerReturn(L, caseValue(L, fn->retType, 0, d->resultOf ? &msg : NULL));
        }
        place(L, ok);
        Val inner = {qkTypeSlots(e->type), whole.r + caseOffset(d, 1)};
        return inner;
    }
    case E_MAP: {
        Val m = newMap(L, e->type, e->line);
        for (int i = 0; i + 1 < e->argCount; i += 2) {
            Val k = lowerExpr(L, e->args[i]);
            Val v = lowerStored(L, e->args[i + 1]);
            mapSet(L, m, k, v, e->line);
        }
        return m;
    }
    case E_NAME: {
        Binding* b = e->binding;
        if (!b) {
            if (e->decl && e->decl->kind == D_CONST) return lowerConst(L, e->decl); /* a constant: its value */
            if (e->decl && e->decl->kind == D_FN) return fnAddress(L, e->decl);           /* a function as a value */
            return caseValue(L, e->type, 0, NULL);                                        /* None */
        }
        if (b->lazy) return materialize(L, b->init, b->type->elem, e->line);
        Val v = {qkTypeSlots(b->type), b->regs};
        return v;
    }
    case E_FIELD: {
        Val v = lowerExpr(L, e->a);
        Decl* d = e->a->type->decl;
        return fieldVal(L, &d->params[e->caseIndex], v.r + recordOffset(d, e->caseIndex));
    }
    case E_INDEX: {
        if (e->b->kind == E_RANGE) { /* xs[a..b], t[a..]: the same memory, a..b of it (checked) */
            Val src = lowerExpr(L, e->a);
            int len = src.r[1];
            int lo = e->b->a ? lowerExpr(L, e->b->a).r[0] : iconst(L, 0);
            int hi = e->b->b ? lowerExpr(L, e->b->b).r[0] : len;
            bounds(L, hi, op2(L, IR_ADD, len, iconst(L, 1), 'I'), PANIC_SLICE_END, e->line); /* 0 <= hi <= len */
            bounds(L, lo, op2(L, IR_ADD, hi, iconst(L, 1), 'I'), PANIC_SLICE_START, e->line); /* 0 <= lo <= hi */
            if (e->type->kind == TY_TEXT) { /* t[a..b] never cuts a character (UTF-8) in two */
                int args[4] = {src.r[0], len, lo, hi};
                bounds(L, rtcall(L, RT_TEXT_CUT, args, 4, 'I', e->line), iconst(L, 1), PANIC_TEXT_CUT, e->line);
            }
            int bytes = e->type->kind == TY_TEXT ? 1 : elemBytes(e->type);
            Val r = {2, regList(2)};
            r.r[0] = op2(L, IR_ADD, src.r[0], bytes == 1 ? lo : op2(L, IR_MUL, lo, iconst(L, bytes), 'I'), 'I');
            r.r[1] = op2(L, IR_SUB, hi, lo, 'I');
            return r;
        }
        if (e->a->type->kind == TY_MAP) return mapGet(L, lowerExpr(L, e->a), lowerExpr(L, e->b), e->type, e->line);
        Val arr = lowerExpr(L, e->a);
        int i = lowerExpr(L, e->b).r[0];
        /* for i in 0..len(xs) { xs[i] }: i is in range, xs never changes */
        bool proven = e->a->kind == E_NAME && e->b->kind == E_NAME && e->b->binding && e->a->binding &&
                      e->b->binding->safeIn == e->a->binding;
        if (!proven) bounds(L, i, arr.r[1], PANIC_INDEX, e->line);
        return loadElem(L, arr, i, e->type);
    }
    case E_UNARY: {
        if (e->decl) return lowerCall(L, e); /* -v: its type's negate */
        Val v = lowerExpr(L, e->a);
        if (e->op == T_NOT) return one(op2(L, IR_NOT, v.r[0], -1, 'I'));
        if (e->op == T_TILDE) return one(op2(L, IR_XOR, v.r[0], iconst(L, -1), 'I'));
        Val r = {v.n, regList(v.n)};
        for (int i = 0; i < v.n; i++) {
            char k = L->fn->kinds[v.r[i]];
            r.r[i] = op2(L, k == 'F' ? IR_FNEG : IR_NEG, v.r[i], -1, k);
        }
        return r;
    }
    case E_BINARY: return lowerBinary(L, e);
    case E_RANGE: return materialize(L, e, &qkTypeInt, e->line);
    case E_IF: return lowerIf(L, e);
    case E_MATCH: return lowerMatch(L, e);
    case E_BLOCK: return lowerBlock(L, e->block, e->type);
    case E_ARRAY: {
        int bytes = qkTypeSlots(e->type->elem) * 8;
        int n = iconst(L, e->argCount);
        int ptr = alloc(L, n, bytes, e->line);
        for (int i = 0; i < e->argCount; i++) {
            Val v = lowerStored(L, e->args[i]);
            storeSlots(L, ptr, iconst(L, i), bytes, 0, v);
        }
        Val r = {2, regList(2)};
        r.r[0] = ptr;
        r.r[1] = n;
        return r;
    }
    case E_CALL: return lowerCall(L, e);
    case E_LAMBDA:
        if (e->decl) return fnAddress(L, e->decl); /* kept as a value: a function of its own */
        break;
    }
    return (Val){0, NULL};
}

/* ---- statements ---------------------------------------------------------------------------------- */

static void debugLine(Lower* L, int line);

/* defer { ... }: kept while the block it is in is lowered; its code goes wherever that block is left: at its end, at a
 * return (this function's defers), at a break or continue (the loop's). The last one written runs first. A panic
 * runs none: it stops the call. */
static void runDefers(Lower* L, int from, int loopLevel) {
    for (int i = L->deferCount - 1; i >= from; i--) {
        if (L->defers[i].inlineDepth != L->inlineDepth || L->defers[i].loopLevel < loopLevel) continue;
        int saved = L->deferCount;
        L->deferCount = i; /* while its own code is lowered, only the ones before it are open */
        lowerBlock(L, L->defers[i].body, &qkTypeVoid);
        L->deferCount = saved;
    }
}

static Val lowerBlock(Lower* L, Block* b, Type* t) {
    bool value = t && t->kind != TY_VOID && t->kind != TY_ERROR;
    int mark = L->deferCount;
    for (int i = 0; i < b->count; i++) {
        Stmt* s = b->stmts[i];
        if (value && i == b->count - 1 && s->kind == S_EXPR) { /* the block's value: a line of its own too */
            debugLine(L, s->line);
            Val v = lowerStored(L, s->value);
            if (L->deferCount > mark) {
                v = copyVal(L, v); /* the value as it is now, before the defers run */
                runDefers(L, mark, 0);
                L->deferCount = mark;
            }
            return v;
        }
        if (s->kind == S_DEFER) {
            struct DeferAt at = {s->body, L->inlineDepth, L->loopLevel};
            QK_PUSH(L->defers, L->deferCount, L->deferCap, at);
            continue;
        }
        lowerStmt(L, s);
    }
    runDefers(L, mark, 0);
    L->deferCount = mark;
    return (Val){0, NULL};
}

/* ---- loops that make things: their memory back at the end of a turn --------------------------------
 * What a turn makes is garbage at its end, except what the vars from outside the loop now hold. So a loop
 * whose body may allocate starts with a mark (a frame in the arena), and ends each turn asking the runtime
 * whether the turns made enough; then those vars' values are copied out and the arena goes back to the mark.
 * Loops that make nothing (the numeric ones) get none of it. */

typedef struct LoopMemory {
    int frame;       /* the frame's register */
    int slotsReg;    /* how many slots the vars need, for the start */
    int nopAt;       /* where the start goes, once it is known to be needed */
    int bodyStart;
    bool started;
    Binding** roots; /* the vars from outside the loop that it changes and that hold memory */
    int rootCount, rootCap;
    Binding** inside;
    int insideCount, insideCap;
} LoopMemory;

static bool holdsMemory(const Type* t) {
    switch (t->kind) {
    case TY_TEXT: case TY_ARRAY: case TY_MAP: case TY_FN: return true;
    case TY_RECORD:
        for (int f = 0; f < t->decl->paramCount; f++)
            if (t->decl->params[f].boxed || holdsMemory(t->decl->params[f].binding->type)) return true;
        return false;
    case TY_ENUM:
        for (int k = 0; qkHasData(t) && k < t->decl->caseCount; k++)
            for (int f = 0; f < t->decl->caseFieldCount[k]; f++)
                if (t->decl->caseFields[k][f].boxed || holdsMemory(t->decl->caseFields[k][f].binding->type)) return true;
        return false;
    default: return false;
    }
}

/* The runtime's view of a type's slots (see rtLoopCollect): I plain, T text, A array, M map, (fields) a record,
 * E enum, B<type> a box (one slot: the address of the type's slots), R<at> the type whose description starts at
 * `at` (three digits of base 64 from '0'): what a box of a type that holds itself holds. */
typedef struct DescFrame {
    const Type* t;
    size_t at;
} DescFrame;

static void typeDescIn(const Type* t, QkBuf* out, DescFrame* open, int openCount);

static void fieldDesc(const Param* prm, QkBuf* out, DescFrame* open, int openCount) {
    if (!prm->boxed) {
        typeDescIn(prm->binding->type, out, open, openCount);
        return;
    }
    qkBufByte(out, 'B');
    for (int i = 0; i < openCount; i++)
        if (open[i].t == prm->binding->type) {
            size_t at = open[i].at;
            qkBufByte(out, 'R');
            for (int d = 2; d >= 0; d--) qkBufByte(out, (uint8_t)('0' + ((at >> (6 * d)) & 63)));
            return;
        }
    typeDescIn(prm->binding->type, out, open, openCount);
}

static void typeDescIn(const Type* t, QkBuf* out, DescFrame* open, int openCount) {
    for (int i = 0; i < openCount && (t->kind == TY_RECORD || t->kind == TY_ENUM); i++)
        if (open[i].t == t) { /* the type around it again (enum Json { List(items: [Json]) }): a reference */
            size_t at = open[i].at;
            qkBufByte(out, 'R');
            for (int d = 2; d >= 0; d--) qkBufByte(out, (uint8_t)('0' + ((at >> (6 * d)) & 63)));
            return;
        }
    switch (t->kind) {
    case TY_TEXT: qkBufByte(out, 'T'); return;
    case TY_FN: qkBufPuts(out, "IC"); return; /* its address, its captured values */
    case TY_ARRAY: qkBufByte(out, 'A'); typeDescIn(t->elem, out, open, openCount); return;
    case TY_MAP:
        qkBufByte(out, 'M');
        typeDescIn(t->key, out, open, openCount);
        typeDescIn(t->elem, out, open, openCount);
        return;
    case TY_RECORD:
        if (openCount < 64) open[openCount++] = (DescFrame){t, out->len};
        qkBufByte(out, '(');
        for (int f = 0; f < t->decl->paramCount; f++) fieldDesc(&t->decl->params[f], out, open, openCount);
        qkBufByte(out, ')');
        return;
    case TY_ENUM:
        if (!qkHasData(t)) {
            qkBufByte(out, 'I');
            return;
        }
        if (openCount < 64) open[openCount++] = (DescFrame){t, out->len};
        qkBufByte(out, 'E');
        qkBufByte(out, (uint8_t)(32 + t->decl->caseCount));
        for (int k = 0; k < t->decl->caseCount; k++) {
            for (int f = 0; f < t->decl->caseFieldCount[k]; f++) fieldDesc(&t->decl->caseFields[k][f], out, open, openCount);
            qkBufByte(out, '.');
        }
        return;
    default:
        for (int i = 0; i < qkTypeSlots(t); i++) qkBufByte(out, 'I');
    }
}

static void typeDesc(const Type* t, QkBuf* out) {
    DescFrame open[64];
    typeDescIn(t, out, open, 0);
}

static void scanBlockFor(LoopMemory* m, Block* b);

static void addBinding(Binding*** list, int* count, int* cap, Binding* b) {
    if (!b) return;
    for (int i = 0; i < *count; i++)
        if ((*list)[i] == b) return;
    QK_PUSH(*list, *count, *cap, b);
}

static void scanExprFor(LoopMemory* m, Expr* e) {
    if (!e) return;
    if (e->kind == E_CALL && (e->builtin == B_PUSH || e->builtin == B_REMOVE) && e->argCount && e->args[0]->kind == E_NAME)
        addBinding(&m->roots, &m->rootCount, &m->rootCap, e->args[0]->binding);
    scanExprFor(m, e->a);
    scanExprFor(m, e->b);
    scanExprFor(m, e->body);
    scanExprFor(m, e->elseExpr);
    for (int i = 0; i < e->argCount; i++) scanExprFor(m, e->args[i]);
    for (int i = 0; i < e->armCount; i++) {
        scanExprFor(m, e->arms[i].guard);
        scanExprFor(m, e->arms[i].body);
        for (int f = 0; e->arms[i].bindings && f < e->arms[i].bindCount; f++)
            addBinding(&m->inside, &m->insideCount, &m->insideCap, e->arms[i].bindings[f]);
    }
    if (e->thenBlock) scanBlockFor(m, e->thenBlock);
    if (e->block) scanBlockFor(m, e->block);
}

/* The vars a loop's body changes, and the names it makes itself (those die with each turn). */
static void scanBlockFor(LoopMemory* m, Block* b) {
    for (int i = 0; i < b->count; i++) {
        Stmt* s = b->stmts[i];
        if (s->kind == S_ASSIGN) addBinding(&m->roots, &m->rootCount, &m->rootCap, rootBinding(s->target));
        addBinding(&m->inside, &m->insideCount, &m->insideCap, s->binding);
        addBinding(&m->inside, &m->insideCount, &m->insideCap, s->indexBinding);
        if (s->pattern)
            for (int f = 0; s->pattern->bindings && f < s->pattern->bindCount; f++)
                addBinding(&m->inside, &m->insideCount, &m->insideCap, s->pattern->bindings[f]);
        scanExprFor(m, s->value);
        scanExprFor(m, s->target);
        scanExprFor(m, s->iter);
        if (s->body) scanBlockFor(m, s->body);
        if (s->elseBody) scanBlockFor(m, s->elseBody);
    }
}

static void loopBegin(Lower* L, LoopMemory* m, Block* body) {
    memset(m, 0, sizeof *m);
    scanBlockFor(m, body);
    int kept = 0, slots = 0;
    for (int i = 0; i < m->rootCount; i++) { /* from outside, holding memory, and lowered already (it has slots) */
        Binding* b = m->roots[i];
        bool inside = false;
        for (int k = 0; k < m->insideCount; k++) inside |= m->inside[k] == b;
        if (inside || !b->regs || !holdsMemory(b->type)) continue;
        m->roots[kept++] = b;
        slots += qkTypeSlots(b->type);
    }
    m->rootCount = kept;
    m->slotsReg = iconst(L, slots);
    m->nopAt = L->fn->count;
    emit(L, IR_NOP, -1, -1, -1);
    m->frame = newReg(L, 'I');
    m->bodyStart = L->fn->count;
}

static bool mayAllocate(const IrIns* ins) {
    if (ins->op == IR_CALL || ins->op == IR_HOSTCALL) return true;
    if (ins->op != IR_RTCALL) return false;
    switch (ins->imm) {
    case RT_PANIC: case RT_PRINT_INT: case RT_PRINT_FLOAT: case RT_PRINT_BOOL: case RT_PRINT_TEXT: case RT_PRINT_SEP:
    case RT_PRINT_END: case RT_NOW: case RT_SIN: case RT_COS: case RT_TAN: case RT_ASIN: case RT_ACOS: case RT_ATAN:
    case RT_ATAN2: case RT_EXP: case RT_LOG: case RT_POW: case RT_FUEL_OUT: case RT_PRINT_HANDLE: case RT_TEXT_COMPARE:
    case RT_FAIL: case RT_EXIT: case RT_PRINT_TO: case RT_STARTS_WITH: case RT_ENDS_WITH: case RT_FIND: case RT_TIME:
    case RT_MAP_GET: case RT_MAP_LEN: case RT_MAP_USED: case RT_MAP_ENTRY: case RT_MAP_REMOVE: case RT_LOOP_CHECK: case RT_TEXT_CUT:
    case RT_RANDOM: case RT_RANDOM_INT: case RT_SEED_RANDOM:
        return false;
    default: return true;
    }
}

/* The end of a turn: when the turns made enough, the vars' values out, the arena back to the mark. */
static void loopTurnEnd(Lower* L, LoopMemory* m, int line) {
    bool allocates = false;
    for (int k = m->bodyStart; k < L->fn->count && !allocates; k++) allocates = mayAllocate(&L->fn->code[k]);
    if (!allocates) return;
    if (!m->started) { /* the placeholder becomes the start: frame = loopStart(slots) */
        m->started = true;
        IrIns* ins = &L->fn->code[m->nopAt];
        ins->op = IR_RTCALL;
        ins->imm = RT_LOOP_START;
        ins->line = line;
        ins->args = regList(1);
        ins->args[0] = m->slotsReg;
        ins->argCount = 1;
        ins->rets = regList(1);
        ins->rets[0] = m->frame;
        ins->retCount = 1;
        use(L, m->slotsReg);
        use(L, m->frame);
        if (L->fn->maxOut < 1) L->fn->maxOut = 1;
    }
    int skip = label(L);
    jz(L, rtcall(L, RT_LOOP_CHECK, &m->frame, 1, 'I', line), skip);
    QkBuf desc = {0};
    int zero = iconst(L, 0);
    for (int i = 0, at = 48; i < m->rootCount; i++) { /* the vars into the frame */
        Binding* b = m->roots[i];
        Val v = {qkTypeSlots(b->type), b->regs};
        storeSlots(L, m->frame, zero, 8, at, v);
        at += 8 * v.n;
        typeDesc(b->type, &desc);
    }
    int args[2] = {m->frame, dataAddress(L, dataText(L, desc.data ? desc.data : "", (int)desc.len))};
    qkBufFree(&desc);
    rtcall(L, RT_LOOP_COLLECT, args, 2, 'V', line);
    for (int i = 0, at = 48; i < m->rootCount; i++) { /* and back, where the copies are */
        Binding* b = m->roots[i];
        for (int j = 0; j < qkTypeSlots(b->type); j++, at += 8) {
            IrIns* ld = emit(L, IR_LOAD, b->regs[j], m->frame, zero);
            ld->imm = 8;
            ld->c = at;
        }
    }
    place(L, skip);
}

/* A for loop's source (its array, or its range's ends) worked out before the loop's mark: it lives on. */
static void preLowerSource(Lower* L, Expr* iter) {
    Expr* base = stripImplicit(iter);
    for (;;) {
        if (base->kind == E_NAME && base->binding && base->binding->lazy) base = stripImplicit(base->binding->init);
        else if (base->kind == E_CALL && (base->builtin == B_MAP || base->builtin == B_FILTER)) base = stripImplicit(base->args[0]);
        else break;
    }
    if (base->kind == E_RANGE) {
        int a = lowerExpr(L, base->a).r[0];
        int b = lowerExpr(L, base->b).r[0];
        L->preRange = base;
        L->preA = newReg(L, 'I');
        L->preB = newReg(L, 'I');
        mov(L, L->preA, a);
        mov(L, L->preB, b);
    } else if (base->type->kind == TY_ARRAY) {
        Val arr = copyVal(L, lowerExpr(L, base)); /* first: lowering it may use (and clear) preArray itself */
        L->preArray = base;
        L->preArrayVal = arr;
    }
}

typedef struct ForCtx {
    Stmt* s;
    LoopMemory* memory;
} ForCtx;

/* Leaves the function (or the one being inlined) with v. */
bool qkDebug;

/* ---- debug builds: lines, values, frames, for the debugger and for traces ---------------------------- */

static bool traceable(const Type* t) {
    switch (t->kind) {
    case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_TEXT: case TY_ENUM: case TY_RECORD: case TY_ARRAY: case TY_MAP:
    case TY_HANDLE:
        return t != &qkTypeNone && t != &qkTypeErr;
    default: return false;
    }
}

static Val textData(Lower* L, const char* s) { return literalText(L, s, (int)strlen(s)); }

/* the name's new value, as print writes it, to the debugger: in its frame, and under its node's graph key */
static void traceValue(Lower* L, const Binding* b, int line) {
    if (!qkDebug || !b || !b->regs || !b->type || !traceable(b->type) || b->name[0] == '#') return;
    char key[256];
    const char* last = NULL;
    bool found = line && qkKeyAtLine(L->prog, line, key, sizeof key);
    if (found) last = strrchr(key, '/');
    if (!found || !last || strcmp(last + 1, b->name)) /* the statement's key, when it is this name's node */
        snprintf(key, sizeof key, "%s/%s", L->current->templateOf ? L->current->templateOf->name : L->current->name, b->name);
    rtcall(L, RT_CAPTURE_BEGIN, NULL, 0, 'V', line);
    printVal(L, (Val){qkTypeSlots(b->type), b->regs}, b->type);
    Val text = unpackText(L, rtcall(L, RT_CAPTURE_END, NULL, 0, 'I', line));
    Val name = textData(L, b->name), k = textData(L, key);
    int args[6] = {name.r[0], name.r[1], k.r[0], k.r[1], text.r[0], text.r[1]};
    rtcall(L, RT_DEBUG_VALUE, args, 6, 'V', line);
}

static void debugLine(Lower* L, int line) {
    if (qkDebug && line > 0) {
        int x = iconst(L, line);
        rtcall(L, RT_DEBUG_LINE, &x, 1, 'V', line);
    }
}

static void lowerReturn(Lower* L, Val v) {
    if (L->deferCount) { /* this function's defers, the value kept as it is */
        v = copyVal(L, v);
        runDefers(L, 0, 0);
    }
    if (L->inlineDepth) { /* inside an inlined function: to its end */
        InlineFrame* fr = &L->inlining[L->inlineDepth - 1];
        if (v.n) movVal(L, fr->result, v);
        jmp(L, fr->end);
        return;
    }
    if (qkDebug) rtcall(L, RT_DEBUG_LEAVE, NULL, 0, 'V', 0);
    IrIns* ins = emit(L, IR_RET, -1, -1, -1);
    ins->args = v.r;
    ins->argCount = v.n;
    for (int i = 0; i < v.n; i++) use(L, v.r[i]);
}

static void entrySink(Lower* L, Val key, Val value, void* ctx) {
    ForCtx* f = ctx;
    f->s->binding->regs = key.r;
    if (f->s->indexBinding) f->s->indexBinding->regs = value.r;
    int savedBreak = L->userBreak, savedContinue = L->userContinue;
    int turnEnd = label(L);
    L->userBreak = L->loopBreak;
    L->userContinue = turnEnd;
    L->loopLevel++;
    lowerBlock(L, f->s->body, &qkTypeVoid);
    L->loopLevel--;
    place(L, turnEnd);
    loopTurnEnd(L, f->memory, f->s->line);
    L->userBreak = savedBreak;
    L->userContinue = savedContinue;
}

static void forSink(Lower* L, Val elem, int index, void* ctx) {
    ForCtx* f = ctx;
    f->s->binding->regs = elem.r;
    if (f->s->indexBinding) f->s->indexBinding->regs = one(index).r;
    traceValue(L, f->s->binding, f->s->line);
    if (f->s->indexBinding) traceValue(L, f->s->indexBinding, 0);
    int savedBreak = L->userBreak, savedContinue = L->userContinue;
    int turnEnd = label(L);
    L->userBreak = L->loopBreak;
    L->userContinue = turnEnd;
    L->loopLevel++;
    lowerBlock(L, f->s->body, &qkTypeVoid);
    L->loopLevel--;
    place(L, turnEnd);
    loopTurnEnd(L, f->memory, f->s->line);
    L->userBreak = savedBreak;
    L->userContinue = savedContinue;
}

/* Where a place's slots are: registers (a var), or memory (an element of a var array). */
typedef struct PlaceRef {
    bool memory;
    bool boxed; /* a boxed field: what is written goes into a new box */
    Val regs;       /* registers */
    int base, index, stride, offset; /* memory */
} PlaceRef;

static PlaceRef placeOf(Lower* L, Expr* e) {
    PlaceRef p = {0};
    if (e->kind == E_NAME) {
        p.regs = (Val){qkTypeSlots(e->binding->type), e->binding->regs};
        return p;
    }
    if (e->kind == E_INDEX) {
        Val arr = lowerExpr(L, e->a);
        int i = lowerExpr(L, e->b).r[0];
        bounds(L, i, arr.r[1], PANIC_INDEX, e->line);
        p.memory = true;
        p.base = arr.r[0];
        p.index = i;
        p.stride = qkTypeSlots(e->type) * 8;
        p.offset = 0;
        return p;
    }
    /* a field */
    PlaceRef outer = placeOf(L, e->a);
    Decl* d = e->a->type->decl;
    int at = recordOffset(d, e->caseIndex);
    outer.boxed = d->params[e->caseIndex].boxed; /* only set whole (the checker says so): a new box */
    if (outer.memory) {
        outer.offset += at * 8;
        return outer;
    }
    outer.regs = (Val){qkFieldSlots(&d->params[e->caseIndex]), outer.regs.r + at};
    return outer;
}

static void writePlace(Lower* L, PlaceRef p, Val v) {
    if (p.boxed) v = boxVal(L, v, 0);
    if (p.memory) storeSlots(L, p.base, p.index, p.stride, p.offset, v);
    else movVal(L, p.regs, v);
}

static void lowerAssign(Lower* L, Stmt* s) {
    Val v;
    if (s->target->kind == E_INDEX && s->target->a->type->kind == TY_MAP) { /* m[k] = v: in place, m is the var's own */
        Val m = lowerExpr(L, s->target->a);
        Val k = lowerExpr(L, s->target->b);
        mapSet(L, m, k, lowerStored(L, s->value), s->line);
        return;
    }
    if (s->op == T_ASSIGN && s->target->kind == E_NAME) { /* xs = ys: xs gets its own copy */
        v = lowerOwned(L, s->value);
        Val fresh = copyVal(L, v);
        writePlace(L, placeOf(L, s->target), fresh);
        return;
    }
    if (s->op == T_ASSIGN || s->opValue) { /* v += w on a record: v = v + w, through its type's plus */
        v = lowerStored(L, s->opValue ? s->opValue : s->value);
        Val fresh = copyVal(L, v); /* p = Point(p.y, p.x) reads before it writes */
        PlaceRef p = placeOf(L, s->target);
        writePlace(L, p, fresh);
        return;
    }
    PlaceRef p = placeOf(L, s->target);
    Val cur;
    if (p.memory) {
        cur = valOf(L, s->target->type);
        for (int j = 0; j < cur.n; j++) {
            IrIns* ins = emit(L, IR_LOAD, cur.r[j], p.base, p.index);
            ins->imm = p.stride;
            ins->c = p.offset + j * 8;
        }
    } else {
        cur = p.regs;
    }
    v = lowerExpr(L, s->value);
    Val r = {cur.n, regList(cur.n)};
    for (int i = 0; i < cur.n; i++) {
        bool f = L->fn->kinds[cur.r[i]] == 'F';
        int y = v.r[i];
        IrOp op;
        switch (s->op) {
        case T_PLUSEQ: op = f ? IR_FADD : IR_ADD; break;
        case T_MINUSEQ: op = f ? IR_FSUB : IR_SUB; break;
        case T_STAREQ: op = f ? IR_FMUL : IR_MUL; break;
        default: op = f ? IR_FDIV : IR_DIV;
        }
        r.r[i] = newReg(L, f ? 'F' : 'I');
        IrIns* in = emit(L, op, r.r[i], cur.r[i], y);
        in->line = s->line;
        if (qkChecked && !f && op != IR_DIV) in->cond = 1;
    }
    writePlace(L, p, r);
}

static void lowerStmtInner(Lower* L, Stmt* s);

static void lowerStmt(Lower* L, Stmt* s) {
    debugLine(L, s->line);
    lowerStmtInner(L, s);
    if (!qkDebug) return;
    if ((s->kind == S_LET || s->kind == S_VAR) && s->pattern) {
        for (int f = 0; s->pattern->bindings && f < s->pattern->bindCount; f++) traceValue(L, s->pattern->bindings[f], s->line);
    } else if ((s->kind == S_LET || s->kind == S_VAR) && s->binding && !s->binding->lazy) {
        traceValue(L, s->binding, s->line);
    } else if (s->kind == S_ASSIGN) {
        Expr* t = s->target;
        while (t->kind == E_FIELD || t->kind == E_INDEX) t = t->a;
        if (t->kind == E_NAME && t->binding) traceValue(L, t->binding, 0);
    }
}

static void lowerStmtInner(Lower* L, Stmt* s) {
    switch (s->kind) {
    case S_LET: case S_VAR: {
        if (s->pattern) { /* let Some(x) = value else { ... }: the else leaves, so below it the case is the one */
            Val whole = copyVal(L, lowerExpr(L, s->value));
            Arm* a = s->pattern;
            int fail = label(L), ok = label(L);
            matchPattern(L, a, whole, s->value->type, fail, true);
            jmp(L, ok);
            place(L, fail);
            if (s->elseBody) lowerBlock(L, s->elseBody, &qkTypeVoid);
            place(L, ok);
            break;
        }
        Binding* b = s->binding;
        if (b->lazy) return;
        Val v;
        Type* vt = s->value->type;
        if (vt->kind == TY_RANGE || vt->kind == TY_ITER) v = materialize(L, s->value, vt->elem, s->line);
        else if (s->kind == S_VAR) v = lowerOwned(L, s->value);
        else v = lowerStored(L, s->value);
        Val c = valOf(L, b->type);
        movVal(L, c, v);
        b->regs = c.r;
        break;
    }
    case S_ASSIGN: lowerAssign(L, s); break;
    case S_EXPR: lowerExpr(L, s->value); break;
    case S_FOR: {
        LoopMemory memory;
        ForCtx f = {s, &memory};
        if (s->iter->type->kind == TY_MAP) {
            Val m = copyVal(L, lowerExpr(L, s->iter));
            loopBegin(L, &memory, s->body);
            forEachEntry(L, m, s->iter->type, entrySink, &f, s->line);
        } else {
            preLowerSource(L, s->iter);
            loopBegin(L, &memory, s->body);
            forEach(L, s->iter, forSink, &f);
        }
        break;
    }
    case S_WHILE: { /* the test at the bottom: one jump per turn */
        LoopMemory memory;
        loopBegin(L, &memory, s->body);
        int top = label(L), test = label(L), done = label(L), turnEnd = label(L);
        L->depth++;
        jmp(L, test);
        place(L, top);
        fuel(L, s->line);
        int savedBreak = L->userBreak, savedContinue = L->userContinue;
        L->userBreak = done;
        L->userContinue = turnEnd;
        L->loopLevel++;
        lowerBlock(L, s->body, &qkTypeVoid);
        L->loopLevel--;
        place(L, turnEnd);
        loopTurnEnd(L, &memory, s->line);
        L->userBreak = savedBreak;
        L->userContinue = savedContinue;
        place(L, test);
        jnz(L, lowerExpr(L, s->value).r[0], top);
        L->depth--;
        place(L, done);
        break;
    }
    case S_DEFER: break; /* kept by lowerBlock, lowered where its block is left */
    case S_RETURN: lowerReturn(L, s->value ? lowerStored(L, s->value) : (Val){0, NULL}); break;
    case S_BREAK:
        runDefers(L, 0, L->loopLevel); /* the defers inside the loop */
        jmp(L, L->userBreak);
        break;
    case S_CONTINUE:
        runDefers(L, 0, L->loopLevel);
        jmp(L, L->userContinue);
        break;
    }
}

/* ---- functions ----------------------------------------------------------------------------------- */

static void lowerFn(Lower* L, Decl* d, IrFn* f) {
    memset(f, 0, sizeof *f);
    f->name = d->name;
    f->line = d->line;
    L->fn = f;
    L->current = d;
    L->depth = 0;
    int total = 0;
    for (int i = 0; i < d->paramCount; i++) total += qkTypeSlots(d->params[i].binding->type);
    f->params = regList(total);
    f->paramCount = total;
    for (int i = 0, k = 0; i < d->paramCount; i++) {
        Binding* b = d->params[i].binding;
        Val v = valOf(L, b->type);
        b->regs = v.r;
        for (int j = 0; j < v.n; j++) f->params[k++] = v.r[j];
    }
    for (int i = 0, at = 8; i < d->captureCount; i++) { /* a lambda's captured values, from where it was made */
        Binding* in = d->captures[i].inner;
        Val v = valOf(L, in->type);
        for (int j = 0; j < v.n; j++, at += 8) {
            IrIns* ld = emit(L, IR_LOAD, v.r[j], d->env->regs[0], iconst(L, 0));
            ld->imm = 8;
            ld->c = at;
        }
        in->regs = v.r;
    }
    f->resultCount = qkTypeSlots(d->retType);
    if (f->resultCount > f->maxOut) f->maxOut = f->resultCount; /* results come back where arguments went */
    if (total > f->maxOut) f->maxOut = total;
    if (qkDebug) { /* its frame, and its parameters' values */
        Val name = textData(L, d->templateOf ? d->templateOf->name : d->name);
        rtcall(L, RT_DEBUG_ENTER, name.r, 2, 'V', d->line);
        for (int i = 0; i < d->paramCount; i++) traceValue(L, d->params[i].binding, 0);
    }
    fuel(L, d->line);
    Val v;
    if (d->body->kind == E_BLOCK) v = lowerBlock(L, d->body->block, d->retType);
    else v = lowerStored(L, d->body);
    if (qkDebug) rtcall(L, RT_DEBUG_LEAVE, NULL, 0, 'V', 0);
    IrIns* ret = emit(L, IR_RET, -1, -1, -1);
    if (f->resultCount && v.n) {
        ret->args = v.r;
        ret->argCount = v.n;
        for (int i = 0; i < v.n; i++) use(L, v.r[i]);
    }
}

/* print(a boxed value): its slots at the address the one parameter gives */
static void lowerHelper(Lower* L, const Type* t, bool byValue, IrFn* f) {
    static Decl helper;
    helper.kind = D_FN;
    helper.name = "print";
    helper.retType = &qkTypeVoid;
    memset(f, 0, sizeof *f);
    QkBuf name = {0};
    qkBufPuts(&name, "print ");
    qkTypeName(t, &name);
    f->name = qkDup(name.data, name.len);
    qkBufFree(&name);
    L->fn = f;
    L->current = &helper;
    L->depth = 0;
    L->inl = 0;
    L->inlineDepth = 0;
    L->printingCount = 0;
    if (byValue) { /* its slots are the arguments */
        Val v = valOf(L, t);
        f->params = v.r;
        f->paramCount = v.n;
        f->maxOut = v.n;
        printVal(L, v, t);
    } else {
        int p = newReg(L, 'I');
        f->params = regList(1);
        f->params[0] = p;
        f->paramCount = 1;
        f->maxOut = 1;
        printVal(L, loadSlots(L, p, t), t);
    }
    emit(L, IR_RET, -1, -1, -1);
}

bool qkLower(Program* p, IrProgram* out, bool fuelOn) {
    memset(out, 0, sizeof *out);
    out->fuel = fuelOn;
    Lower L = {0};
    L.prog = p;
    L.out = out;
    L.fnIndex = regList(p->count);
    L.externIndex = regList(p->count);
    out->mainIndex = -1;
    int n = 0;
    for (int i = 0; i < p->count; i++) {
        L.externIndex[i] = -1;
        Decl* d = p->decls[i];
        if (d->kind == D_FN && !d->isExtern && !d->isTemplate && !d->inlineOnly) L.fnIndex[i] = n++;
    }
    out->fns = qkNew(sizeof(IrFn) * (size_t)(n ? n : 1));
    out->count = n;
    out->externs = qkNew(sizeof(IrExtern) * (size_t)(p->count ? p->count : 1));
    qkBufAdd(&out->data, "", 0);
    for (int i = 0; i < p->count; i++) {
        Decl* d = p->decls[i];
        if (d->kind != D_FN || d->isExtern || d->isTemplate || d->inlineOnly) continue;
        lowerFn(&L, d, &out->fns[L.fnIndex[i]]);
        if (!strcmp(d->name, "main")) out->mainIndex = L.fnIndex[i];
    }
    for (int h = 0; h < L.helperCount; h++) { /* printers of boxed values (they may ask for more) */
        IrFn* grown = qkNew(sizeof(IrFn) * (size_t)(out->count + 1));
        memcpy(grown, out->fns, sizeof(IrFn) * (size_t)out->count);
        out->fns = grown;
        lowerHelper(&L, L.helpers[h].t, L.helpers[h].byValue, &out->fns[out->count++]);
    }
    return true;
}

/* ---- a readable dump, for `quark ir` ------------------------------------------------------------- */

void qkIrDump(const IrProgram* p, QkBuf* out) {
    static const char* const names[] = {"const", "mov", "add", "sub", "mul", "div", "mod", "neg", "fadd", "fsub", "fmul",
                                        "fdiv", "fneg", "sqrt", "fmin", "fmax", "cmp", "fcmp", "not", "i2f", "f2i", "round",
                                        "load", "store", "bounds", "label", "jmp", "jz", "jnz", "call", "rtcall", "ret", "fuel", "hostcall", "nop",
                                        "and", "or", "xor", "shl", "sar"};
    static const char* const conds[] = {"==", "!=", "<", "<=", ">", ">="};
    for (int i = 0; i < p->count; i++) {
        const IrFn* f = &p->fns[i];
        qkBufPrintf(out, "fn %s (%d params, %d results, %d registers)\n", f->name, f->paramCount, f->resultCount, f->vregCount);
        for (int k = 0; k < f->count; k++) {
            const IrIns* ins = &f->code[k];
            if (ins->op == IR_LABEL) {
                qkBufPrintf(out, "  L%lld:\n", (long long)ins->imm);
                continue;
            }
            qkBufPrintf(out, "    %-7s", names[ins->op]);
            if (ins->dst >= 0) qkBufPrintf(out, " %c%d", f->kinds[ins->dst] == 'F' ? 'f' : 'r', ins->dst);
            if (ins->op == IR_CMP || ins->op == IR_FCMP) qkBufPrintf(out, " = r%d %s r%d", ins->a, conds[ins->cond], ins->b);
            else {
                if (ins->a >= 0) qkBufPrintf(out, " %c%d", f->kinds[ins->a] == 'F' ? 'f' : 'r', ins->a);
                if (ins->b >= 0) qkBufPrintf(out, " %c%d", f->kinds[ins->b] == 'F' ? 'f' : 'r', ins->b);
            }
            if (ins->op == IR_CONST || ins->op == IR_JMP || ins->op == IR_JZ || ins->op == IR_JNZ || ins->op == IR_LOAD ||
                ins->op == IR_STORE || ins->op == IR_CALL || ins->op == IR_RTCALL || ins->op == IR_ROUND || ins->op == IR_HOSTCALL)
                qkBufPrintf(out, " #%lld", (long long)ins->imm);
            if ((ins->op == IR_LOAD || ins->op == IR_STORE) && ins->c) qkBufPrintf(out, " +%d", ins->c);
            if (ins->op == IR_CONST && ins->c == 1) qkBufPuts(out, " (data)");
            if (ins->op == IR_CONST && ins->c == 2) qkBufPuts(out, " (function)");
            for (int j = 0; j < ins->argCount; j++) qkBufPrintf(out, "%s%c%d", j ? ", " : " (", f->kinds[ins->args[j]] == 'F' ? 'f' : 'r', ins->args[j]);
            if (ins->argCount) qkBufPuts(out, ")");
            for (int j = 0; j < ins->retCount; j++) qkBufPrintf(out, "%s%d", j ? ", r" : " -> r", ins->rets[j]);
            qkBufPuts(out, "\n");
        }
    }
}
