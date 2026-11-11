/* Quark — lowering: the checked tree to a small register IR. Every value is a few 8-byte slots (an Int is
 * one, a Vec3 three, an array two: its address and length), so records and vectors live in registers, never
 * in memory. Loops are fused: map, filter and the like do not build arrays; they add code inside the one
 * loop of their source (a range or an array), and sum, fold, histogram... end it. Lambdas are inlined. */
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
    } inlining[8];  /* functions being inlined, innermost last */
    Decl* current;     /* the function being lowered */
    int* externIndex;  /* decl index -> the program's host function list, or -1 */
    int inlineDepth;
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

static Val lowerExpr(Lower* L, Expr* e);
static Val lowerBlock(Lower* L, Block* b, Type* t);
static void lowerStmt(Lower* L, Stmt* s);
static void forEach(Lower* L, Expr* src, Sink sink, void* ctx);

static Expr* stripImplicit(Expr* e) {
    while (e->kind == E_CALL && e->implicit && e->builtin == B_TOARRAY) e = e->args[0];
    return e;
}

static Binding* rootBinding(Expr* e) {
    while (e->kind == E_FIELD || e->kind == E_INDEX) e = e->a;
    return e->kind == E_NAME ? e->binding : NULL;
}

/* A stored array must not change when the var (or the caller's var) it came from changes. */
static bool needsCopy(Expr* e) {
    if (e->type->kind != TY_ARRAY) return false;
    if (e->kind != E_NAME && e->kind != E_FIELD && e->kind != E_INDEX) return false;
    Binding* b = rootBinding(e);
    return b && (b->mutable || b->isParam);
}

static Val lowerStored(Lower* L, Expr* e) {
    Val v = lowerExpr(L, e);
    if (needsCopy(e)) return copyArray(L, v, elemBytes(e->type), e->line);
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
    if (e->kind == E_CALL && (e->builtin == B_FILTER)) return NULL;
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
    int bins;
    int ptr;
    int line;
} Acc;

static void bindLambda(Expr* lam, Val* vals, int count) {
    for (int i = 0; i < lam->paramCount && i < count; i++) {
        lam->params[i].binding->regs = vals[i].r;
    }
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

static void histogramSink(Lower* L, Val elem, int index, void* ctx) {
    (void)index;
    Acc* a = ctx;
    int k;
    if (a->lambda) {
        Val args[1] = {elem};
        bindLambda(a->lambda, args, 1);
        k = lowerExpr(L, a->lambda->body).r[0];
    } else {
        k = elem.r[0];
    }
    bounds(L, k, a->bins, PANIC_HISTOGRAM, a->line);
    int v = newReg(L, 'I');
    IrIns* ld = emit(L, IR_LOAD, v, a->ptr, k);
    ld->imm = 8;
    ld->c = 0;
    emit(L, IR_ADD, v, v, iconst(L, 1));
    IrIns* st = emit(L, IR_STORE, v, a->ptr, k);
    st->imm = 8;
    st->c = 0;
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
    int counter; /* filter: the index after it */
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
        Through t = {src->args[1], sink, ctx, iconst(L, 0)};
        forEach(L, src->args[0], filterSink, &t);
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

/* ---- printing ------------------------------------------------------------------------------------ */

static void printVal(Lower* L, Val v, const Type* t);

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

static void printVal(Lower* L, Val v, const Type* t) {
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
    case TY_ENUM: {
        /* a table of (offset, length) per case in the data block */
        Decl* d = t->decl;
        int64_t* table = qkNew(sizeof(int64_t) * 2 * (size_t)d->caseCount);
        for (int k = 0; k < d->caseCount; k++) {
            table[2 * k] = dataText(L, d->cases[k], (int)strlen(d->cases[k]));
            table[2 * k + 1] = (int64_t)strlen(d->cases[k]);
        }
        int at = (int)L->out->data.len;
        qkBufAdd(&L->out->data, table, sizeof(int64_t) * 2 * (size_t)d->caseCount);
        qkBufPrintf(&L->out->data, "%s.", d->name);
        int prefix = at + (int)(sizeof(int64_t) * 2 * (size_t)d->caseCount);
        while (L->out->data.len % 8) qkBufByte(&L->out->data, 0);
        int args[2] = {dataAddress(L, prefix), iconst(L, (int64_t)strlen(d->name) + 1)};
        rtcall(L, RT_PRINT_TEXT, args, 2, 'V', 0);
        int base = dataAddress(L, 0);
        int tableAt = dataAddress(L, at);
        int off = newReg(L, 'I');
        IrIns* ld = emit(L, IR_LOAD, off, tableAt, v.r[0]);
        ld->imm = 16;
        ld->c = 0;
        int len = newReg(L, 'I');
        ld = emit(L, IR_LOAD, len, tableAt, v.r[0]);
        ld->imm = 16;
        ld->c = 8;
        int args2[2] = {op2(L, IR_ADD, base, off, 'I'), len};
        rtcall(L, RT_PRINT_TEXT, args2, 2, 'V', 0);
        break;
    }
    case TY_VEC: {
        char head[8];
        snprintf(head, sizeof head, "Vec%d(", t->n);
        printLiteral(L, head);
        for (int i = 0; i < t->n; i++) {
            if (i) printLiteral(L, ", ");
            rtcall(L, RT_PRINT_FLOAT, &v.r[i], 1, 'V', 0);
        }
        printLiteral(L, ")");
        break;
    }
    case TY_RECORD: {
        Decl* d = t->decl;
        QkBuf head = {0};
        qkBufPrintf(&head, "%s(", d->name);
        printLiteral(L, head.data);
        qkBufFree(&head);
        int at = 0;
        for (int f = 0; f < d->paramCount; f++) {
            Type* ft = d->params[f].binding->type;
            QkBuf name = {0};
            qkBufPrintf(&name, "%s%s: ", f ? ", " : "", d->params[f].name);
            printLiteral(L, name.data);
            qkBufFree(&name);
            Val part = {qkTypeSlots(ft), v.r + at};
            printVal(L, part, ft);
            at += part.n;
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

/* ---- calls --------------------------------------------------------------------------------------- */

static Val lowerBuiltin(Lower* L, Expr* e) {
    Expr** a = e->args;
    switch (e->builtin) {
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
        return one(op2(L, IR_F2I, f, -1, 'I'));
    }
    case B_TOFLOAT: {
        int x = lowerExpr(L, a[0]).r[0];
        return one(L->fn->kinds[x] == 'F' ? x : op2(L, IR_I2F, x, -1, 'F'));
    }
    case B_TOINT: {
        int x = lowerExpr(L, a[0]).r[0];
        return one(L->fn->kinds[x] == 'I' ? x : op2(L, IR_F2I, x, -1, 'I'));
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
        Val x = lowerExpr(L, a[0]);
        Val y = lowerExpr(L, a[1]);
        int t = lowerExpr(L, a[2]).r[0];
        Val r = {x.n, regList(x.n)};
        for (int i = 0; i < x.n; i++)
            r.r[i] = op2(L, IR_FADD, x.r[i], op2(L, IR_FMUL, op2(L, IR_FSUB, y.r[i], x.r[i], 'F'), t, 'F'), 'F');
        return r;
    }
    case B_LENGTH: case B_DOT: {
        Val x = lowerExpr(L, a[0]);
        Val y = e->builtin == B_DOT ? lowerExpr(L, a[1]) : x;
        int s = op2(L, IR_FMUL, x.r[0], y.r[0], 'F');
        for (int i = 1; i < x.n; i++) s = op2(L, IR_FADD, s, op2(L, IR_FMUL, x.r[i], y.r[i], 'F'), 'F');
        return one(e->builtin == B_LENGTH ? op2(L, IR_SQRT, s, -1, 'F') : s);
    }
    case B_LEN: {
        Expr* src = stripImplicit(a[0]);
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
    case B_HISTOGRAM: {
        Acc acc = {0};
        acc.bins = newReg(L, 'I');
        mov(L, acc.bins, lowerExpr(L, a[1]).r[0]);
        acc.ptr = alloc(L, acc.bins, 8, e->line);
        acc.lambda = e->argCount == 3 ? a[2] : NULL;
        acc.line = e->line;
        forEach(L, a[0], histogramSink, &acc);
        Val r = {2, regList(2)};
        r.r[0] = acc.ptr;
        r.r[1] = acc.bins;
        return r;
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
    case B_VEC2: case B_VEC3: case B_VEC4: {
        Val r = {e->argCount, regList(e->argCount)};
        for (int i = 0; i < e->argCount; i++) {
            r.r[i] = newReg(L, 'F');
            mov(L, r.r[i], lowerExpr(L, a[i]).r[0]);
        }
        return r;
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
static Val hostCall(Lower* L, Expr* e, Decl* d, Val* args, int total) {
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
    for (int i = 0, k = 0; i < e->argCount; i++)
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

static Val lowerCall(Lower* L, Expr* e) {
    if (e->builtin) return lowerBuiltin(L, e);
    Decl* d = e->decl;
    if (d->kind == D_TYPE) {
        Val r = valOf(L, d->type);
        int* fieldAt = regList(d->paramCount);
        for (int f = 0, at = 0; f < d->paramCount; f++) {
            fieldAt[f] = at;
            at += qkTypeSlots(d->params[f].binding->type);
        }
        for (int i = 0; i < e->argCount; i++) {
            int f = i;
            if (e->argNames[i])
                for (int k = 0; k < d->paramCount; k++)
                    if (!strcmp(d->params[k].name, e->argNames[i])) f = k;
            Val v = lowerStored(L, e->args[i]);
            Val part = {v.n, r.r + fieldAt[f]};
            movVal(L, part, v);
        }
        return r;
    }
    /* a function: arguments as a flat list of slots */
    int total = 0;
    Val* args = qkNew(sizeof(Val) * (size_t)(e->argCount ? e->argCount : 1));
    for (int i = 0; i < e->argCount; i++) {
        args[i] = lowerExpr(L, e->args[i]);
        total += args[i].n;
    }
    if (d->isExtern) return hostCall(L, e, d, args, total);
    /* a small function is inlined: no call at all. Parameters never change, so they can be the arguments' own
     * registers; a return becomes a jump to the end. Not into itself (recursion stays a call). */
    bool inside = false;
    for (int i = 0; i < L->inlineDepth; i++) inside |= L->inlining[i].d == d;
    if (!inside && L->inlineDepth < 8 && d != L->current && exprSize(d->body) <= 120) {
        int** saved = qkNew(sizeof(int*) * (size_t)(d->paramCount ? d->paramCount : 1));
        for (int i = 0; i < d->paramCount; i++) {
            saved[i] = d->params[i].binding->regs;
            d->params[i].binding->regs = args[i].r;
        }
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
        for (int i = 0; i < d->paramCount; i++) d->params[i].binding->regs = saved[i];
        return r;
    }
    IrIns* ins = emit(L, IR_CALL, -1, -1, -1);
    ins->imm = L->fnIndex[d->index];
    ins->line = e->line;
    ins->args = regList(total);
    ins->argCount = total;
    for (int i = 0, k = 0; i < e->argCount; i++)
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

static Val lowerBinary(Lower* L, Expr* e) {
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
    Val a = lowerExpr(L, e->a);
    Val b = lowerExpr(L, e->b);
    Type* ta = e->a->type;
    Type* tb = e->b->type;
    switch (e->op) {
    case T_PLUS: case T_MINUS: case T_STAR: case T_SLASH: case T_PERCENT: {
        if (e->type->kind == TY_VEC) {
            static const IrOp fops[] = {IR_FADD, IR_FSUB, IR_FMUL, IR_FDIV};
            IrOp op = fops[e->op == T_PLUS ? 0 : e->op == T_MINUS ? 1 : e->op == T_STAR ? 2 : 3];
            Val r = {e->type->n, regList(e->type->n)};
            for (int i = 0; i < r.n; i++) {
                int x = ta->kind == TY_VEC ? a.r[i] : a.r[0];
                int y = tb->kind == TY_VEC ? b.r[i] : b.r[0];
                r.r[i] = op2(L, op, x, y, 'F');
            }
            return r;
        }
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
        emit(L, op, r, a.r[0], b.r[0])->line = e->line;
        return one(r);
    }
    default: {
        static const int conds[] = {C_EQ, C_NE, C_LT, C_LE, C_GT, C_GE};
        int cond = conds[e->op - T_EQ];
        if (ta->kind == TY_VEC) {
            int all = iconst(L, 1);
            for (int i = 0; i < a.n; i++) emit(L, IR_MUL, all, all, cmp(L, C_EQ, a.r[i], b.r[i]));
            return one(cond == C_EQ ? all : op2(L, IR_NOT, all, -1, 'I'));
        }
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

static Val lowerMatch(Lower* L, Expr* e) {
    bool value = e->type->kind != TY_VOID && e->type->kind != TY_ERROR;
    Val r = value ? valOf(L, e->type) : (Val){0, NULL};
    int subject = newReg(L, 'I');
    mov(L, subject, lowerExpr(L, e->a).r[0]);
    int done = label(L);
    for (int i = 0; i < e->armCount; i++) {
        Arm* a = &e->arms[i];
        int next = label(L);
        bool last = i == e->armCount - 1;
        if (a->pattern || a->intPattern) {
            bool exhaustiveLast = last && a->pattern; /* checked: every case is here */
            if (!exhaustiveLast) {
                int64_t k = a->pattern ? a->caseIndex : a->intPattern->i;
                jz(L, cmp(L, C_EQ, subject, iconst(L, k)), next);
            }
        }
        Expr* body = a->body;
        Val v = body->kind == E_BLOCK ? lowerBlock(L, body->block, value ? e->type : &qkTypeVoid) : lowerExpr(L, body);
        if (value) movVal(L, r, v);
        jmp(L, done);
        place(L, next);
    }
    place(L, done);
    return r;
}

static Val lowerExpr(Lower* L, Expr* e) {
    switch (e->kind) {
    case E_INT: return one(iconst(L, e->i));
    case E_FLOAT: return one(fconst(L, e->f));
    case E_BOOL: return one(iconst(L, e->i));
    case E_CASE: return one(iconst(L, e->caseIndex));
    case E_TEXT: {
        Val v = {2, regList(2)};
        v.r[0] = dataAddress(L, dataText(L, e->text, e->textLen));
        v.r[1] = iconst(L, e->textLen);
        return v;
    }
    case E_NAME: {
        Binding* b = e->binding;
        if (b->lazy) return materialize(L, b->init, b->type->elem, e->line);
        Val v = {qkTypeSlots(b->type), b->regs};
        return v;
    }
    case E_FIELD: {
        Val v = lowerExpr(L, e->a);
        if (e->a->type->kind == TY_VEC) return one(v.r[e->caseIndex]);
        Decl* d = e->a->type->decl;
        int at = 0;
        for (int f = 0; f < e->caseIndex; f++) at += qkTypeSlots(d->params[f].binding->type);
        Val part = {qkTypeSlots(e->type), v.r + at};
        return part;
    }
    case E_INDEX: {
        Val arr = lowerExpr(L, e->a);
        int i = lowerExpr(L, e->b).r[0];
        bounds(L, i, arr.r[1], PANIC_INDEX, e->line);
        return loadElem(L, arr, i, e->type);
    }
    case E_UNARY: {
        Val v = lowerExpr(L, e->a);
        if (e->op == T_NOT) return one(op2(L, IR_NOT, v.r[0], -1, 'I'));
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
    case E_LAMBDA: break;
    }
    return (Val){0, NULL};
}

/* ---- statements ---------------------------------------------------------------------------------- */

static Val lowerBlock(Lower* L, Block* b, Type* t) {
    bool value = t && t->kind != TY_VOID && t->kind != TY_ERROR;
    for (int i = 0; i < b->count; i++) {
        Stmt* s = b->stmts[i];
        if (value && i == b->count - 1 && s->kind == S_EXPR) return lowerStored(L, s->value);
        lowerStmt(L, s);
    }
    return (Val){0, NULL};
}

typedef struct ForCtx {
    Stmt* s;
} ForCtx;

static void forSink(Lower* L, Val elem, int index, void* ctx) {
    ForCtx* f = ctx;
    f->s->binding->regs = elem.r;
    if (f->s->indexBinding) f->s->indexBinding->regs = one(index).r;
    int savedBreak = L->userBreak, savedContinue = L->userContinue;
    L->userBreak = L->loopBreak;
    L->userContinue = L->loopContinue;
    lowerBlock(L, f->s->body, &qkTypeVoid);
    L->userBreak = savedBreak;
    L->userContinue = savedContinue;
}

/* Where a place's slots are: registers (a var), or memory (an element of a var array). */
typedef struct PlaceRef {
    bool memory;
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
    int at;
    if (e->a->type->kind == TY_VEC) {
        at = e->caseIndex;
    } else {
        Decl* d = e->a->type->decl;
        at = 0;
        for (int f = 0; f < e->caseIndex; f++) at += qkTypeSlots(d->params[f].binding->type);
    }
    if (outer.memory) {
        outer.offset += at * 8;
        return outer;
    }
    outer.regs = (Val){qkTypeSlots(e->type), outer.regs.r + at};
    return outer;
}

static void writePlace(Lower* L, PlaceRef p, Val v) {
    if (p.memory) storeSlots(L, p.base, p.index, p.stride, p.offset, v);
    else movVal(L, p.regs, v);
}

static void lowerAssign(Lower* L, Stmt* s) {
    Val v;
    if (s->op == T_ASSIGN) {
        v = lowerStored(L, s->value);
        Val fresh = copyVal(L, v); /* x = Vec2(x.y, x.x) reads before it writes */
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
    bool vecScalar = s->target->type->kind == TY_VEC && s->value->type->kind != TY_VEC;
    for (int i = 0; i < cur.n; i++) {
        bool f = L->fn->kinds[cur.r[i]] == 'F';
        int y = vecScalar ? v.r[0] : v.r[i];
        IrOp op;
        switch (s->op) {
        case T_PLUSEQ: op = f ? IR_FADD : IR_ADD; break;
        case T_MINUSEQ: op = f ? IR_FSUB : IR_SUB; break;
        case T_STAREQ: op = f ? IR_FMUL : IR_MUL; break;
        default: op = f ? IR_FDIV : IR_DIV;
        }
        r.r[i] = newReg(L, f ? 'F' : 'I');
        emit(L, op, r.r[i], cur.r[i], y)->line = s->line;
    }
    writePlace(L, p, r);
}

static void lowerStmt(Lower* L, Stmt* s) {
    switch (s->kind) {
    case S_LET: case S_VAR: {
        Binding* b = s->binding;
        if (b->lazy) return;
        Val v;
        Type* vt = s->value->type;
        if (vt->kind == TY_RANGE || vt->kind == TY_ITER) v = materialize(L, s->value, vt->elem, s->line);
        else v = lowerStored(L, s->value);
        Val c = valOf(L, b->type);
        movVal(L, c, v);
        b->regs = c.r;
        break;
    }
    case S_ASSIGN: lowerAssign(L, s); break;
    case S_EXPR: lowerExpr(L, s->value); break;
    case S_FOR: {
        ForCtx f = {s};
        forEach(L, s->iter, forSink, &f);
        break;
    }
    case S_WHILE: { /* the test at the bottom: one jump per turn */
        int top = label(L), test = label(L), done = label(L);
        L->depth++;
        jmp(L, test);
        place(L, top);
        fuel(L, s->line);
        int savedBreak = L->userBreak, savedContinue = L->userContinue;
        L->userBreak = done;
        L->userContinue = test;
        lowerBlock(L, s->body, &qkTypeVoid);
        L->userBreak = savedBreak;
        L->userContinue = savedContinue;
        place(L, test);
        jnz(L, lowerExpr(L, s->value).r[0], top);
        L->depth--;
        place(L, done);
        break;
    }
    case S_RETURN: {
        Val v = s->value ? lowerStored(L, s->value) : (Val){0, NULL};
        if (L->inlineDepth) { /* inside an inlined function: to its end */
            InlineFrame* fr = &L->inlining[L->inlineDepth - 1];
            if (v.n) movVal(L, fr->result, v);
            jmp(L, fr->end);
            break;
        }
        IrIns* ins = emit(L, IR_RET, -1, -1, -1);
        ins->args = v.r;
        ins->argCount = v.n;
        for (int i = 0; i < v.n; i++) use(L, v.r[i]);
        break;
    }
    case S_BREAK: jmp(L, L->userBreak); break;
    case S_CONTINUE: jmp(L, L->userContinue); break;
    }
}

/* ---- functions ----------------------------------------------------------------------------------- */

static void lowerFn(Lower* L, Decl* d, IrFn* f) {
    memset(f, 0, sizeof *f);
    f->name = d->name;
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
    f->resultCount = qkTypeSlots(d->retType);
    if (f->resultCount > f->maxOut) f->maxOut = f->resultCount; /* results come back where arguments went */
    if (total > f->maxOut) f->maxOut = total;
    fuel(L, d->line);
    Val v;
    if (d->body->kind == E_BLOCK) v = lowerBlock(L, d->body->block, d->retType);
    else v = lowerStored(L, d->body);
    IrIns* ret = emit(L, IR_RET, -1, -1, -1);
    if (f->resultCount && v.n) {
        ret->args = v.r;
        ret->argCount = v.n;
        for (int i = 0; i < v.n; i++) use(L, v.r[i]);
    }
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
        if (p->decls[i]->kind == D_FN && !p->decls[i]->isExtern) L.fnIndex[i] = n++;
    }
    out->fns = qkNew(sizeof(IrFn) * (size_t)(n ? n : 1));
    out->count = n;
    out->externs = qkNew(sizeof(IrExtern) * (size_t)(p->count ? p->count : 1));
    qkBufAdd(&out->data, "", 0);
    for (int i = 0; i < p->count; i++) {
        Decl* d = p->decls[i];
        if (d->kind != D_FN || d->isExtern) continue;
        lowerFn(&L, d, &out->fns[L.fnIndex[i]]);
        if (!strcmp(d->name, "main")) out->mainIndex = L.fnIndex[i];
    }
    return true;
}

/* ---- a readable dump, for `quark ir` ------------------------------------------------------------- */

void qkIrDump(const IrProgram* p, QkBuf* out) {
    static const char* const names[] = {"const", "mov", "add", "sub", "mul", "div", "mod", "neg", "fadd", "fsub", "fmul",
                                        "fdiv", "fneg", "sqrt", "fmin", "fmax", "cmp", "fcmp", "not", "i2f", "f2i", "round",
                                        "load", "store", "bounds", "label", "jmp", "jz", "jnz", "call", "rtcall", "ret", "fuel", "hostcall", "nop"};
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
            for (int j = 0; j < ins->argCount; j++) qkBufPrintf(out, "%s%c%d", j ? ", " : " (", f->kinds[ins->args[j]] == 'F' ? 'f' : 'r', ins->args[j]);
            if (ins->argCount) qkBufPuts(out, ")");
            for (int j = 0; j < ins->retCount; j++) qkBufPrintf(out, "%s%d", j ? ", r" : " -> r", ins->rets[j]);
            qkBufPuts(out, "\n");
        }
    }
}
