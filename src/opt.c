/* Quark — a few cheap optimizations on the IR, run before machine code:
 *  - a copy right after the instruction that computed its value is folded into that instruction;
 *  - code whose result nobody reads is dropped (a loop's index nobody asked for, say);
 *  - an int register set once by a constant becomes an immediate (no register, no instruction);
 *  - a float constant is computed once, at the start, and keeps a register if it is used in loops. */
#include "qk.h"

static bool defines(const IrIns* ins) {
    switch (ins->op) {
    case IR_STORE: case IR_BOUNDS: case IR_LABEL: case IR_JMP: case IR_JZ: case IR_JNZ: case IR_CALL: case IR_RTCALL: case IR_HOSTCALL:
    case IR_RET: case IR_FUEL: case IR_NOP:
        return false;
    default:
        return ins->dst >= 0;
    }
}

static bool pure(const IrIns* ins) {
    switch (ins->op) {
    case IR_CONST: case IR_MOV: case IR_ADD: case IR_SUB: case IR_MUL: case IR_NEG: case IR_FADD: case IR_FSUB: case IR_FMUL:
    case IR_FDIV: case IR_FNEG: case IR_SQRT: case IR_FMIN: case IR_FMAX: case IR_CMP: case IR_FCMP: case IR_NOT: case IR_I2F:
    case IR_F2I: case IR_ROUND: case IR_LOAD:
        return true;
    default:
        return false;
    }
}

typedef struct Counts {
    int* defs;
    int* uses;
    int* foreignUses; /* uses by instructions that do not define the register itself */
} Counts;

static void addUse(Counts* c, const IrIns* ins, int v, int k) {
    if (v < 0) return;
    c->uses[v] += k;
    if (!(defines(ins) && ins->dst == v)) c->foreignUses[v] += k;
}

static void countIns(Counts* c, const IrIns* ins, int k) {
    if (ins->op == IR_NOP || ins->op == IR_LABEL || ins->op == IR_JMP) return;
    if (defines(ins)) c->defs[ins->dst] += k;
    if (ins->op == IR_STORE) addUse(c, ins, ins->dst, k);
    if (ins->op != IR_CONST) {
        addUse(c, ins, ins->a, k);
        addUse(c, ins, ins->b, k);
    }
    for (int i = 0; i < ins->argCount; i++) addUse(c, ins, ins->args[i], k);
    for (int i = 0; i < ins->retCount; i++) c->defs[ins->rets[i]] += k;
}

static void recount(IrFn* f, Counts* c) {
    memset(c->defs, 0, sizeof(int) * (size_t)f->vregCount);
    memset(c->uses, 0, sizeof(int) * (size_t)f->vregCount);
    memset(c->foreignUses, 0, sizeof(int) * (size_t)f->vregCount);
    for (int k = 0; k < f->paramCount; k++) c->defs[f->params[k]]++;
    for (int k = 0; k < f->count; k++) countIns(c, &f->code[k], 1);
}

/* ---- common subexpressions ----------------------------------------------------------------------- */

typedef struct Seen {
    uint8_t op, cond, kind;
    int64_t a, b;   /* an operand: a register's number and version, or a constant's value */
    bool aK, bK;
    int va, vb;
    int64_t imm;
    int c;
    int dst;
} Seen;

static bool commutative(int op) {
    return op == IR_ADD || op == IR_MUL || op == IR_FADD || op == IR_FMUL || op == IR_FMIN || op == IR_FMAX;
}

static int resolve(const int* rename, int v) {
    while (v >= 0 && rename[v] >= 0) v = rename[v];
    return v;
}

static void renameOperands(IrIns* ins, const int* rename) {
    if (ins->op == IR_STORE) ins->dst = resolve(rename, ins->dst);
    if (ins->op != IR_CONST) {
        ins->a = resolve(rename, ins->a);
        ins->b = resolve(rename, ins->b);
    }
    for (int i = 0; i < ins->argCount; i++) ins->args[i] = resolve(rename, ins->args[i]);
}

/* In a straight run of code, the same pure computation on the same values is done once (p * 3 for three
 * loads of one pixel, say). Constants count by value, and a float constant is one register for the whole
 * function. Only registers set once are merged. */
static int addReg(IrFn* f, char kind) {
    if (f->vregCount == f->vregCap) {
        int cap = f->vregCap ? f->vregCap * 2 : 64;
        char* kinds = qkNew((size_t)cap);
        double* weight = qkNew(sizeof(double) * (size_t)cap);
        memcpy(kinds, f->kinds, (size_t)f->vregCount);
        memcpy(weight, f->weight, sizeof(double) * (size_t)f->vregCount);
        f->kinds = kinds;
        f->weight = weight;
        f->vregCap = cap;
    }
    f->kinds[f->vregCount] = kind;
    return f->vregCount++;
}

/* x % d by a constant (not a power of two) is x - x / d * d: then x / d nearby is shared, one multiplication
 * by d's magic number for both, as pixel % width and pixel / width are. */
static void remainderFromQuotient(IrFn* f) {
    int n = f->vregCount ? f->vregCount : 1;
    int* defs = qkNew(sizeof(int) * (size_t)n);
    int64_t* kv = qkNew(sizeof(int64_t) * (size_t)n);
    for (int k = 0; k < f->count; k++) {
        const IrIns* ins = &f->code[k];
        if (defines(ins)) defs[ins->dst]++;
        for (int i = 0; i < ins->retCount; i++) defs[ins->rets[i]]++;
    }
    for (int k = 0; k < f->paramCount; k++) defs[f->params[k]]++;
    int found = 0;
    for (int k = 0; k < f->count; k++) {
        const IrIns* ins = &f->code[k];
        if (ins->op == IR_CONST && ins->c != 1 && f->kinds[ins->dst] == 'I' && defs[ins->dst] == 1) kv[ins->dst] = ins->imm;
        else if (ins->op == IR_MOV && defs[ins->dst] == 1 && f->kinds[ins->dst] == 'I') kv[ins->dst] = kv[ins->a]; /* let d = 1000 */
        else if (ins->op == IR_MOD && ins->b >= 0 && kv[ins->b] > 1 && (kv[ins->b] & (kv[ins->b] - 1))) found++;
    }
    if (!found) return;
    IrIns* code = qkNew(sizeof(IrIns) * (size_t)(f->count + 2 * found + 1));
    int m = 0;
    for (int k = 0; k < f->count; k++) {
        IrIns ins = f->code[k];
        if (ins.op == IR_MOD && ins.b >= 0 && kv[ins.b] > 1 && (kv[ins.b] & (kv[ins.b] - 1))) {
            int q = addReg(f, 'I'), p = addReg(f, 'I');
            f->weight[q] = f->weight[p] = f->weight[ins.dst];
            IrIns div = ins, mul = ins, sub = ins;
            div.op = IR_DIV;
            div.dst = q;
            mul.op = IR_MUL;
            mul.dst = p;
            mul.a = q;
            sub.op = IR_SUB;
            sub.b = p;
            code[m++] = div;
            code[m++] = mul;
            code[m++] = sub;
            continue;
        }
        code[m++] = ins;
    }
    f->code = code;
    f->count = m;
    f->cap = m;
}

static void commonSubexpressions(IrFn* f) {
    int n = f->vregCount ? f->vregCount : 1;
    int* defs = qkNew(sizeof(int) * (size_t)n);
    int* version = qkNew(sizeof(int) * (size_t)n);
    int* rename = qkNew(sizeof(int) * (size_t)n);
    char* isK = qkNew((size_t)n);
    int64_t* kv = qkNew(sizeof(int64_t) * (size_t)n);
    for (int v = 0; v < f->vregCount; v++) rename[v] = -1;
    for (int k = 0; k < f->paramCount; k++) defs[f->params[k]]++;
    for (int k = 0; k < f->count; k++) {
        const IrIns* ins = &f->code[k];
        if (defines(ins)) defs[ins->dst]++;
        for (int i = 0; i < ins->retCount; i++) defs[ins->rets[i]]++;
    }
    for (int k = 0; k < f->count; k++) {
        const IrIns* ins = &f->code[k];
        if (ins->op == IR_CONST && ins->c != 1 && defs[ins->dst] == 1) {
            isK[ins->dst] = 1;
            kv[ins->dst] = ins->imm;
        }
    }
    /* float constants: one register per value (they are all hoisted to the start afterwards) */
    for (int k = 0; k < f->count; k++) {
        IrIns* ins = &f->code[k];
        if (ins->op != IR_CONST || !isK[ins->dst] || f->kinds[ins->dst] != 'F') continue;
        for (int j = 0; j < k; j++) {
            IrIns* first = &f->code[j];
            if (first->op == IR_CONST && isK[first->dst] && f->kinds[first->dst] == 'F' && first->imm == ins->imm) {
                rename[ins->dst] = first->dst;
                ins->op = IR_NOP;
                break;
            }
        }
    }
    Seen* table = qkNew(sizeof(Seen) * (size_t)(f->count ? f->count : 1));
    int seen = 0;
    for (int k = 0; k < f->count; k++) {
        IrIns* ins = &f->code[k];
        if (ins->op == IR_NOP) continue;
        renameOperands(ins, rename);
        /* constants fold: a copy of one, an Int made Float, arithmetic on two (registers set once only) */
        if (defines(ins) && defs[ins->dst] == 1 && ins->op != IR_CONST) {
            bool aK = ins->a >= 0 && isK[ins->a], bK = ins->b >= 0 && isK[ins->b];
            int64_t x = aK ? kv[ins->a] : 0, y = bK ? kv[ins->b] : 0, r = 0;
            bool fold = true;
            switch (ins->op) {
            case IR_MOV: fold = aK; r = x; break;
            case IR_I2F: {
                double v = (double)x;
                fold = aK;
                memcpy(&r, &v, 8);
                break;
            }
            case IR_ADD: fold = aK && bK; r = (int64_t)((uint64_t)x + (uint64_t)y); break;
            case IR_SUB: fold = aK && bK; r = (int64_t)((uint64_t)x - (uint64_t)y); break;
            case IR_MUL: fold = aK && bK; r = (int64_t)((uint64_t)x * (uint64_t)y); break;
            case IR_NEG: fold = aK; r = (int64_t)(0 - (uint64_t)x); break;
            default: fold = false;
            }
            if (fold) {
                ins->op = IR_CONST;
                ins->a = ins->b = -1;
                ins->c = 0;
                ins->imm = r;
                isK[ins->dst] = 1;
                kv[ins->dst] = r;
            }
        }
        /* x * 2.0 is x + x: the same result, a cheaper instruction */
        if (ins->op == IR_FMUL) {
            double two = 2.0;
            int64_t bits;
            memcpy(&bits, &two, 8);
            if (ins->b >= 0 && isK[ins->b] && kv[ins->b] == bits) ins->b = ins->a, ins->op = IR_FADD;
            else if (ins->a >= 0 && isK[ins->a] && kv[ins->a] == bits) ins->a = ins->b, ins->op = IR_FADD;
        }
        switch (ins->op) {
        case IR_LABEL: case IR_JMP: case IR_JZ: case IR_JNZ: case IR_CALL: case IR_RTCALL: case IR_HOSTCALL: case IR_RET:
            seen = 0; /* a new straight run */
            break;
        default: break;
        }
        bool safeDivide = (ins->op == IR_DIV || ins->op == IR_MOD) && ins->b >= 0 && isK[ins->b] && kv[ins->b] != 0;
        bool candidate = (pure(ins) || safeDivide) && ins->op != IR_LOAD && ins->op != IR_MOV && ins->op != IR_CONST &&
                         defines(ins) && defs[ins->dst] == 1;
        if (candidate) {
            Seen s = {0};
            s.op = ins->op;
            s.cond = ins->cond;
            s.kind = (uint8_t)f->kinds[ins->dst];
            s.imm = ins->imm;
            s.c = ins->c;
            s.dst = ins->dst;
            if (ins->a >= 0) {
                s.aK = isK[ins->a];
                s.a = s.aK ? kv[ins->a] : ins->a;
                s.va = s.aK ? 0 : version[ins->a];
            } else {
                s.a = -1;
            }
            if (ins->b >= 0) {
                s.bK = isK[ins->b];
                s.b = s.bK ? kv[ins->b] : ins->b;
                s.vb = s.bK ? 0 : version[ins->b];
            } else {
                s.b = -1;
            }
            if (commutative(ins->op) && (s.aK > s.bK || (s.aK == s.bK && s.a > s.b))) {
                Seen t = s;
                s.a = t.b, s.aK = t.bK, s.va = t.vb;
                s.b = t.a, s.bK = t.aK, s.vb = t.va;
            }
            int found = -1;
            for (int i = 0; i < seen && found < 0; i++) {
                const Seen* o = &table[i];
                if (o->op == s.op && o->cond == s.cond && o->kind == s.kind && o->imm == s.imm && o->c == s.c && o->a == s.a &&
                    o->aK == s.aK && o->va == s.va && o->b == s.b && o->bK == s.bK && o->vb == s.vb)
                    found = o->dst;
            }
            if (found >= 0) {
                rename[ins->dst] = found;
                ins->op = IR_NOP;
                continue;
            }
            table[seen++] = s;
        }
        if (defines(ins)) version[ins->dst]++;
        for (int i = 0; i < ins->retCount; i++) version[ins->rets[i]]++;
    }
    for (int k = 0; k < f->count; k++)
        if (f->code[k].op != IR_NOP) renameOperands(&f->code[k], rename);
}

static void optimizeFn(IrFn* f) {
    remainderFromQuotient(f); /* adds registers: before the counts are sized */
    commonSubexpressions(f);
    int n = f->vregCount ? f->vregCount : 1;
    Counts c = {qkNew(sizeof(int) * (size_t)n), qkNew(sizeof(int) * (size_t)n), qkNew(sizeof(int) * (size_t)n)};
    recount(f, &c);

    for (bool changed = true; changed;) {
        changed = false;
        /* copies */
        for (int k = 1; k < f->count; k++) {
            IrIns* mov = &f->code[k];
            if (mov->op != IR_MOV) continue;
            int t = mov->a, d = mov->dst;
            if (t == d || c.defs[t] != 1 || c.uses[t] != 1 || f->kinds[t] != f->kinds[d]) continue;
            int p = k - 1;
            while (p >= 0 && f->code[p].op == IR_NOP) p--;
            if (p < 0) continue;
            IrIns* prev = &f->code[p];
            bool done = false;
            if (defines(prev) && prev->dst == t && prev->op != IR_CONST) {
                countIns(&c, prev, -1);
                prev->dst = d;
                countIns(&c, prev, 1);
                done = true;
            } else if (prev->op == IR_CALL || prev->op == IR_RTCALL) {
                for (int i = 0; i < prev->retCount; i++)
                    if (prev->rets[i] == t) {
                        countIns(&c, prev, -1);
                        prev->rets[i] = d;
                        countIns(&c, prev, 1);
                        done = true;
                    }
            }
            if (done) {
                countIns(&c, mov, -1);
                mov->op = IR_NOP;
                changed = true;
            }
        }
        /* unused results */
        for (int k = 0; k < f->count; k++) {
            IrIns* ins = &f->code[k];
            if (!pure(ins) || !defines(ins) || c.foreignUses[ins->dst] != 0) continue;
            countIns(&c, ins, -1);
            ins->op = IR_NOP;
            changed = true;
        }
    }

    /* constants */
    f->isConst = qkNew((size_t)n);
    f->constValue = qkNew(sizeof(int64_t) * (size_t)n);
    IrIns* hoisted = qkNew(sizeof(IrIns) * (size_t)(f->count ? f->count : 1));
    int hoistedCount = 0;
    bool* defParam = qkNew((size_t)n);
    for (int k = 0; k < f->paramCount; k++) defParam[f->params[k]] = true;
    for (int k = 0; k < f->count; k++) {
        IrIns* ins = &f->code[k];
        if (ins->op != IR_CONST || ins->c == 1 || c.defs[ins->dst] != 1 || defParam[ins->dst]) continue;
        if (f->kinds[ins->dst] == 'I') {
            f->isConst[ins->dst] = 1;
            f->constValue[ins->dst] = ins->imm;
            f->weight[ins->dst] = 0;
        } else {
            hoisted[hoistedCount++] = *ins;
        }
        ins->op = IR_NOP;
    }
    /* drop the NOPs, the hoisted constants first */
    IrIns* code = qkNew(sizeof(IrIns) * (size_t)(f->count + hoistedCount + 1));
    int m = 0;
    for (int k = 0; k < hoistedCount; k++) code[m++] = hoisted[k];
    for (int k = 0; k < f->count; k++)
        if (f->code[k].op != IR_NOP) code[m++] = f->code[k];
    f->code = code;
    f->count = m;
    f->cap = m;
}

void qkOptimize(IrProgram* p) {
    for (int i = 0; i < p->count; i++) optimizeFn(&p->fns[i]);
}
