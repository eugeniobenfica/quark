/* Quark — a few cheap optimizations on the IR, run before machine code:
 *  - a copy right after the instruction that computed its value is folded into that instruction;
 *  - code whose result nobody reads is dropped (a loop's index nobody asked for, say);
 *  - an int register set once by a constant becomes an immediate (no register, no instruction);
 *  - a float constant is computed once, at the start, and keeps a register if it is used in loops. */
#include "quark.h"

static bool defines(const IrIns* ins) {
    switch (ins->op) {
    case IR_STORE: case IR_BOUNDS: case IR_LABEL: case IR_JMP: case IR_JZ: case IR_JNZ: case IR_CALL: case IR_RTCALL:
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

static void optimizeFn(IrFn* f) {
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
