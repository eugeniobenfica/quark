/* Quark — the optimizing tier: quark run / build / object --release, qkVmRelease. The quick tier (opt.c) stays
 * the default, a fraction of a millisecond; this one takes longer to make code that runs faster.
 *
 * Each function is analysed as a control-flow graph of basic blocks, with its dominator tree, its natural loops
 * and the IR in SSA form (every value defined once, phis where control flow joins, copies folded into the
 * values they copy). The passes, in order:
 *  - escape analysis: a small record or array that never leaves the function (only loaded from and stored to at
 *    constant places) is not allocated: its fields live in registers;
 *  - devirtualization: a call through a function value that SSA shows is one known function is a direct call
 *    (an interface whose concrete type is known);
 *  - range analysis: every Int value's interval, refined on the branches that test it (i < n inside the loop),
 *    widened at loop heads; it drops the bounds checks that cannot fail (also r * 3 + 2 < len when r < len / 3)
 *    and the checks repeated where an identical one already passed, and the floored-division fix-ups (and zero
 *    checks) of divisions whose operands cannot be negative;
 *  - loop-invariant code motion: pure computations of values that do not change in a loop move before it;
 *  - SIMD: a counted loop over arrays (a fused map / filter / sum / ... pipeline on Int or Float) runs 4 (AVX2)
 *    or 2 (SSE, NEON) elements at a time, the rest in the original loop; Float sums keep their order (the same
 *    result as without SIMD), Int sums are added by lane;
 *  - strength reduction: i * k in a loop is a value of its own, increased by k when i is;
 *  - address folding: [base + (i + k) * 8] is [base + i * 8 + 8k].
 * Then the quick tier's passes (opt.c) and the backends' register allocation, as for every build. */
#include "qk.h"

#include <stdio.h>
#include <stdlib.h>

int qkSimdFeatures;
int qkSimdLanes;

/* ---- the IR's shape ------------------------------------------------------------------------------ */

static bool isBranch(int op) { return op == IR_JMP || op == IR_JZ || op == IR_JNZ; }

static bool definesReg(const IrIns* ins) {
    switch (ins->op) {
    case IR_STORE: case IR_BOUNDS: case IR_LABEL: case IR_JMP: case IR_JZ: case IR_JNZ: case IR_CALL: case IR_RTCALL:
    case IR_HOSTCALL: case IR_RET: case IR_FUEL: case IR_NOP: case IR_VSTORE: case IR_VEND:
        return false;
    default:
        return ins->dst >= 0;
    }
}

/* the registers an instruction reads: slot 0 its a, 1 its b, 2 a store's value, 3... its arguments */
static int useSlots(const IrIns* ins) { return 3 + ins->argCount; }
static int useReg(const IrIns* ins, int s) {
    if (s == 0) return ins->op == IR_CONST || ins->op == IR_LABEL || ins->op == IR_JMP ? -1 : ins->a;
    if (s == 1) return ins->op == IR_CONST || ins->op == IR_LABEL || ins->op == IR_JMP ? -1 : ins->b;
    if (s == 2) return ins->op == IR_STORE || ins->op == IR_VSTORE ? ins->dst : -1;
    return ins->args[s - 3];
}

static int newReg(IrFn* f, char kind, double weight) {
    if (f->vregCount == f->vregCap) {
        int cap = f->vregCap ? f->vregCap * 2 : 64;
        char* kinds = qkNew((size_t)cap);
        double* w = qkNew(sizeof(double) * (size_t)cap);
        memcpy(kinds, f->kinds, (size_t)f->vregCount);
        memcpy(w, f->weight, sizeof(double) * (size_t)f->vregCount);
        f->kinds = kinds;
        f->weight = w;
        f->vregCap = cap;
    }
    f->kinds[f->vregCount] = kind;
    f->weight[f->vregCount] = weight;
    return f->vregCount++;
}

static IrIns mk(int op, int dst, int a, int b) {
    IrIns ins;
    memset(&ins, 0, sizeof ins);
    ins.op = (uint8_t)op;
    ins.dst = dst;
    ins.a = a;
    ins.b = b;
    return ins;
}

/* instructions inserted before others, all at once: at[i] is the place (an instruction index, or count: the end) */
typedef struct Edit {
    IrIns* ins;
    int* at;
    int count, cap, atCap;
} Edit;

static void editAdd(Edit* e, int at, IrIns ins) {
    QK_PUSH(e->ins, e->count, e->cap, ins);
    e->count--;
    QK_PUSH(e->at, e->count, e->atCap, at);
}

static void editApply(IrFn* f, Edit* e) {
    if (!e->count) return;
    /* stable: inserted in the order added, before the instruction at their place */
    IrIns* code = qkNew(sizeof(IrIns) * (size_t)(f->count + e->count + 1));
    int m = 0;
    int* order = qkNew(sizeof(int) * (size_t)e->count);
    for (int i = 0; i < e->count; i++) order[i] = i;
    for (int i = 1; i < e->count; i++) { /* insertion sort by place, stable */
        int v = order[i], j = i - 1;
        while (j >= 0 && e->at[order[j]] > e->at[v]) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = v;
    }
    int next = 0;
    for (int k = 0; k <= f->count; k++) {
        while (next < e->count && e->at[order[next]] == k) code[m++] = e->ins[order[next++]];
        if (k < f->count && f->code[k].op != IR_NOP) code[m++] = f->code[k];
    }
    f->code = code;
    f->count = m;
    f->cap = m;
    e->count = 0;
}

static void dropNops(IrFn* f) {
    int m = 0;
    for (int k = 0; k < f->count; k++)
        if (f->code[k].op != IR_NOP) f->code[m++] = f->code[k];
    f->count = m;
}

/* ---- the control-flow graph, dominators, loops ----------------------------------------------------- */

typedef struct Blk {
    int start, end;      /* its instructions, [start, end) */
    int succ[2], nsucc;
    int* pred;
    int npred, cpred;
    int idom;            /* -1: the entry, or not reachable */
    int order;           /* reverse postorder number, -1: not reachable */
    int pre, post;       /* the dominator tree's numbering: a dominates b when a.pre <= b.pre and b.post <= a.post */
    int* kids;
    int nkids, ckids;
    int loop;            /* the innermost loop it is in, -1: none */
    int condIns;         /* entered only by one side of this branch (-1: no) */
    bool condTrue;       /* ... the side taken when the tested value is not zero */
    int* phis;           /* SSA values */
    int nphi, cphi;
    int* df;             /* dominance frontier */
    int ndf, cdf;
} Blk;

typedef struct Loop {
    int header;
    char* in;   /* per block */
    int size;
    int parent;
    int preAt;  /* where code before the loop goes (an instruction index), -1: nowhere */
    int preBlock;
} Loop;

enum { V_UNDEF, V_PARAM, V_INS, V_RET, V_PHI };

typedef struct Val {
    int kind;
    int ins;   /* V_INS, V_RET: the instruction; V_RET: index in rets is `blk` */
    int reg;
    int blk;
    int* args; /* V_PHI: per predecessor */
} Val;

typedef struct A {
    IrFn* f;
    int n;
    Blk* b;
    int nb;
    int* blockOf;
    int* labelBlock;
    int* rpo;
    int nrpo;
    Loop* loops;
    int nloops, cloops;
    int* defCount; /* per register: its definitions anywhere (and parameters) */
    /* SSA */
    Val* v;
    int nv, cv;
    int* useAt; /* per instruction: where its slots start in useV */
    int* useV;
    int* defV;  /* per instruction: the value its dst holds after it (-1: none) */
    int* retAt;
    int* retV;
    /* ranges */
    int64_t* lo;
    int64_t* hi;
} A;

static void addPred(Blk* b, int p) { QK_PUSH(b->pred, b->npred, b->cpred, p); }

static bool dominates(const A* a, int x, int y) {
    if (a->b[x].order < 0 || a->b[y].order < 0) return false;
    return a->b[x].pre <= a->b[y].pre && a->b[y].post <= a->b[x].post;
}

static void buildCfg(A* a) {
    IrFn* f = a->f;
    int n = a->n = f->count;
    a->blockOf = qkNew(sizeof(int) * (size_t)(n + 1));
    a->labelBlock = qkNew(sizeof(int) * (size_t)(f->labelCount + 1));
    for (int i = 0; i <= f->labelCount; i++) a->labelBlock[i] = -1;
    char* leader = qkNew((size_t)(n + 1));
    if (n) leader[0] = 1;
    for (int k = 0; k < n; k++) {
        int op = f->code[k].op;
        if (op == IR_LABEL && (k == 0 || f->code[k - 1].op != IR_LABEL)) leader[k] = 1;
        if ((isBranch(op) || op == IR_RET) && k + 1 < n) leader[k + 1] = 1;
    }
    int nb = 0;
    for (int k = 0; k < n; k++) nb += leader[k];
    a->nb = nb;
    a->b = qkNew(sizeof(Blk) * (size_t)(nb ? nb : 1));
    int cur = -1;
    for (int k = 0; k < n; k++) {
        if (leader[k]) {
            if (cur >= 0) a->b[cur].end = k;
            cur++;
            a->b[cur].start = k;
        }
        a->blockOf[k] = cur;
        if (f->code[k].op == IR_LABEL) a->labelBlock[f->code[k].imm] = cur;
    }
    if (cur >= 0) a->b[cur].end = n;
    for (int i = 0; i < nb; i++) {
        Blk* b = &a->b[i];
        b->idom = -1;
        b->order = -1;
        b->loop = -1;
        b->condIns = -1;
        const IrIns* last = &f->code[b->end - 1];
        if (last->op == IR_JMP) {
            b->succ[b->nsucc++] = a->labelBlock[last->imm];
        } else if (last->op == IR_JZ || last->op == IR_JNZ) {
            b->succ[b->nsucc++] = a->labelBlock[last->imm];
            if (i + 1 < nb) b->succ[b->nsucc++] = i + 1;
        } else if (last->op != IR_RET && i + 1 < nb) {
            b->succ[b->nsucc++] = i + 1;
        }
    }
    for (int i = 0; i < nb; i++)
        for (int s = 0; s < a->b[i].nsucc; s++)
            if (a->b[i].succ[s] >= 0) addPred(&a->b[a->b[i].succ[s]], i);
    if (!nb) return;
    /* reverse postorder, from the entry */
    a->rpo = qkNew(sizeof(int) * (size_t)nb);
    int* stack = qkNew(sizeof(int) * (size_t)nb);
    int* next = qkNew(sizeof(int) * (size_t)nb);
    char* seen = qkNew((size_t)nb);
    int* post = qkNew(sizeof(int) * (size_t)nb);
    int np = 0, sp = 0;
    stack[sp++] = 0;
    seen[0] = 1;
    while (sp) {
        int x = stack[sp - 1];
        if (next[x] < a->b[x].nsucc) {
            int y = a->b[x].succ[next[x]++];
            if (y >= 0 && !seen[y]) {
                seen[y] = 1;
                stack[sp++] = y;
            }
        } else {
            post[np++] = x;
            sp--;
        }
    }
    a->nrpo = np;
    for (int i = 0; i < np; i++) {
        a->rpo[i] = post[np - 1 - i];
        a->b[a->rpo[i]].order = i;
    }
    /* dominators (Cooper, Harvey, Kennedy) */
    int* idom = qkNew(sizeof(int) * (size_t)nb);
    for (int i = 0; i < nb; i++) idom[i] = -1;
    idom[0] = 0;
    for (bool changed = true; changed;) {
        changed = false;
        for (int r = 1; r < np; r++) {
            int x = a->rpo[r], nd = -1;
            for (int p = 0; p < a->b[x].npred; p++) {
                int y = a->b[x].pred[p];
                if (idom[y] < 0) continue;
                if (nd < 0) {
                    nd = y;
                    continue;
                }
                int u = y, w = nd;
                while (u != w) {
                    while (a->b[u].order > a->b[w].order) u = idom[u];
                    while (a->b[w].order > a->b[u].order) w = idom[w];
                }
                nd = u;
            }
            if (nd >= 0 && idom[x] != nd) {
                idom[x] = nd;
                changed = true;
            }
        }
    }
    for (int r = 1; r < np; r++) {
        int x = a->rpo[r];
        a->b[x].idom = idom[x];
        QK_PUSH(a->b[idom[x]].kids, a->b[idom[x]].nkids, a->b[idom[x]].ckids, x);
    }
    /* the dominator tree's numbering */
    int counter = 0;
    sp = 0;
    memset(next, 0, sizeof(int) * (size_t)nb);
    stack[sp++] = 0;
    a->b[0].pre = counter++;
    while (sp) {
        int x = stack[sp - 1];
        if (next[x] < a->b[x].nkids) {
            int y = a->b[x].kids[next[x]++];
            a->b[y].pre = counter++;
            stack[sp++] = y;
        } else {
            a->b[x].post = counter++;
            sp--;
        }
    }
    /* entered by one side of a branch */
    for (int i = 0; i < nb; i++) {
        Blk* b = &a->b[i];
        if (b->order < 0 || b->npred != 1) continue;
        const Blk* p = &a->b[b->pred[0]];
        const IrIns* last = &f->code[p->end - 1];
        if ((last->op != IR_JZ && last->op != IR_JNZ) || p->nsucc != 2 || p->succ[0] == p->succ[1]) continue;
        bool isTarget = p->succ[0] == i;
        b->condIns = p->end - 1;
        b->condTrue = (last->op == IR_JNZ) == isTarget;
    }
    /* natural loops */
    int* headerLoop = qkNew(sizeof(int) * (size_t)nb);
    for (int i = 0; i < nb; i++) headerLoop[i] = -1;
    int* work = qkNew(sizeof(int) * (size_t)(nb + 1));
    for (int u = 0; u < nb; u++) {
        if (a->b[u].order < 0) continue;
        for (int s = 0; s < a->b[u].nsucc; s++) {
            int h = a->b[u].succ[s];
            if (h < 0 || !dominates(a, h, u)) continue;
            int li = headerLoop[h];
            if (li < 0) {
                Loop L;
                memset(&L, 0, sizeof L);
                L.header = h;
                L.in = qkNew((size_t)nb);
                L.in[h] = 1;
                L.size = 1;
                L.parent = -1;
                QK_PUSH(a->loops, a->nloops, a->cloops, L);
                li = headerLoop[h] = a->nloops - 1;
            }
            Loop* L = &a->loops[li];
            int w = 0;
            if (!L->in[u]) {
                L->in[u] = 1;
                L->size++;
                work[w++] = u;
            }
            while (w) {
                int x = work[--w];
                for (int p = 0; p < a->b[x].npred; p++) {
                    int y = a->b[x].pred[p];
                    if (a->b[y].order < 0 || L->in[y]) continue;
                    L->in[y] = 1;
                    L->size++;
                    work[w++] = y;
                }
            }
        }
    }
    for (int i = 0; i < a->nloops; i++) {
        Loop* L = &a->loops[i];
        int best = -1;
        for (int j = 0; j < a->nloops; j++)
            if (j != i && a->loops[j].in[L->header] && a->loops[j].size > L->size && (best < 0 || a->loops[j].size < a->loops[best].size))
                best = j;
        L->parent = best;
        for (int x = 0; x < nb; x++)
            if (L->in[x] && (a->b[x].loop < 0 || a->loops[a->b[x].loop].size > L->size)) a->b[x].loop = i;
        /* the one way in from outside: code before the loop goes at its end */
        L->preAt = -1;
        L->preBlock = -1;
        int outside = 0, P = -1;
        const Blk* h = &a->b[L->header];
        for (int p = 0; p < h->npred; p++)
            if (!L->in[h->pred[p]] && a->b[h->pred[p]].order >= 0) outside++, P = h->pred[p];
        if (outside != 1) continue;
        const Blk* pb = &a->b[P];
        const IrIns* last = &f->code[pb->end - 1];
        int at = pb->end;
        if (isBranch(last->op)) {
            at = pb->end - 1;
            if (last->op != IR_JMP && at - 1 >= pb->start && (f->code[at - 1].op == IR_CMP || f->code[at - 1].op == IR_FCMP) &&
                f->code[at - 1].dst == last->a)
                at--;
        }
        L->preAt = at;
        L->preBlock = P;
    }
}

/* ---- SSA ----------------------------------------------------------------------------------------- */

static int newVal(A* a, int kind, int ins, int reg, int blk) {
    Val v;
    memset(&v, 0, sizeof v);
    v.kind = kind;
    v.ins = ins;
    v.reg = reg;
    v.blk = blk;
    QK_PUSH(a->v, a->nv, a->cv, v);
    return a->nv - 1;
}

static void countDefs(A* a) {
    IrFn* f = a->f;
    int nr = f->vregCount ? f->vregCount : 1;
    a->defCount = qkNew(sizeof(int) * (size_t)nr);
    for (int i = 0; i < f->paramCount; i++) a->defCount[f->params[i]]++;
    for (int k = 0; k < f->count; k++) {
        const IrIns* ins = &f->code[k];
        if (definesReg(ins)) a->defCount[ins->dst]++;
        for (int i = 0; i < ins->retCount; i++) a->defCount[ins->rets[i]]++;
    }
}

static void buildSsa(A* a) {
    IrFn* f = a->f;
    int nr = f->vregCount ? f->vregCount : 1, n = a->n, nb = a->nb;
    a->useAt = qkNew(sizeof(int) * (size_t)(n + 1));
    a->retAt = qkNew(sizeof(int) * (size_t)(n + 1));
    a->defV = qkNew(sizeof(int) * (size_t)(n + 1));
    int slots = 0, rets = 0;
    for (int k = 0; k < n; k++) {
        a->useAt[k] = slots;
        a->retAt[k] = rets;
        slots += useSlots(&f->code[k]);
        rets += f->code[k].retCount;
        a->defV[k] = -1;
    }
    a->useV = qkNew(sizeof(int) * (size_t)(slots + 1));
    a->retV = qkNew(sizeof(int) * (size_t)(rets + 1));
    newVal(a, V_UNDEF, -1, -1, -1); /* 0: no definition reaches */
    if (!nb) return;
    /* registers read in a block before it defines them: the ones phis are for */
    char* global = qkNew((size_t)nr);
    int* stamp = qkNew(sizeof(int) * (size_t)nr);
    for (int r = 0; r < nr; r++) stamp[r] = -1;
    int** defBlocks = qkNew(sizeof(int*) * (size_t)nr);
    int* ndb = qkNew(sizeof(int) * (size_t)nr);
    int* cdb = qkNew(sizeof(int) * (size_t)nr);
    for (int i = 0; i < f->paramCount; i++) {
        int r = f->params[i];
        QK_PUSH(defBlocks[r], ndb[r], cdb[r], 0);
    }
    for (int bi = 0; bi < nb; bi++) {
        Blk* b = &a->b[bi];
        if (b->order < 0) continue;
        for (int k = b->start; k < b->end; k++) {
            const IrIns* ins = &f->code[k];
            for (int s = 0; s < useSlots(ins); s++) {
                int r = useReg(ins, s);
                if (r >= 0 && stamp[r] != bi) global[r] = 1;
            }
            if (definesReg(ins)) {
                int r = ins->dst;
                if (stamp[r] != bi) QK_PUSH(defBlocks[r], ndb[r], cdb[r], bi);
                stamp[r] = bi;
            }
            for (int i = 0; i < ins->retCount; i++) {
                int r = ins->rets[i];
                if (stamp[r] != bi) QK_PUSH(defBlocks[r], ndb[r], cdb[r], bi);
                stamp[r] = bi;
            }
        }
    }
    /* dominance frontiers */
    for (int bi = 0; bi < nb; bi++) {
        Blk* b = &a->b[bi];
        if (b->order < 0 || b->npred < 2) continue;
        for (int p = 0; p < b->npred; p++) {
            int runner = b->pred[p];
            if (a->b[runner].order < 0) continue;
            while (runner >= 0 && runner != b->idom) {
                Blk* r = &a->b[runner];
                if (!r->ndf || r->df[r->ndf - 1] != bi) QK_PUSH(r->df, r->ndf, r->cdf, bi);
                runner = r->idom;
            }
        }
    }
    /* phis */
    int* hasPhi = qkNew(sizeof(int) * (size_t)nb);
    int* inWork = qkNew(sizeof(int) * (size_t)nb);
    for (int i = 0; i < nb; i++) hasPhi[i] = inWork[i] = -1;
    int* work = qkNew(sizeof(int) * (size_t)(nb + 1));
    for (int r = 0; r < f->vregCount; r++) {
        if (!global[r] || !ndb[r]) continue;
        int w = 0;
        for (int i = 0; i < ndb[r]; i++) {
            work[w++] = defBlocks[r][i];
            inWork[defBlocks[r][i]] = r;
        }
        while (w) {
            int d = work[--w];
            for (int j = 0; j < a->b[d].ndf; j++) {
                int y = a->b[d].df[j];
                if (hasPhi[y] == r) continue;
                hasPhi[y] = r;
                int pv = newVal(a, V_PHI, -1, r, y);
                a->v[pv].args = qkNew(sizeof(int) * (size_t)(a->b[y].npred ? a->b[y].npred : 1));
                QK_PUSH(a->b[y].phis, a->b[y].nphi, a->b[y].cphi, pv);
                if (inWork[y] != r) {
                    inWork[y] = r;
                    work[w++] = y;
                }
            }
        }
    }
    /* renaming, down the dominator tree */
    int* cur = qkNew(sizeof(int) * (size_t)nr);
    for (int i = 0; i < f->paramCount; i++) cur[f->params[i]] = newVal(a, V_PARAM, i, f->params[i], 0);
    int logCap = 64, logLen = 0;
    int* logReg = malloc(sizeof(int) * (size_t)logCap);
    int* logOld = malloc(sizeof(int) * (size_t)logCap);
#define SET(r, val)                                                         \
    do {                                                                    \
        if (logLen == logCap) {                                             \
            logCap *= 2;                                                    \
            logReg = realloc(logReg, sizeof(int) * (size_t)logCap);         \
            logOld = realloc(logOld, sizeof(int) * (size_t)logCap);         \
        }                                                                   \
        logReg[logLen] = (r);                                               \
        logOld[logLen++] = cur[r];                                          \
        cur[r] = (val);                                                     \
    } while (0)
    int* stack = qkNew(sizeof(int) * (size_t)(2 * nb + 2));
    int* mark = qkNew(sizeof(int) * (size_t)(nb + 1));
    int sp = 0;
    stack[sp++] = 0;
    while (sp) {
        int e = stack[--sp];
        if (e < 0) { /* leaving block -e - 1 */
            int bi = -e - 1;
            while (logLen > mark[bi]) {
                logLen--;
                cur[logReg[logLen]] = logOld[logLen];
            }
            continue;
        }
        Blk* b = &a->b[e];
        mark[e] = logLen;
        for (int i = 0; i < b->nphi; i++) SET(a->v[b->phis[i]].reg, b->phis[i]);
        for (int k = b->start; k < b->end; k++) {
            const IrIns* ins = &f->code[k];
            for (int s = 0; s < useSlots(ins); s++) {
                int r = useReg(ins, s);
                a->useV[a->useAt[k] + s] = r >= 0 ? cur[r] : -1;
            }
            if (ins->op == IR_MOV && ins->a >= 0 && f->kinds[ins->a] == f->kinds[ins->dst]) {
                int val = cur[ins->a];
                a->defV[k] = val;
                SET(ins->dst, val);
            } else if (definesReg(ins)) {
                int val = newVal(a, V_INS, k, ins->dst, e);
                a->defV[k] = val;
                SET(ins->dst, val);
            }
            for (int i = 0; i < ins->retCount; i++) {
                int val = newVal(a, V_RET, k, ins->rets[i], i);
                a->retV[a->retAt[k] + i] = val;
                SET(ins->rets[i], val);
            }
        }
        for (int s = 0; s < b->nsucc; s++) {
            int y = b->succ[s];
            if (y < 0) continue;
            Blk* yb = &a->b[y];
            for (int p = 0; p < yb->npred; p++) {
                if (yb->pred[p] != e) continue;
                for (int i = 0; i < yb->nphi; i++) a->v[yb->phis[i]].args[p] = cur[a->v[yb->phis[i]].reg];
            }
        }
        stack[sp++] = -e - 1;
        for (int i = b->nkids - 1; i >= 0; i--) stack[sp++] = b->kids[i];
    }
#undef SET
    free(logReg);
    free(logOld);
}

static A* analyse(IrFn* f) {
    A* a = qkNew(sizeof(A));
    a->f = f;
    buildCfg(a);
    countDefs(a);
    buildSsa(a);
    return a;
}

/* the value's instruction, when it is one */
static const IrIns* defIns(const A* a, int v) {
    if (v < 0 || a->v[v].kind != V_INS) return NULL;
    return &a->f->code[a->v[v].ins];
}

static bool constVal(const A* a, int v, int64_t* k) {
    const IrIns* d = defIns(a, v);
    if (!d || d->op != IR_CONST || d->c >= 1 || a->f->kinds[d->dst] != 'I') return false;
    *k = d->imm;
    return true;
}

static int useOf(const A* a, int k, int slot) { return a->useV[a->useAt[k] + slot]; }

/* ---- escape analysis: records that never leave, in registers --------------------------------------- */

static bool scalarReplace(A* a) {
    IrFn* f = a->f;
    bool changed = false;
    int nr = f->vregCount;
    for (int k = 0; k < f->count; k++) {
        IrIns* al = &f->code[k];
        if (al->op != IR_RTCALL || al->imm != RT_ALLOC || al->argCount != 2 || al->retCount != 1) continue;
        if (a->blockOf[k] < 0 || a->b[a->blockOf[k]].order < 0) continue;
        int64_t count, each;
        if (!constVal(a, useOf(a, k, 3), &count) || !constVal(a, useOf(a, k, 4), &each)) continue;
        if (count <= 0 || each <= 0 || count * each > 256 || (count * each) % 8) continue;
        int slots = (int)(count * each / 8);
        int p = al->rets[0];
        if (a->defCount[p] != 1) continue;
        /* the registers that hold the pointer: p, and copies of it (each set once) */
        char* alias = qkNew((size_t)nr);
        alias[p] = 1;
        for (bool grew = true; grew;) {
            grew = false;
            for (int j = 0; j < f->count; j++) {
                const IrIns* m = &f->code[j];
                if (m->op == IR_MOV && alias[m->a] && !alias[m->dst]) {
                    alias[m->dst] = 1;
                    grew = true;
                }
            }
        }
        bool ok = true;
        int blk = a->blockOf[k];
        bool inLoop = a->b[blk].loop >= 0;
        char* kind = qkNew((size_t)slots);
        for (int j = 0; j < f->count && ok; j++) {
            const IrIns* m = &f->code[j];
            bool mentions = false;
            for (int s = 0; s < useSlots(m); s++)
                if (useReg(m, s) >= 0 && alias[useReg(m, s)]) mentions = true;
            bool defsAlias = definesReg(m) && alias[m->dst] && j != k;
            if (!mentions && !defsAlias) continue;
            if (inLoop && (a->blockOf[j] != blk || j < k)) ok = false;
            if (m->op == IR_MOV && alias[m->a] && alias[m->dst]) {
                if (a->defCount[m->dst] != 1) ok = false;
                continue;
            }
            if (defsAlias) {
                ok = false;
                break;
            }
            int64_t idx;
            if ((m->op == IR_LOAD || m->op == IR_STORE) && alias[m->a] && m->b >= 0 && !alias[m->b] &&
                !(m->op == IR_STORE && alias[m->dst]) && constVal(a, useOf(a, j, 1), &idx)) {
                int64_t off = idx * m->imm + m->c;
                if (off < 0 || off % 8 || off / 8 >= slots) {
                    ok = false;
                    break;
                }
                char kd = f->kinds[m->dst];
                if (kind[off / 8] && kind[off / 8] != kd) ok = false;
                kind[off / 8] = kd;
                continue;
            }
            ok = false;
        }
        if (!ok) continue;
        /* fields in registers, zero at first (the memory would have been) */
        int* field = qkNew(sizeof(int) * (size_t)slots);
        Edit e = {0};
        for (int s = 0; s < slots; s++) {
            field[s] = newReg(f, kind[s] ? kind[s] : 'I', f->weight[p]);
            IrIns c = mk(IR_CONST, field[s], -1, -1);
            c.line = al->line;
            editAdd(&e, k, c);
        }
        for (int j = 0; j < f->count; j++) {
            IrIns* m = &f->code[j];
            if (j == k) continue;
            if (m->op == IR_MOV && alias[m->dst]) {
                m->op = IR_NOP;
                continue;
            }
            if ((m->op == IR_LOAD || m->op == IR_STORE) && m->a >= 0 && alias[m->a]) {
                int64_t idx = 0;
                constVal(a, useOf(a, j, 1), &idx);
                int s = (int)((idx * m->imm + m->c) / 8);
                if (m->op == IR_LOAD) *m = (IrIns){.op = IR_MOV, .dst = m->dst, .a = field[s], .b = -1, .c = 0, .line = m->line, .inl = m->inl};
                else *m = (IrIns){.op = IR_MOV, .dst = field[s], .a = m->dst, .b = -1, .c = 0, .line = m->line, .inl = m->inl};
            }
        }
        f->code[k].op = IR_NOP;
        editApply(f, &e);
        return true; /* the analysis is stale: once more from the start */
    }
    return changed;
}

/* ---- devirtualization ---------------------------------------------------------------------------- */

static bool devirtualize(A* a) {
    bool changed = false;
    for (int k = 0; k < a->n; k++) {
        IrIns* ins = &a->f->code[k];
        if (ins->op != IR_CALL || ins->a < 0 || a->b[a->blockOf[k]].order < 0) continue;
        const IrIns* d = defIns(a, useOf(a, k, 0));
        if (!d || d->op != IR_CONST || d->c != 2) continue;
        ins->imm = d->imm; /* the one function it can be: called directly */
        ins->a = -1;
        changed = true;
    }
    return changed;
}

/* ---- range analysis ------------------------------------------------------------------------------ */

#define LO_MIN INT64_MIN
#define HI_MAX INT64_MAX

static bool isInt(const A* a, int v) { return v > 0 && a->v[v].reg >= 0 && a->f->kinds[a->v[v].reg] == 'I'; }

/* the relation a REL b, as cond; on the false side, its negation */
static int negateCond(int c) {
    static const int neg[] = {C_NE, C_EQ, C_GE, C_GT, C_LE, C_LT};
    return neg[c];
}
static int swapCond(int c) {
    static const int sw[] = {C_EQ, C_NE, C_GT, C_GE, C_LT, C_LE};
    return sw[c];
}

/* What the branch block `bi` is entered by says about value x: x REL y (returns false when nothing) */
static bool edgeFact(const A* a, int bi, int x, int* rel, int* y) {
    const Blk* b = &a->b[bi];
    if (b->condIns < 0) return false;
    int t = useOf(a, b->condIns, 0);
    bool truth = b->condTrue;
    const IrIns* c = defIns(a, t);
    while (c && c->op == IR_NOT) { /* !x */
        truth = !truth;
        t = useOf(a, a->v[t].ins, 0);
        c = defIns(a, t);
    }
    if (!c || c->op != IR_CMP) return false;
    int k = a->v[t].ins;
    int u = useOf(a, k, 0), w = useOf(a, k, 1);
    int cond = truth ? c->cond : negateCond(c->cond);
    if (u == x) {
        *rel = cond;
        *y = w;
        return true;
    }
    if (w == x) {
        *rel = swapCond(cond);
        *y = u;
        return true;
    }
    return false;
}

static void rangeOf(const A* a, int v, int64_t* lo, int64_t* hi) {
    if (v <= 0 || !isInt(a, v)) {
        *lo = LO_MIN;
        *hi = HI_MAX;
        return;
    }
    *lo = a->lo[v];
    *hi = a->hi[v];
}

/* v's range where block bi runs: its own, narrowed by the branches that lead there */
static void rangeAt(const A* a, int v, int bi, int64_t* lo, int64_t* hi) {
    rangeOf(a, v, lo, hi);
    if (*lo > *hi) return;
    for (int d = bi, depth = 0; d >= 0 && depth < 48; d = a->b[d].idom, depth++) {
        int rel, y;
        if (!edgeFact(a, d, v, &rel, &y)) continue;
        int64_t ylo, yhi;
        rangeOf(a, y, &ylo, &yhi);
        if (ylo > yhi) continue;
        switch (rel) {
        case C_LT: if (yhi != LO_MIN && yhi - 1 < *hi) *hi = yhi - 1; break;
        case C_LE: if (yhi < *hi) *hi = yhi; break;
        case C_GT: if (ylo != HI_MAX && ylo + 1 > *lo) *lo = ylo + 1; break;
        case C_GE: if (ylo > *lo) *lo = ylo; break;
        case C_EQ:
            if (ylo > *lo) *lo = ylo;
            if (yhi < *hi) *hi = yhi;
            break;
        default: break;
        }
    }
}

static int64_t floorDiv(int64_t x, int64_t d) {
    int64_t q = x / d;
    if ((x % d != 0) && ((x < 0) != (d < 0))) q--;
    return q;
}

static void transfer(const A* a, int k, int bi, int64_t* lo, int64_t* hi) {
    const IrIns* ins = &a->f->code[k];
    *lo = LO_MIN;
    *hi = HI_MAX;
    int64_t alo = 0, ahi = 0, blo = 0, bhi = 0;
    if (ins->op != IR_CONST) {
        if (ins->a >= 0) rangeAt(a, useOf(a, k, 0), bi, &alo, &ahi);
        if (ins->b >= 0) rangeAt(a, useOf(a, k, 1), bi, &blo, &bhi);
        if ((ins->a >= 0 && alo > ahi) || (ins->b >= 0 && blo > bhi)) { /* not known yet */
            *lo = 1;
            *hi = 0;
            return;
        }
    }
    int64_t x, y;
    switch (ins->op) {
    case IR_CONST:
        if (ins->c < 1) *lo = *hi = ins->imm;
        break;
    case IR_ADD:
        if (!__builtin_add_overflow(alo, blo, &x) && !__builtin_add_overflow(ahi, bhi, &y)) *lo = x, *hi = y;
        break;
    case IR_SUB:
        if (!__builtin_sub_overflow(alo, bhi, &x) && !__builtin_sub_overflow(ahi, blo, &y)) *lo = x, *hi = y;
        break;
    case IR_MUL: {
        int64_t c[4];
        if (__builtin_mul_overflow(alo, blo, &c[0]) || __builtin_mul_overflow(alo, bhi, &c[1]) ||
            __builtin_mul_overflow(ahi, blo, &c[2]) || __builtin_mul_overflow(ahi, bhi, &c[3]))
            break;
        *lo = *hi = c[0];
        for (int i = 1; i < 4; i++) {
            if (c[i] < *lo) *lo = c[i];
            if (c[i] > *hi) *hi = c[i];
        }
        break;
    }
    case IR_NEG:
        if (alo != LO_MIN) *lo = -ahi, *hi = -alo;
        break;
    case IR_DIV:
        if (blo >= 1) {
            int64_t p = floorDiv(alo, blo), q = floorDiv(alo, bhi), r = floorDiv(ahi, blo), s = floorDiv(ahi, bhi);
            *lo = p < q ? p : q;
            *hi = r > s ? r : s;
        }
        break;
    case IR_MOD:
        if (blo >= 1) {
            *lo = 0;
            *hi = bhi - 1;
            if (alo >= 0 && ahi < *hi) *hi = ahi;
        }
        break;
    case IR_AND:
        if (alo >= 0 && blo >= 0) *lo = 0, *hi = ahi < bhi ? ahi : bhi;
        else if (alo >= 0) *lo = 0, *hi = ahi;
        else if (blo >= 0) *lo = 0, *hi = bhi;
        break;
    case IR_OR: case IR_XOR:
        if (alo >= 0 && blo >= 0) {
            int64_t m = ahi > bhi ? ahi : bhi, p = 1;
            while (p <= m && p < (INT64_C(1) << 62)) p <<= 1;
            if (p > m) *lo = 0, *hi = 2 * p - 1 < 0 ? HI_MAX : p - 1;
        }
        break;
    case IR_SAR:
        if (blo == bhi && blo >= 0 && blo < 64) *lo = alo >> blo, *hi = ahi >> blo;
        break;
    case IR_CMP: case IR_FCMP: case IR_NOT:
        *lo = 0;
        *hi = 1;
        break;
    default: break;
    }
}

static void ranges(A* a) {
    int nv = a->nv;
    a->lo = qkNew(sizeof(int64_t) * (size_t)(nv + 1));
    a->hi = qkNew(sizeof(int64_t) * (size_t)(nv + 1));
    int* visits = qkNew(sizeof(int) * (size_t)(nv + 1));
    for (int v = 0; v < nv; v++) {
        int kind = a->v[v].kind;
        if (kind == V_PHI || kind == V_INS) a->lo[v] = 1, a->hi[v] = 0; /* nothing known yet: empty */
        else a->lo[v] = LO_MIN, a->hi[v] = HI_MAX;
    }
    IrFn* f = a->f;
    int changed = 1;
    for (int round = 0; changed && round < 40; round++) {
        changed = 0;
        for (int r = 0; r < a->nrpo; r++) {
            int bi = a->rpo[r];
            Blk* b = &a->b[bi];
            for (int i = 0; i < b->nphi; i++) {
                int pv = b->phis[i];
                if (!isInt(a, pv)) continue;
                int64_t lo = 1, hi = 0;
                for (int p = 0; p < b->npred; p++) {
                    if (a->b[b->pred[p]].order < 0) continue;
                    int64_t l, h;
                    rangeAt(a, a->v[pv].args[p], b->pred[p], &l, &h);
                    if (l > h) continue;
                    if (lo > hi) lo = l, hi = h;
                    else {
                        if (l < lo) lo = l;
                        if (h > hi) hi = h;
                    }
                }
                if (lo > hi) continue;
                int64_t olo = a->lo[pv], ohi = a->hi[pv];
                if (olo <= ohi) { /* grows only; widened after a few rounds */
                    if (lo > olo) lo = olo;
                    if (hi < ohi) hi = ohi;
                    if (++visits[pv] > 3) {
                        if (lo < olo) lo = LO_MIN;
                        if (hi > ohi) hi = HI_MAX;
                    }
                }
                if (lo != olo || hi != ohi) {
                    a->lo[pv] = lo;
                    a->hi[pv] = hi;
                    changed = 1;
                }
            }
            for (int k = b->start; k < b->end; k++) {
                int dv = a->defV[k];
                if (dv <= 0 || a->v[dv].kind != V_INS || a->v[dv].ins != k || f->kinds[f->code[k].dst] != 'I') continue;
                int64_t lo, hi;
                transfer(a, k, bi, &lo, &hi);
                if (lo > hi) continue;
                int64_t olo = a->lo[dv], ohi = a->hi[dv];
                if (olo <= ohi) {
                    if (lo > olo) lo = olo;
                    if (hi < ohi) hi = ohi;
                }
                if (lo != olo || hi != ohi) {
                    a->lo[dv] = lo;
                    a->hi[dv] = hi;
                    changed = 1;
                }
            }
        }
    }
    for (int v = 0; v < nv; v++) /* never reached (or not settled): anything */
        if (a->lo[v] > a->hi[v] || changed) a->lo[v] = LO_MIN, a->hi[v] = HI_MAX;
}

/* x < len wherever block bi runs, from the branches that lead there (and x = y - k, x = r * k + c with
 * r < len / k) */
static bool lessThan(const A* a, int x, int len, int bi, int depth) {
    if (depth > 4 || x <= 0) return false;
    int64_t xlo, xhi, llo, lhi;
    rangeAt(a, x, bi, &xlo, &xhi);
    rangeAt(a, len, bi, &llo, &lhi);
    if (xlo < 0) return false;
    for (int d = bi, steps = 0; d >= 0 && steps < 48; d = a->b[d].idom, steps++) {
        int rel, y;
        if (!edgeFact(a, d, x, &rel, &y)) continue;
        if (rel == C_LT && y == len) return true;
        if (rel == C_LT) { /* x < y, y = len / k (k >= 1, len >= 0 when x >= 0 is below it): y <= len */
            const IrIns* q = defIns(a, y);
            int64_t k;
            if (q && q->op == IR_DIV && useOf(a, a->v[y].ins, 0) == len && constVal(a, useOf(a, a->v[y].ins, 1), &k) && k >= 1) return true;
        }
        if (rel == C_LE) { /* x <= len - 1 */
            const IrIns* s = defIns(a, y);
            int64_t k;
            if (llo < 0) continue; /* len - k does not wrap */
            if (s && s->op == IR_SUB && useOf(a, a->v[y].ins, 0) == len && constVal(a, useOf(a, a->v[y].ins, 1), &k) && k >= 1) return true;
            if (s && s->op == IR_ADD && useOf(a, a->v[y].ins, 0) == len && constVal(a, useOf(a, a->v[y].ins, 1), &k) && k <= -1 && k > LO_MIN) return true;
        }
    }
    const IrIns* d = defIns(a, x);
    if (!d) return false;
    int k = a->v[x].ins;
    int u = useOf(a, k, 0), w = useOf(a, k, 1);
    int64_t c;
    /* x = y + c (c <= 0) or y - c (c >= 0): x <= y */
    if (d->op == IR_ADD && constVal(a, w, &c) && c <= 0) return lessThan(a, u, len, bi, depth + 1);
    if (d->op == IR_ADD && constVal(a, u, &c) && c <= 0) return lessThan(a, w, len, bi, depth + 1);
    if (d->op == IR_SUB && constVal(a, w, &c) && c >= 0) return lessThan(a, u, len, bi, depth + 1);
    /* x = r * m + c, 0 <= c < m, 0 <= r < len / m: x <= (len / m - 1) * m + m - 1 < len */
    int mulV = -1;
    int64_t clo = 0, chi = 0;
    if (d->op == IR_MUL) {
        mulV = x;
    } else if (d->op == IR_ADD) {
        const IrIns* du = defIns(a, u);
        const IrIns* dw = defIns(a, w);
        if (du && du->op == IR_MUL) mulV = u, rangeAt(a, w, bi, &clo, &chi);
        else if (dw && dw->op == IR_MUL) mulV = w, rangeAt(a, u, bi, &clo, &chi);
    }
    if (mulV < 0) return false;
    int mk_ = a->v[mulV].ins;
    int r = useOf(a, mk_, 0), mv = useOf(a, mk_, 1);
    int64_t m;
    if (!constVal(a, mv, &m)) {
        int t = r;
        r = mv;
        mv = t;
        if (!constVal(a, mv, &m)) return false;
    }
    if (m < 1 || clo < 0 || chi >= m) return false;
    int64_t rlo, rhi;
    rangeAt(a, r, bi, &rlo, &rhi);
    if (rlo < 0) return false;
    for (int dd = bi, steps = 0; dd >= 0 && steps < 48; dd = a->b[dd].idom, steps++) {
        int rel, y;
        if (!edgeFact(a, dd, r, &rel, &y) || rel != C_LT) continue;
        const IrIns* q = defIns(a, y);
        int64_t qk;
        if (q && q->op == IR_DIV && useOf(a, a->v[y].ins, 0) == len && constVal(a, useOf(a, a->v[y].ins, 1), &qk) && qk == m) return true;
    }
    return false;
}

typedef struct Check {
    int idx, len, blk, at;
} Check;

static bool boundsAndDivisions(A* a) {
    ranges(a);
    IrFn* f = a->f;
    bool changed = false;
    Check* done = NULL;
    int ndone = 0, cdone = 0;
    for (int r = 0; r < a->nrpo; r++) {
        int bi = a->rpo[r];
        for (int k = a->b[bi].start; k < a->b[bi].end; k++) {
            IrIns* ins = &f->code[k];
            if (ins->op == IR_BOUNDS) {
                int x = useOf(a, k, 0), len = useOf(a, k, 1);
                int64_t xlo, xhi, llo, lhi;
                rangeAt(a, x, bi, &xlo, &xhi);
                rangeAt(a, len, bi, &llo, &lhi);
                bool safe = xlo >= 0 && (xhi < llo || lessThan(a, x, len, bi, 0));
                int64_t kx;
                for (int i = 0; i < ndone && !safe; i++) { /* an identical check (or one of a constant as big) passed */
                    const Check* c = &done[i];
                    if (c->len != len || !(c->blk == bi ? c->at < k : dominates(a, c->blk, bi))) continue;
                    int64_t kc;
                    if (c->idx == x || (constVal(a, x, &kx) && kx >= 0 && constVal(a, c->idx, &kc) && kc >= kx)) safe = true;
                }
                if (safe) {
                    ins->op = IR_NOP;
                    changed = true;
                } else {
                    Check c = {x, len, bi, k};
                    QK_PUSH(done, ndone, cdone, c);
                }
            } else if ((ins->op == IR_DIV || ins->op == IR_MOD) && !(ins->cond & 2)) {
                int64_t xlo, xhi, ylo, yhi;
                rangeAt(a, useOf(a, k, 0), bi, &xlo, &xhi);
                rangeAt(a, useOf(a, k, 1), bi, &ylo, &yhi);
                if (xlo >= 0 && ylo >= 1) { /* truncated is floored, and no zero to check */
                    ins->cond |= 2;
                    changed = true;
                }
            }
        }
    }
    return changed;
}

/* ---- loop-invariant code motion ------------------------------------------------------------------ */

static bool hoistable(const A* a, const IrIns* ins, int k) {
    switch (ins->op) {
    case IR_CONST: return true;
    case IR_ADD: case IR_SUB: case IR_MUL: return ins->cond == 0;
    case IR_NEG: case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SAR: case IR_FADD: case IR_FSUB: case IR_FMUL:
    case IR_FDIV: case IR_FNEG: case IR_SQRT: case IR_FMIN: case IR_FMAX: case IR_CMP: case IR_FCMP: case IR_NOT: case IR_I2F:
    case IR_ROUND:
        return true;
    case IR_F2I: return ins->cond == 0;
    case IR_DIV: case IR_MOD: { /* by a constant other than 0: nothing to stop for */
        int64_t d;
        return constVal(a, useOf(a, k, 1), &d) && d != 0;
    }
    default: return false;
    }
}

static bool licm(A* a) {
    IrFn* f = a->f;
    int nr = f->vregCount ? f->vregCount : 1;
    int* defIn = qkNew(sizeof(int) * (size_t)nr);
    char* moved = qkNew((size_t)(f->count + 1));
    Edit e = {0};
    for (int li = 0; li < a->nloops; li++) {
        Loop* L = &a->loops[li];
        if (L->preAt < 0) continue;
        memset(defIn, 0, sizeof(int) * (size_t)nr);
        for (int bi = 0; bi < a->nb; bi++) {
            if (!L->in[bi]) continue;
            for (int k = a->b[bi].start; k < a->b[bi].end; k++) {
                const IrIns* ins = &f->code[k];
                if (definesReg(ins)) defIn[ins->dst]++;
                for (int i = 0; i < ins->retCount; i++) defIn[ins->rets[i]]++;
            }
        }
        if (L->preAt < f->count && (f->code[L->preAt].op == IR_CMP || f->code[L->preAt].op == IR_FCMP)) defIn[f->code[L->preAt].dst]++;
        bool any = false;
        for (bool grew = true; grew;) {
            grew = false;
            for (int k = 0; k < f->count; k++) {
                IrIns* ins = &f->code[k];
                if (moved[k] || !L->in[a->blockOf[k]] || !hoistable(a, ins, k) || a->defCount[ins->dst] != 1) continue;
                bool inv = true;
                for (int s = 0; s < 2; s++) {
                    int r = useReg(ins, s);
                    if (r >= 0 && defIn[r]) inv = false;
                }
                if (!inv) continue;
                moved[k] = 1;
                defIn[ins->dst] = 0;
                grew = any = true;
            }
        }
        if (!any) continue;
        for (int k = 0; k < f->count; k++)
            if (moved[k] && L->in[a->blockOf[k]] && f->code[k].op != IR_NOP) {
                editAdd(&e, L->preAt, f->code[k]);
                f->code[k].op = IR_NOP;
            }
        editApply(f, &e);
        return true; /* one loop at a time: the graph changed */
    }
    return false;
}

/* ---- strength reduction -------------------------------------------------------------------------- */

static bool strengthReduce(A* a) {
    IrFn* f = a->f;
    int nr = f->vregCount ? f->vregCount : 1;
    int* defIn = qkNew(sizeof(int) * (size_t)nr);
    int* defAt = qkNew(sizeof(int) * (size_t)nr);
    for (int li = 0; li < a->nloops; li++) {
        Loop* L = &a->loops[li];
        if (L->preAt < 0) continue;
        memset(defIn, 0, sizeof(int) * (size_t)nr);
        for (int bi = 0; bi < a->nb; bi++) {
            if (!L->in[bi]) continue;
            for (int k = a->b[bi].start; k < a->b[bi].end; k++) {
                const IrIns* ins = &f->code[k];
                if (definesReg(ins)) defIn[ins->dst]++, defAt[ins->dst] = k;
                for (int i = 0; i < ins->retCount; i++) defIn[ins->rets[i]] += 2;
            }
        }
        for (int bi = 0; bi < a->nb; bi++) {
            if (!L->in[bi]) continue;
            for (int k = a->b[bi].start; k < a->b[bi].end; k++) {
                IrIns* ins = &f->code[k];
                if ((ins->op != IR_MUL && ins->op != IR_SHL) || ins->cond || f->kinds[ins->dst] != 'I' || a->defCount[ins->dst] != 1) continue;
                int64_t m;
                int i = -1;
                if (constVal(a, useOf(a, k, 1), &m)) i = ins->a;
                else if (ins->op == IR_MUL && constVal(a, useOf(a, k, 0), &m)) i = ins->b;
                if (i < 0 || defIn[i] != 1) continue;
                if (ins->op == IR_SHL) {
                    if (m < 0 || m > 62) continue;
                    m = INT64_C(1) << m;
                }
                if (m == 1 || m == 0 || m == 2 || m == 4 || m == 8) continue; /* free in an address, or cheaper as it is */
                /* i: a basic induction value, i = i + s once in the loop */
                int ik = defAt[i];
                IrIns* inc = &f->code[ik];
                int64_t s;
                if (inc->op != IR_ADD || inc->cond) continue;
                if (!((inc->a == i && constVal(a, useOf(a, ik, 1), &s)) || (inc->b == i && constVal(a, useOf(a, ik, 0), &s)))) continue;
                double w = f->weight[ins->dst];
                int t2 = newReg(f, 'I', w), km = newReg(f, 'I', 0), ks = newReg(f, 'I', 0);
                Edit e = {0};
                IrIns c1 = mk(IR_CONST, km, -1, -1);
                c1.imm = m;
                IrIns c2 = mk(IR_CONST, ks, -1, -1);
                c2.imm = (int64_t)((uint64_t)s * (uint64_t)m);
                IrIns init = mk(IR_MUL, t2, i, km);
                init.line = ins->line;
                editAdd(&e, L->preAt, c1);
                editAdd(&e, L->preAt, c2);
                editAdd(&e, L->preAt, init);
                IrIns step = mk(IR_ADD, t2, t2, ks);
                step.line = inc->line;
                editAdd(&e, ik + 1, step);
                /* t = i * m is t2, read in place when no step of i comes between (same block, after it) */
                int t = ins->dst;
                bool rename = true;
                int blk = a->blockOf[k];
                for (int j = 0; j < f->count && rename; j++) {
                    const IrIns* u = &f->code[j];
                    for (int sl = 0; sl < useSlots(u); sl++)
                        if (useReg(u, sl) == t && (a->blockOf[j] != blk || j < k || (ik > k && ik < j && a->blockOf[ik] == blk))) rename = false;
                }
                if (rename) {
                    for (int j = k + 1; j < a->b[blk].end; j++) {
                        IrIns* u = &f->code[j];
                        if (u->op != IR_CONST) {
                            if (u->a == t) u->a = t2;
                            if (u->b == t) u->b = t2;
                        }
                        if ((u->op == IR_STORE || u->op == IR_VSTORE) && u->dst == t) u->dst = t2;
                        for (int q = 0; q < u->argCount; q++)
                            if (u->args[q] == t) u->args[q] = t2;
                    }
                    ins->op = IR_NOP;
                } else {
                    *ins = mk(IR_MOV, t, t2, -1);
                    ins->line = init.line;
                }
                editApply(f, &e);
                return true;
            }
        }
    }
    return false;
}

/* ---- address folding: [b + (x + k) * s + c] is [b + x * s + c + k * s] ------------------------------ */

static bool foldAddresses(A* a) {
    IrFn* f = a->f;
    bool changed = false;
    for (int k = 0; k < f->count; k++) {
        IrIns* m = &f->code[k];
        if (m->op != IR_LOAD && m->op != IR_STORE) continue;
        if (m->b < 0) continue;
        const IrIns* d = defIns(a, useOf(a, k, 1));
        if (!d || (d->op != IR_ADD && d->op != IR_SUB) || d->cond) continue;
        int dk = a->v[useOf(a, k, 1)].ins;
        if (a->blockOf[dk] != a->blockOf[k] || dk > k) continue;
        int64_t c;
        int x;
        if (constVal(a, useOf(a, dk, 1), &c)) x = d->a;
        else if (d->op == IR_ADD && constVal(a, useOf(a, dk, 0), &c)) x = d->b;
        else continue;
        if (d->op == IR_SUB) c = -c;
        int64_t off;
        if (__builtin_mul_overflow(c, m->imm, &off) || __builtin_add_overflow(off, (int64_t)m->c, &off) || off < INT32_MIN / 2 ||
            off > INT32_MAX / 2)
            continue;
        bool kept = true; /* x unchanged from the add to here */
        for (int j = dk + 1; j < k && kept; j++) {
            const IrIns* u = &f->code[j];
            if (definesReg(u) && u->dst == x) kept = false;
            for (int i = 0; i < u->retCount; i++)
                if (u->rets[i] == x) kept = false;
        }
        if (!kept || f->kinds[x] != 'I') continue;
        m->b = x;
        m->c = (int)off;
        changed = true;
    }
    return changed;
}

/* ---- SIMD ---------------------------------------------------------------------------------------- */

/* A counted loop, as the pipelines lower:
 *       jmp C
 *   T:  body (loads and stores at [base + i * 8], arithmetic, filters' jumps forward, sums)
 *       i = i + 1 (and other indices counting with it)
 *   C:  t = i < n
 *       jnz t, T
 * becomes, before it, the same loop on vectors of W elements while W are left; the original loop does the rest. */

enum { R_NONE, R_IV, R_RED, R_MASK, R_TEMP };

typedef struct Vec {
    A* a;
    IrFn* f;
    int lanes;
    Edit e;
    int at;           /* where the vector loop goes: the jmp into the loop */
    int* vecOf;       /* scalar register -> vector register */
    int* splat;       /* invariant register -> its vector */
    int* role;
    int vregs;        /* vector registers made */
    int zeroI, ones, negZero;
    int intCompares;
} Vec;

static void vEmit(Vec* v, IrIns ins) { editAdd(&v->e, v->at, ins); }

static int vNew(Vec* v) {
    v->vregs++;
    return newReg(v->f, 'V', 1e300);
}

static int vConst(Vec* v, char kind, int64_t bits) {
    int r = newReg(v->f, kind, 1);
    IrIns c = mk(IR_CONST, r, -1, -1);
    c.imm = bits;
    vEmit(v, c);
    return r;
}

static int vSplatOf(Vec* v, int scalar, IrIns* pre, int* npre) {
    if (v->splat[scalar] >= 0) return v->splat[scalar];
    int d = vNew(v);
    pre[(*npre)++] = mk(IR_VSPLAT, d, scalar, -1);
    v->splat[scalar] = d;
    return d;
}

/* a constant multiplier W can do as shifts and adds (no 64-bit vector multiply before AVX-512) */
static bool mulPlan(int64_t m) {
    if (m < 0) return false;
    if (__builtin_popcountll((uint64_t)m) <= 3) return true;
    return m < INT64_MAX && __builtin_popcountll((uint64_t)m + 1) == 1;
}

static bool vectorizeLoop(A* a, Loop* L, int lanes) {
    IrFn* f = a->f;
    const Blk* H = &a->b[L->header];
    int hk = H->start;
    while (hk < H->end && f->code[hk].op == IR_LABEL) hk++;
    if (H->end - hk != 2) return false;
    const IrIns* cmp = &f->code[hk];
    const IrIns* jnz = &f->code[hk + 1];
    if (cmp->op != IR_CMP || cmp->cond != C_LT || jnz->op != IR_JNZ || jnz->a != cmp->dst) return false;
    int T = a->labelBlock[jnz->imm];
    if (T < 0 || !L->in[T]) return false;
    int bodyStart = a->b[T].start, bodyEnd = H->start;
    if (bodyStart >= bodyEnd || bodyStart < 1) return false;
    /* the loop is exactly the code from T to the jnz, entered only by the jmp before T */
    for (int bi = 0; bi < a->nb; bi++) {
        bool inside = a->b[bi].start >= bodyStart && a->b[bi].start < H->end;
        if (inside != (L->in[bi] != 0)) return false;
    }
    const IrIns* entry = &f->code[bodyStart - 1];
    if (entry->op != IR_JMP || a->labelBlock[entry->imm] != L->header || L->preAt != bodyStart - 1) return false;
    for (int p = 0; p < a->b[T].npred; p++)
        if (!L->in[a->b[T].pred[p]]) return false;
    int nr = f->vregCount;
    int* defIn = qkNew(sizeof(int) * (size_t)nr);
    int* defAt = qkNew(sizeof(int) * (size_t)nr);
    int* usesAll = qkNew(sizeof(int) * (size_t)nr);
    int* usesIn = qkNew(sizeof(int) * (size_t)nr);
    for (int k = 0; k < f->count; k++) {
        const IrIns* ins = &f->code[k];
        bool in = k >= bodyStart && k < H->end;
        for (int s = 0; s < useSlots(ins); s++) {
            int r = useReg(ins, s);
            if (r < 0) continue;
            usesAll[r]++;
            if (in) usesIn[r]++;
        }
        if (in && definesReg(ins)) defIn[ins->dst]++, defAt[ins->dst] = k;
        if (in && ins->retCount) return false;
    }
    int i = cmp->a, n = cmp->b;
    if (defIn[n] || f->kinds[i] != 'I') return false;
    /* regions: each forward jump of a filter opens one, its label closes it */
    int* region = qkNew(sizeof(int) * (size_t)(bodyEnd - bodyStart + 1));
    int* parent = qkNew(sizeof(int) * (size_t)(bodyEnd - bodyStart + 2));
    int* closeAt = qkNew(sizeof(int) * (size_t)(bodyEnd - bodyStart + 2));
    int nreg = 1, curR = 0;
    parent[0] = -1;
    closeAt[0] = -1;
    int* labelUses = qkNew(sizeof(int) * (size_t)(f->labelCount + 1));
    for (int k = 0; k < f->count; k++)
        if (isBranch(f->code[k].op)) labelUses[f->code[k].imm]++;
    int* role = qkNew(sizeof(int) * (size_t)nr);
    int lastWork = -1, firstInc = bodyEnd;
    for (int k = bodyStart; k < bodyEnd; k++) {
        const IrIns* ins = &f->code[k];
        if (ins->op == IR_LABEL) {
            int inner = 0;
            while (curR > 0 && closeAt[curR] == ins->imm) curR = parent[curR], inner++;
            /* a jump to it from elsewhere (a continue); T's own labels: the jump back */
            if (a->labelBlock[ins->imm] != T && labelUses[ins->imm] != inner) return false;
            region[k - bodyStart] = curR;
            continue;
        }
        region[k - bodyStart] = curR;
        if (ins->op == IR_JZ || ins->op == IR_JNZ) {
            int t = a->labelBlock[ins->imm];
            if (t < 0 || a->b[t].start <= k || a->b[t].start >= bodyEnd) return false;
            parent[nreg] = curR;
            closeAt[nreg] = (int)ins->imm;
            curR = nreg++;
            lastWork = k;
            continue;
        }
        if (ins->op == IR_ADD && ins->dst >= 0 && ins->dst == ins->a && !ins->cond) {
            int64_t one;
            if (constVal(a, useOf(a, k, 1), &one) && one == 1 && defIn[ins->dst] == 1 && curR == 0 && f->kinds[ins->dst] == 'I') {
                role[ins->dst] = R_IV;
                if (k < firstInc) firstInc = k;
                continue;
            }
        }
        lastWork = k;
    }
    if (curR != 0 || role[i] != R_IV || lastWork > firstInc) return false;
    /* every instruction of the body, then: what it is, and whether vectors do it */
    for (int k = bodyStart; k < bodyEnd; k++) {
        const IrIns* ins = &f->code[k];
        int rg = region[k - bodyStart];
        switch (ins->op) {
        case IR_LABEL: case IR_JZ: case IR_JNZ: case IR_NOP: continue;
        default: break;
        }
        if (ins->dst >= 0 && role[ins->dst] == R_IV && ins->op == IR_ADD && k >= firstInc) continue;
        /* operands: an index only as an index */
        for (int s = 0; s < useSlots(ins); s++) {
            int r = useReg(ins, s);
            if (r < 0) continue;
            if (role[r] == R_IV && !((ins->op == IR_LOAD || ins->op == IR_STORE) && s == 1)) return false;
        }
        switch (ins->op) {
        case IR_LOAD: case IR_STORE:
            if (ins->b < 0 || role[ins->b] != R_IV || ins->imm != 8 || defIn[ins->a]) return false;
            if (ins->op == IR_STORE && rg != 0) return false;
            if (ins->op == IR_LOAD && defIn[ins->dst] != 1) return false;
            break;
        case IR_ADD: case IR_SUB: case IR_AND: case IR_OR: case IR_XOR: case IR_NEG:
            if (ins->cond || f->kinds[ins->dst] != 'I') return false;
            break;
        case IR_MUL: {
            int64_t m;
            if (ins->cond) return false;
            if (!((constVal(a, useOf(a, k, 1), &m) && mulPlan(m)) || (constVal(a, useOf(a, k, 0), &m) && mulPlan(m)))) return false;
            break;
        }
        case IR_SHL: {
            int64_t m;
            if (!constVal(a, useOf(a, k, 1), &m) || m < 0 || m > 63) return false;
            break;
        }
        case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV: case IR_FMIN: case IR_FMAX: case IR_FNEG: case IR_SQRT:
            break;
        case IR_CMP:
            if (qkTargetArch == QK_ARCH_X64 && lanes == 2 && !(qkCpuHasSse42() && !(qkObjectMode && !qkUseAvx))) return false;
            /* fallthrough */
        case IR_FCMP:
            role[ins->dst] = R_MASK;
            break;
        case IR_MOV:
            if (f->kinds[ins->dst] != f->kinds[ins->a]) return false;
            break;
        default:
            return false;
        }
        int d = ins->op == IR_STORE ? -1 : ins->dst;
        if (d < 0) continue;
        if (defIn[d] != 1) return false;
        if (role[d] == R_MASK) {
            /* read only by the filter jumps of this body */
            int jumps = 0;
            for (int j = bodyStart; j < bodyEnd; j++)
                if ((f->code[j].op == IR_JZ || f->code[j].op == IR_JNZ) && f->code[j].a == d) jumps++;
            if (jumps != usesAll[d]) return false;
            continue;
        }
        /* a sum: d = d + x, d read by nothing else in the loop */
        bool sum = (ins->op == IR_ADD || ins->op == IR_FADD) && (ins->a == d) != (ins->b == d) && usesIn[d] == 1;
        if (sum) {
            role[d] = R_RED;
            continue;
        }
        if (ins->a == d || ins->b == d) return false;
        /* a temporary: read only after it, in its region (or one inside it), and nowhere outside the loop */
        if (usesAll[d] != usesIn[d]) return false;
        role[d] = R_TEMP;
        for (int j = bodyStart; j < bodyEnd; j++) {
            const IrIns* u = &f->code[j];
            for (int s = 0; s < useSlots(u); s++) {
                if (useReg(u, s) != d) continue;
                if (j <= k) return false;
                int ur = region[j - bodyStart];
                while (ur > 0 && ur != rg) ur = parent[ur];
                if (ur != rg) return false;
            }
        }
    }
    for (int k = bodyStart; k < bodyEnd; k++) { /* a filter's jump tests a comparison of this body */
        const IrIns* ins = &f->code[k];
        if ((ins->op == IR_JZ || ins->op == IR_JNZ) && role[ins->a] != R_MASK) return false;
    }
    /* ---- the vector loop ---- */
    Vec v;
    memset(&v, 0, sizeof v);
    v.a = a;
    v.f = f;
    v.lanes = lanes;
    v.at = bodyStart - 1;
    int room = nr + 16 * (bodyEnd - bodyStart) + 64; /* the registers the translation makes are looked up too */
    v.vecOf = qkNew(sizeof(int) * (size_t)room);
    v.splat = qkNew(sizeof(int) * (size_t)room);
    for (int r = 0; r < room; r++) v.vecOf[r] = v.splat[r] = -1;
    int VT = f->labelCount++, VC = f->labelCount++, SKIP = f->labelCount++;
    double w = f->weight[i];
    /* n - (lanes - 1) must not wrap: n >= INT64_MIN + lanes */
    int kmin = vConst(&v, 'I', INT64_MIN + lanes);
    int g = newReg(f, 'I', 1);
    vEmit(&v, (IrIns){.op = IR_CMP, .cond = C_LT, .dst = g, .a = n, .b = kmin, .c = 0});
    vEmit(&v, (IrIns){.op = IR_JNZ, .dst = -1, .a = g, .b = -1, .imm = SKIP});
    int kl = vConst(&v, 'I', lanes - 1);
    int lim = newReg(f, 'I', w);
    vEmit(&v, mk(IR_SUB, lim, n, kl));
    int kW = vConst(&v, 'I', lanes);
    /* the body, translated; splats collected for before the loop */
    IrIns* pre = qkNew(sizeof(IrIns) * (size_t)(4 * (bodyEnd - bodyStart) + 16));
    int npre = 0;
    IrIns* body = qkNew(sizeof(IrIns) * (size_t)(16 * (bodyEnd - bodyStart) + 16));
    int nbody = 0;
    int* redVec = qkNew(sizeof(int) * (size_t)nr);
    for (int r = 0; r < nr; r++) redVec[r] = -1;
    int maskStack[64], labelStack[64], ms = 0;
    int zero = -1, ones = -1, negz = -1;
#define OPERAND(r) (v.vecOf[r] >= 0 ? v.vecOf[r] : vSplatOf(&v, r, pre, &npre))
#define VOP(op_, d_, x_, y_) (body[nbody++] = (IrIns){.op = IR_VOP, .imm = (op_), .dst = (d_), .a = (x_), .b = (y_), .c = 0})
    for (int k = bodyStart; k < bodyEnd; k++) {
        const IrIns* ins = &f->code[k];
        int cur = ms ? maskStack[ms - 1] : -1;
        switch (ins->op) {
        case IR_LABEL:
            while (ms && labelStack[ms - 1] == ins->imm) ms--;
            continue;
        case IR_JZ: case IR_JNZ: {
            int m = v.vecOf[ins->a];
            int nm = m;
            if (ins->op == IR_JZ) { /* on where the comparison holds */
                if (cur >= 0) {
                    nm = vNew(&v);
                    VOP(IR_AND, nm, cur, m);
                }
            } else { /* on where it does not */
                nm = vNew(&v);
                if (cur < 0) {
                    if (ones < 0) {
                        int k1 = vConst(&v, 'I', -1);
                        ones = vSplatOf(&v, k1, pre, &npre);
                    }
                    cur = ones;
                }
                VOP(IR_NOT, nm, cur, m); /* cur & ~m */
            }
            maskStack[ms] = nm;
            labelStack[ms++] = (int)ins->imm;
            continue;
        }
        case IR_NOP: continue;
        default: break;
        }
        if (ins->dst >= 0 && role[ins->dst] == R_IV && ins->op == IR_ADD && k >= firstInc) continue;
        int d = ins->dst;
        switch (ins->op) {
        case IR_LOAD: {
            int vd = vNew(&v);
            body[nbody++] = (IrIns){.op = IR_VLOAD, .dst = vd, .a = ins->a, .b = ins->b, .c = ins->c, .imm = 8};
            v.vecOf[d] = vd;
            break;
        }
        case IR_STORE:
            body[nbody++] = (IrIns){.op = IR_VSTORE, .dst = OPERAND(ins->dst), .a = ins->a, .b = ins->b, .c = ins->c, .imm = 8};
            break;
        case IR_MOV:
            v.vecOf[d] = OPERAND(ins->a);
            break;
        case IR_ADD: case IR_FADD:
            if (role[d] == R_RED) {
                int x = OPERAND(ins->a == d ? ins->b : ins->a);
                if (ins->op == IR_ADD) {
                    if (cur >= 0) {
                        int t = vNew(&v);
                        VOP(IR_AND, t, x, cur);
                        x = t;
                    }
                    if (redVec[d] < 0) {
                        redVec[d] = vNew(&v);
                        if (zero < 0) zero = vConst(&v, 'I', 0);
                        pre[npre++] = mk(IR_VSPLAT, redVec[d], zero, -1);
                    }
                    VOP(IR_ADD, redVec[d], redVec[d], x);
                } else {
                    if (cur >= 0) { /* lanes filtered out add -0.0: x + -0.0 is x, for every x */
                        if (negz < 0) {
                            int kz = vConst(&v, 'F', INT64_MIN);
                            negz = vSplatOf(&v, kz, pre, &npre);
                        }
                        int t1 = vNew(&v), t2 = vNew(&v), t3 = vNew(&v);
                        VOP(IR_AND, t1, x, cur);
                        VOP(IR_NOT, t2, negz, cur);
                        VOP(IR_OR, t3, t1, t2);
                        x = t3;
                    }
                    for (int l = 0; l < lanes; l++) { /* in order: the same sum as one at a time */
                        int s = newReg(f, 'F', 1e299);
                        body[nbody++] = (IrIns){.op = IR_VLANE, .dst = s, .a = x, .b = -1, .c = l};
                        body[nbody++] = ins->a == d ? mk(IR_FADD, d, d, s) : mk(IR_FADD, d, s, d);
                    }
                }
                break;
            }
            /* fallthrough */
        case IR_SUB: case IR_AND: case IR_OR: case IR_XOR: case IR_FSUB: case IR_FMUL: case IR_FDIV: case IR_FMIN: case IR_FMAX:
        case IR_CMP: case IR_FCMP: {
            int x = OPERAND(ins->a), y = OPERAND(ins->b);
            int vd = vNew(&v);
            body[nbody++] = (IrIns){.op = IR_VOP, .imm = ins->op, .cond = ins->cond, .dst = vd, .a = x, .b = y};
            if (ins->op == IR_CMP) v.intCompares++;
            v.vecOf[d] = vd;
            break;
        }
        case IR_NEG: {
            if (zero < 0) zero = vConst(&v, 'I', 0);
            int vd = vNew(&v);
            VOP(IR_SUB, vd, vSplatOf(&v, zero, pre, &npre), OPERAND(ins->a));
            v.vecOf[d] = vd;
            break;
        }
        case IR_FNEG: {
            if (negz < 0) {
                int kz = vConst(&v, 'F', INT64_MIN);
                negz = vSplatOf(&v, kz, pre, &npre);
            }
            int vd = vNew(&v);
            VOP(IR_XOR, vd, OPERAND(ins->a), negz);
            v.vecOf[d] = vd;
            break;
        }
        case IR_SQRT: {
            int vd = vNew(&v);
            VOP(IR_SQRT, vd, OPERAND(ins->a), -1);
            v.vecOf[d] = vd;
            break;
        }
        case IR_SHL: {
            int64_t m = 0;
            constVal(a, useOf(a, k, 1), &m);
            int vd = vNew(&v);
            body[nbody++] = (IrIns){.op = IR_VOP, .imm = IR_SHL, .dst = vd, .a = OPERAND(ins->a), .b = -1, .c = (int)m};
            v.vecOf[d] = vd;
            break;
        }
        case IR_MUL: { /* by a constant: shifts and adds */
            int64_t m = 0;
            int x;
            if (constVal(a, useOf(a, k, 1), &m) && mulPlan(m)) x = OPERAND(ins->a);
            else {
                constVal(a, useOf(a, k, 0), &m);
                x = OPERAND(ins->b);
            }
            int vd;
            if (m == 0) {
                if (zero < 0) zero = vConst(&v, 'I', 0);
                vd = vSplatOf(&v, zero, pre, &npre);
            } else if (__builtin_popcountll((uint64_t)m) <= 3) {
                vd = -1;
                for (int bit = 0; bit < 63; bit++) {
                    if (!((uint64_t)m >> bit & 1)) continue;
                    int term = x;
                    if (bit) {
                        term = vNew(&v);
                        body[nbody++] = (IrIns){.op = IR_VOP, .imm = IR_SHL, .dst = term, .a = x, .b = -1, .c = bit};
                    }
                    if (vd < 0) vd = term;
                    else {
                        int s = vNew(&v);
                        VOP(IR_ADD, s, vd, term);
                        vd = s;
                    }
                }
            } else { /* 2^p - 1 */
                int p = 0;
                while (((uint64_t)1 << p) != (uint64_t)m + 1) p++;
                int t = vNew(&v);
                body[nbody++] = (IrIns){.op = IR_VOP, .imm = IR_SHL, .dst = t, .a = x, .b = -1, .c = p};
                vd = vNew(&v);
                VOP(IR_SUB, vd, t, x);
            }
            v.vecOf[d] = vd;
            break;
        }
        default: return false; /* not reached: checked above */
        }
    }
#undef VOP
#undef OPERAND
    if (v.vregs > 12) { /* too many to keep in registers: the scalar loop as it was */
        f->labelCount -= 3;
        return false;
    }
    for (int k = 0; k < npre; k++) vEmit(&v, pre[k]);
    vEmit(&v, (IrIns){.op = IR_JMP, .dst = -1, .a = -1, .b = -1, .imm = VC});
    vEmit(&v, (IrIns){.op = IR_LABEL, .dst = -1, .a = -1, .b = -1, .imm = VT});
    for (int k = 0; k < nbody; k++) vEmit(&v, body[k]);
    for (int r = 0; r < nr; r++)
        if (role[r] == R_IV) vEmit(&v, mk(IR_ADD, r, r, kW));
    vEmit(&v, (IrIns){.op = IR_LABEL, .dst = -1, .a = -1, .b = -1, .imm = VC});
    int c2 = newReg(f, 'I', w);
    vEmit(&v, (IrIns){.op = IR_CMP, .cond = C_LT, .dst = c2, .a = i, .b = lim});
    vEmit(&v, (IrIns){.op = IR_JNZ, .dst = -1, .a = c2, .b = -1, .imm = VT});
    for (int r = 0; r < nr; r++) { /* the Int sums' lanes, added (in any order: the same) */
        if (redVec[r] < 0) continue;
        for (int l = 0; l < lanes; l++) {
            int s = newReg(f, 'I', 1);
            vEmit(&v, (IrIns){.op = IR_VLANE, .dst = s, .a = redVec[r], .b = -1, .c = l});
            vEmit(&v, mk(IR_ADD, r, r, s));
        }
    }
    vEmit(&v, (IrIns){.op = IR_VEND, .dst = -1, .a = -1, .b = -1});
    vEmit(&v, (IrIns){.op = IR_LABEL, .dst = -1, .a = -1, .b = -1, .imm = SKIP});
    editApply(f, &v.e);
    if (qkTargetArch == QK_ARCH_X64 && lanes == 4) qkSimdFeatures |= 2;
    if (qkTargetArch == QK_ARCH_X64 && lanes == 2 && v.intCompares) qkSimdFeatures |= 4; /* pcmpgtq */
    return true;
}

/* tried: the header labels of the loops looked at already (the scalar loop left after a vector one, too) */
static bool vectorize(A* a, int lanes, char* tried) {
    for (int li = 0; li < a->nloops; li++) {
        Loop* L = &a->loops[li];
        bool inner = true;
        for (int j = 0; j < a->nloops; j++)
            if (a->loops[j].parent == li) inner = false;
        const IrIns* first = &a->f->code[a->b[L->header].start];
        if (!inner || L->preAt < 0 || first->op != IR_LABEL || tried[first->imm]) continue;
        tried[first->imm] = 1;
        if (vectorizeLoop(a, L, lanes)) return true;
    }
    return false;
}

/* ---- the tier ------------------------------------------------------------------------------------ */

static int simdLanes(void) {
    if (qkTargetArch == QK_ARCH_ARM64) return 2;
    if (qkUseAvx && qkCpuHasAvx2() && !(qkObjectMode && !qkUseAvx)) return 4;
    return 2;
}

/* ---- block layout: a loop's way out (a return, a break) after the function, the loop falling through -- */

static void dropUnreachable(A* a) {
    for (int bi = 0; bi < a->nb; bi++)
        if (a->b[bi].order < 0)
            for (int k = a->b[bi].start; k < a->b[bi].end; k++) a->f->code[k].op = IR_NOP;
    dropNops(a->f);
}

static bool coldExits(A* a) {
    IrFn* f = a->f;
    if (!f->count || (f->code[f->count - 1].op != IR_RET && f->code[f->count - 1].op != IR_JMP)) return false; /* falls off its end */
    for (int bi = 0; bi + 2 < a->nb; bi++) {
        const Blk* b = &a->b[bi];
        const Blk* x = &a->b[bi + 1]; /* the branch's fall-through: the exit */
        if (b->order < 0 || b->loop < 0 || x->npred != 1) continue;
        IrIns* br = &f->code[b->end - 1];
        if (br->op != IR_JZ && br->op != IR_JNZ) continue;
        if (a->labelBlock[br->imm] != bi + 2 || !a->loops[b->loop].in[bi + 2]) continue; /* jumps over it, staying in */
        const IrIns* last = &f->code[x->end - 1];
        bool leaves = last->op == IR_RET || (last->op == IR_JMP && !a->loops[b->loop].in[a->labelBlock[last->imm]]);
        if (!leaves || f->code[x->start].op == IR_LABEL) continue;
        /* jz c, L; exit...; L:  ->  jnz c, N; L: ... ; (after the function) N: exit... */
        int N = f->labelCount++;
        Edit e = {0};
        editAdd(&e, f->count, (IrIns){.op = IR_LABEL, .dst = -1, .a = -1, .b = -1, .imm = N});
        for (int k = x->start; k < x->end; k++) {
            editAdd(&e, f->count, f->code[k]);
            f->code[k].op = IR_NOP;
        }
        br->op = br->op == IR_JZ ? IR_JNZ : IR_JZ;
        br->imm = N;
        editApply(f, &e);
        return true;
    }
    return false;
}

/* values nobody reads (a counter a lambda did not ask for): gone before the loops are looked at */
static void deadCode(IrFn* f) {
    int nr = f->vregCount ? f->vregCount : 1;
    int* reads = qkNew(sizeof(int) * (size_t)nr); /* by instructions other than the one defining it */
    for (bool changed = true; changed;) {
        changed = false;
        memset(reads, 0, sizeof(int) * (size_t)nr);
        for (int k = 0; k < f->count; k++) {
            const IrIns* ins = &f->code[k];
            for (int s = 0; s < useSlots(ins); s++) {
                int r = useReg(ins, s);
                if (r >= 0 && !(definesReg(ins) && ins->dst == r)) reads[r]++;
            }
        }
        for (int k = 0; k < f->count; k++) {
            IrIns* ins = &f->code[k];
            if (!definesReg(ins) || reads[ins->dst]) continue;
            switch (ins->op) {
            case IR_CONST: case IR_MOV: case IR_NEG: case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SAR: case IR_FADD:
            case IR_FSUB: case IR_FMUL: case IR_FDIV: case IR_FNEG: case IR_SQRT: case IR_FMIN: case IR_FMAX: case IR_CMP: case IR_FCMP:
            case IR_NOT: case IR_I2F: case IR_ROUND: case IR_LOAD:
                break;
            case IR_ADD: case IR_SUB: case IR_MUL: case IR_F2I:
                if (ins->cond) continue; /* may stop with a panic */
                break;
            default: continue;
            }
            ins->op = IR_NOP;
            changed = true;
        }
    }
    dropNops(f);
}

static void releaseFn(IrFn* f, int lanes, bool fuel) {
    if (!f->count) return;
    qkCommonSubexpressions(f); /* i * 3 written three times is one value before loops are looked at */
    deadCode(f);
    /* each pass on a fresh analysis when the one before changed the code */
    for (int round = 0; round < 16 && scalarReplace(analyse(f)); round++) dropNops(f);
    devirtualize(analyse(f));
    if (boundsAndDivisions(analyse(f))) dropNops(f);
    for (int round = 0; round < 32 && licm(analyse(f)); round++) dropNops(f);
    if (lanes && !fuel) {
        char* tried = qkNew((size_t)f->labelCount + 1);
        for (int round = 0; round < 16 && vectorize(analyse(f), lanes, tried); round++) dropNops(f);
    }
    for (int round = 0; round < 32 && strengthReduce(analyse(f)); round++) dropNops(f);
    foldAddresses(analyse(f));
    dropUnreachable(analyse(f));
    for (int round = 0; round < 16 && coldExits(analyse(f)); round++) dropNops(f);
}

void qkOptimizeRelease(IrProgram* p) {
    if (qkDebug) return; /* a debug build keeps every value where the debugger looks for it */
    qkSimdFeatures = 0;
    int lanes = simdLanes();
    qkSimdLanes = lanes;
    for (int i = 0; i < p->count; i++) releaseFn(&p->fns[i], lanes, p->fuel);
}
