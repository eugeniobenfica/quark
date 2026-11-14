/* Quark — the ARM64 (AArch64) backend: Linux, macOS on Apple Silicon, Android, Raspberry Pi. No assembler, no
 * linker: every instruction is one 32-bit word, encoded here.
 *
 * Registers: the most used virtual registers own a machine register while they are alive (linear scan, loops
 * weighing more): x9-x15 and d16-d31 (the platform lets calls change them) for values no call crosses,
 * x19-x28 and d8-d15 (kept by calls) for the others; the rest live in the frame. x0-x7 and d0-d7 carry the
 * arguments of calls into C, and with x16, x17, d0-d3 they are this backend's scratch registers.
 *
 * The frame is the one the panic's stack of calls walks: x29 points at the saved x29 and the return address
 * (x30), as rbp does on x86-64. Calls between Quark functions are the same as there: arguments go to
 * [sp + 8i] of the caller (its outgoing area), the callee reads them at [x29 + 16 + 8i] and writes its results
 * there before returning. Calls into C follow AAPCS64: x0-x7 for ints and d0-d7 for floats, each counted on
 * its own, the rest on the stack; a record comes back in x0-x1, in d0-d3 when it is all floats (four at
 * most), or through the address in x8. */
#include "qk.h"

#include <stdio.h>
#include <stdlib.h>

enum { X0 = 0, X1, X2, X3, X4, X5, X6, X7, X8, X9, X16 = 16, X17 = 17, X19 = 19, FP = 29, LR = 30, SP = 31, XZR = 31 };
enum { D0 = 0, D1, D2, D3 };
/* condition codes */
enum { EQ = 0, NE = 1, HS = 2, LO = 3, MI = 4, PL = 5, HI = 8, LS = 9, GE = 10, LT = 11, GT = 12, LE = 13 };

enum { FIX_B, FIX_BCOND, FIX_CBZ, FIX_BL, FIX_ADR };

typedef struct Fixup {
    int at;     /* the instruction */
    int target; /* label, or function index */
    int kind;
} Fixup;

typedef struct Cold {
    int at;
    int code, line;
    int a, b;
    int inl;
} Cold;

typedef struct Gen {
    MachineCode* mc;
    QkBuf* b;
    const IrFn* fn;
    const IrProgram* prog;
    int* home;        /* >= 0: register (x 0-31, d 32-63); < 0: slot -(n+1) */
    int savedGpr, savedFpr;
    int out;          /* bytes of the outgoing area at sp */
    int frame;        /* bytes below x29 */
    int* labelAt;
    Fixup* fixups;
    int fixupCount, fixupCap;
    int epilogue;
    Cold* cold;
    int coldCount, coldCap;
    int* fnAt;
    int* uses;
    Fixup* callFixups;
    int callCount, callCap;
    int fnIndex;
    int inl;
} Gen;


/* ---- encoding ------------------------------------------------------------------------------------ */

static void ins(Gen* g, uint32_t w) { qkBufAdd(g->b, &w, 4); }
static int here(Gen* g) { return (int)g->b->len; }
static uint32_t wordAt(Gen* g, int at) {
    uint32_t w;
    memcpy(&w, g->b->data + at, 4);
    return w;
}
static void patch(Gen* g, int at, uint32_t w) { memcpy(g->b->data + at, &w, 4); }

static void movRR(Gen* g, int d, int s) {
    if (d == s) return;
    if (d == SP || s == SP) ins(g, 0x91000000u | (uint32_t)s << 5 | (uint32_t)d); /* add d, s, #0 */
    else ins(g, 0xAA0003E0u | (uint32_t)s << 16 | (uint32_t)d);                   /* orr d, xzr, s */
}

static void movImm(Gen* g, int d, int64_t v) {
    uint64_t u = (uint64_t)v;
    int zeros = 0, ones = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t h = (uint16_t)(u >> (16 * i));
        zeros += h == 0;
        ones += h == 0xFFFF;
    }
    bool inverted = ones > zeros; /* movn, then movk the halves that are not all ones */
    bool first = true;
    for (int i = 0; i < 4; i++) {
        uint16_t h = (uint16_t)(u >> (16 * i));
        if (h == (inverted ? 0xFFFF : 0) && !(i == 3 && first)) continue;
        if (first && i == 3 && h == (inverted ? 0xFFFF : 0)) i = 0, h = (uint16_t)u; /* 0 or -1: one instruction */
        if (first) {
            if (inverted) ins(g, 0x92800000u | (uint32_t)i << 21 | (uint32_t)(uint16_t)~h << 5 | (uint32_t)d);
            else ins(g, 0xD2800000u | (uint32_t)i << 21 | (uint32_t)h << 5 | (uint32_t)d);
            first = false;
        } else {
            ins(g, 0xF2800000u | (uint32_t)i << 21 | (uint32_t)h << 5 | (uint32_t)d);
        }
    }
}

/* an address patched at load time: ldr d, =address (the 8 bytes follow, aligned, jumped over) */
static void movReloc(Gen* g, int d, int kind, int64_t target) {
    if (qkObjectMode) { /* adrp d, entry; ldr d, [d, #lo12]: the address from the table (the object's relocations) */
        Reloc r = {here(g), kind, target};
        QK_PUSH(g->mc->relocs, g->mc->relocCount, g->mc->relocCap, r);
        ins(g, 0x90000000u | (uint32_t)d);
        ins(g, 0xF9400000u | (uint32_t)d << 5 | (uint32_t)d);
        return;
    }
    if (here(g) % 8 != 0) ins(g, 0xD503201Fu); /* nop: the literal lands on 8 bytes */
    ins(g, 0x58000000u | 2u << 5 | (uint32_t)d); /* ldr d, [pc + 8] */
    ins(g, 0x14000003u);                         /* b +12 */
    Reloc r = {here(g), kind, target};
    QK_PUSH(g->mc->relocs, g->mc->relocCount, g->mc->relocCap, r);
    ins(g, 0);
    ins(g, 0);
}

static void addImm(Gen* g, int d, int n, int64_t k) { /* d = n + k (n, d may be sp) */
    if (k >= 0 && k < 4096) ins(g, 0x91000000u | (uint32_t)k << 10 | (uint32_t)n << 5 | (uint32_t)d);
    else if (k < 0 && k > -4096) ins(g, 0xD1000000u | (uint32_t)(-k) << 10 | (uint32_t)n << 5 | (uint32_t)d);
    else {
        movImm(g, X17, k);
        ins(g, 0x8B206000u | (uint32_t)X17 << 16 | (uint32_t)n << 5 | (uint32_t)d); /* add d, n, x17, uxtx */
    }
}

static void add3(Gen* g, int d, int n, int m, int shift) { ins(g, 0x8B000000u | (uint32_t)m << 16 | (uint32_t)shift << 10 | (uint32_t)n << 5 | (uint32_t)d); }
static void sub3(Gen* g, int d, int n, int m) { ins(g, 0xCB000000u | (uint32_t)m << 16 | (uint32_t)n << 5 | (uint32_t)d); }
static void mul3(Gen* g, int d, int n, int m) { ins(g, 0x9B007C00u | (uint32_t)m << 16 | (uint32_t)n << 5 | (uint32_t)d); }
static void cmpRR(Gen* g, int n, int m) { ins(g, 0xEB00001Fu | (uint32_t)m << 16 | (uint32_t)n << 5); }
static void cset(Gen* g, int d, int cond) { ins(g, 0x9A9F07E0u | (uint32_t)(cond ^ 1) << 12 | (uint32_t)d); }

/* ldr / str, 64 bits, of an x or a d register, at base + off */
static void access(Gen* g, bool load, bool fp, int r, int base, int64_t off) {
    uint32_t imm = fp ? (load ? 0xFD400000u : 0xFD000000u) : (load ? 0xF9400000u : 0xF9000000u);
    uint32_t unscaled = fp ? (load ? 0xFC400000u : 0xFC000000u) : (load ? 0xF8400000u : 0xF8000000u);
    uint32_t reg = fp ? (load ? 0xFC606800u : 0xFC206800u) : (load ? 0xF8606800u : 0xF8206800u);
    if (off >= 0 && off < 32768 && off % 8 == 0) ins(g, imm | (uint32_t)(off / 8) << 10 | (uint32_t)base << 5 | (uint32_t)r);
    else if (off >= -256 && off < 256) ins(g, unscaled | (uint32_t)(off & 0x1FF) << 12 | (uint32_t)base << 5 | (uint32_t)r);
    else {
        movImm(g, X17, off);
        ins(g, reg | (uint32_t)X17 << 16 | (uint32_t)base << 5 | (uint32_t)r);
    }
}

static void fmovToX(Gen* g, int x, int d) { ins(g, 0x9E660000u | (uint32_t)d << 5 | (uint32_t)x); }
static void fmovToD(Gen* g, int d, int x) { ins(g, 0x9E670000u | (uint32_t)x << 5 | (uint32_t)d); }
static void fmovDD(Gen* g, int d, int n) {
    if (d != n) ins(g, 0x1E604000u | (uint32_t)n << 5 | (uint32_t)d);
}

static void fixup(Gen* g, int target, int kind, uint32_t w) {
    Fixup f = {here(g), target, kind};
    QK_PUSH(g->fixups, g->fixupCount, g->fixupCap, f);
    ins(g, w);
}
static void bLabel(Gen* g, int target) { fixup(g, target, FIX_B, 0x14000000u); }
static void bcondLabel(Gen* g, int cond, int target) { fixup(g, target, FIX_BCOND, 0x54000000u | (uint32_t)cond); }

/* ---- where a virtual register lives -------------------------------------------------------------- */

static bool isK(Gen* g, int v) { return g->fn->isConst && g->fn->isConst[v]; }
static int64_t kOf(Gen* g, int v) { return g->fn->constValue[v]; }
static bool inReg(Gen* g, int v) { return !isK(g, v) && g->home[v] >= 0; }
static bool isFloat(Gen* g, int v) { return g->fn->kinds[v] == 'F'; }
static int gprOf(Gen* g, int v) { return g->home[v]; }
static int fprOf(Gen* g, int v) { return g->home[v] - 32; }
static int64_t slotOff(Gen* g, int v) { return g->out + 8 * (int64_t)(-g->home[v] - 1); } /* from sp */

static void loadBits(Gen* g, int x, int v) {
    if (isK(g, v)) movImm(g, x, kOf(g, v));
    else if (inReg(g, v)) {
        if (isFloat(g, v)) fmovToX(g, x, fprOf(g, v));
        else movRR(g, x, gprOf(g, v));
    } else access(g, true, false, x, SP, slotOff(g, v));
}

static void storeBits(Gen* g, int v, int x) {
    if (inReg(g, v)) {
        if (isFloat(g, v)) fmovToD(g, fprOf(g, v), x);
        else movRR(g, gprOf(g, v), x);
    } else access(g, false, false, x, SP, slotOff(g, v));
}

static void loadF(Gen* g, int d, int v) {
    if (isK(g, v)) {
        movImm(g, X16, kOf(g, v));
        fmovToD(g, d, X16);
    } else if (inReg(g, v)) {
        if (isFloat(g, v)) fmovDD(g, d, fprOf(g, v));
        else fmovToD(g, d, gprOf(g, v));
    } else access(g, true, true, d, SP, slotOff(g, v));
}

static void storeF(Gen* g, int v, int d) {
    if (inReg(g, v)) {
        if (isFloat(g, v)) fmovDD(g, fprOf(g, v), d);
        else fmovToX(g, gprOf(g, v), d);
    } else access(g, false, true, d, SP, slotOff(g, v));
}

/* v in an x register: its own, or `scratch` loaded with it */
static int gprIn(Gen* g, int v, int scratch) {
    if (inReg(g, v) && !isFloat(g, v)) return gprOf(g, v);
    loadBits(g, scratch, v);
    return scratch;
}
static int fprIn(Gen* g, int v, int scratch) {
    if (inReg(g, v) && isFloat(g, v)) return fprOf(g, v);
    loadF(g, scratch, v);
    return scratch;
}
/* where v's new value can be made: its own register, or the scratch one (then stored) */
static int gprOut(Gen* g, int v, int scratch) { return inReg(g, v) && !isFloat(g, v) ? gprOf(g, v) : scratch; }
static int fprOut(Gen* g, int v, int scratch) { return inReg(g, v) && isFloat(g, v) ? fprOf(g, v) : scratch; }
static void doneGpr(Gen* g, int v, int r) {
    if (!(inReg(g, v) && !isFloat(g, v) && gprOf(g, v) == r)) storeBits(g, v, r);
}
static void doneFpr(Gen* g, int v, int r) {
    if (!(inReg(g, v) && isFloat(g, v) && fprOf(g, v) == r)) storeF(g, v, r);
}

/* ---- calls --------------------------------------------------------------------------------------- */

static void callRuntime(Gen* g, int id) {
    movReloc(g, X16, RELOC_RUNTIME, id);
    ins(g, 0xD63F0000u | (uint32_t)X16 << 5); /* blr x16 */
}

static void markPanicFrame(Gen* g) {
    movReloc(g, X16, RELOC_PANIC_FRAME, 0);
    access(g, false, false, FP, X16, 0); /* str x29, [x16] */
}

/* The arguments of a call into C, from their homes to where AAPCS64 wants them. */
static void cArguments(Gen* g, const int* args, int count, const char* kinds) {
    int ints = 0, floats = 0, stack = 0;
    for (int i = 0; i < count; i++) { /* past the registers: on the stack, in order */
        bool f = kinds[i] == 'F';
        if (f ? floats++ < 8 : ints++ < 8) continue;
        loadBits(g, X16, args[i]);
        access(g, false, false, X16, SP, 8 * stack++);
    }
    ints = floats = 0;
    for (int i = 0; i < count; i++) {
        if (kinds[i] == 'F') {
            if (floats < 8) loadF(g, floats, args[i]);
            floats++;
        } else {
            if (ints < 8) loadBits(g, ints, args[i]);
            ints++;
        }
    }
}

static int stackArgs(const char* kinds, int count) {
    int ints = 0, floats = 0, stack = 0;
    for (int i = 0; i < count; i++) {
        bool f = kinds[i] == 'F';
        if (!(f ? floats++ < 8 : ints++ < 8)) stack++;
    }
    return stack;
}

static void coldJump(Gen* g, int cond, int code, int line, int a, int b) {
    Cold c = {here(g), code, line, a, b, g->inl};
    QK_PUSH(g->cold, g->coldCount, g->coldCap, c);
    ins(g, 0x54000000u | (uint32_t)cond); /* b.cond, patched by emitCold */
}

static void panicCall(Gen* g, int code, int64_t line) { /* x2, x3 hold what it reports */
    markPanicFrame(g);
    movImm(g, X0, code);
    movImm(g, X1, line);
    callRuntime(g, RT_PANIC);
}

static void emitCold(Gen* g) {
    for (int i = 0; i < g->coldCount; i++) {
        Cold* c = &g->cold[i];
        int32_t rel = (here(g) - c->at) / 4;
        patch(g, c->at, wordAt(g, c->at) | ((uint32_t)rel & 0x7FFFF) << 5);
        if (c->a >= 0) loadBits(g, X2, c->a);
        else movImm(g, X2, 0);
        if (c->b >= 0) loadBits(g, X3, c->b);
        else movImm(g, X3, 0);
        panicCall(g, c->code, c->line | (int64_t)c->inl << 32);
    }
    g->coldCount = 0;
}

/* ---- one instruction ----------------------------------------------------------------------------- */

/* The address [a + b * imm + c] as (base register, offset) */
static int addressOf(Gen* g, const IrIns* in, int64_t* off) {
    int base = gprIn(g, in->a, X1);
    if (isK(g, in->b)) {
        *off = kOf(g, in->b) * in->imm + in->c;
        return base;
    }
    int idx = gprIn(g, in->b, X2);
    if (in->imm == 8) add3(g, X3, base, idx, 3);
    else if (in->imm == 1) add3(g, X3, base, idx, 0);
    else {
        movImm(g, X17, in->imm);
        mul3(g, X3, idx, X17);
        add3(g, X3, base, X3, 0);
    }
    *off = in->c;
    return X3;
}

/* The magic multiplier and shift that divide by d (d >= 2) without a division (Hacker's Delight, 10-1). */
static void magic(int64_t d, int64_t* m, int* shift) {
    const uint64_t two63 = UINT64_C(1) << 63;
    uint64_t ad = (uint64_t)d;
    uint64_t anc = two63 - 1 - two63 % ad;
    int p = 63;
    uint64_t q1 = two63 / anc, r1 = two63 - q1 * anc;
    uint64_t q2 = two63 / ad, r2 = two63 - q2 * ad;
    uint64_t delta;
    do {
        p++;
        q1 *= 2;
        r1 *= 2;
        if (r1 >= anc) q1++, r1 -= anc;
        q2 *= 2;
        r2 *= 2;
        if (r2 >= ad) q2++, r2 -= ad;
        delta = ad - r2;
    } while (q1 < delta || (q1 == delta && r1 == 0));
    *m = (int64_t)(q2 + 1);
    *shift = p - 64;
}

/* x / d and x % d by a positive constant, floored: a shift and a mask for a power of two, else smulh by the
 * magic number, then the remainder's sign fixed (a division takes 7 to 20 cycles on ARM, this 3 or 4). */
static void divideByConstant(Gen* g, const IrIns* in) {
    int64_t d = kOf(g, in->b);
    bool quotient = in->op == IR_DIV;
    int n = gprIn(g, in->a, X0);
    int out = gprOut(g, in->dst, X2);
    if (d == 1) {
        if (quotient) movRR(g, out, n);
        else movImm(g, out, 0);
        doneGpr(g, in->dst, out);
        return;
    }
    if ((d & (d - 1)) == 0) {
        int k = 0;
        while ((INT64_C(1) << k) < d) k++;
        if (quotient) ins(g, 0x9340FC00u | (uint32_t)k << 16 | (uint32_t)n << 5 | (uint32_t)out); /* asr: rounds down */
        else { /* and out, n, d - 1 */
            movImm(g, X17, d - 1);
            ins(g, 0x8A000000u | (uint32_t)X17 << 16 | (uint32_t)n << 5 | (uint32_t)out);
        }
        doneGpr(g, in->dst, out);
        return;
    }
    int64_t m;
    int shift;
    magic(d, &m, &shift);
    movImm(g, X17, m);
    ins(g, 0x9B407C00u | (uint32_t)X17 << 16 | (uint32_t)n << 5 | (uint32_t)X2);       /* smulh x2, n, m */
    if (m < 0) add3(g, X2, X2, n, 0);
    if (shift > 0) ins(g, 0x9340FC00u | (uint32_t)shift << 16 | (uint32_t)X2 << 5 | (uint32_t)X2); /* asr x2, x2, shift */
    ins(g, 0x8B400000u | (uint32_t)n << 16 | 63u << 10 | (uint32_t)X2 << 5 | (uint32_t)X2); /* add x2, x2, n, lsr #63 */
    movImm(g, X17, d);
    ins(g, 0x9B008000u | (uint32_t)X17 << 16 | (uint32_t)n << 10 | (uint32_t)X2 << 5 | (uint32_t)X3); /* x3 = n - x2 * d */
    ins(g, 0xB6F80000u | 3u << 5 | (uint32_t)X3);                                     /* tbz x3, #63: not below 0 */
    ins(g, 0xD1000442u);                                                             /* sub x2, x2, #1 */
    add3(g, X3, X3, X17, 0);                                                         /* add x3, x3, d */
    storeBits(g, in->dst, quotient ? X2 : X3);
}

static const int intCond[] = {EQ, NE, LT, LE, GT, GE};
static const int floatCond[] = {EQ, NE, MI, LS, GT, GE};     /* false when unordered, but != */
static const int floatNotCond[] = {NE, EQ, PL, HI, LE, LT};  /* their negations, NaN included */

static void compare(Gen* g, const IrIns* in) {
    int a = gprIn(g, in->a, X0);
    if (isK(g, in->b) && kOf(g, in->b) >= 0 && kOf(g, in->b) < 4096)
        ins(g, 0xF100001Fu | (uint32_t)kOf(g, in->b) << 10 | (uint32_t)a << 5); /* cmp a, #k */
    else if (isK(g, in->b) && kOf(g, in->b) < 0 && kOf(g, in->b) > -4096)
        ins(g, 0xB100001Fu | (uint32_t)(-kOf(g, in->b)) << 10 | (uint32_t)a << 5); /* cmn a, #-k */
    else cmpRR(g, a, gprIn(g, in->b, X1));
}

static void fcompare(Gen* g, const IrIns* in) {
    int a = fprIn(g, in->a, D0), b = fprIn(g, in->b, D1);
    ins(g, 0x1E602000u | (uint32_t)b << 16 | (uint32_t)a << 5); /* fcmp a, b */
}

static void genIns(Gen* g, const IrIns* in) {
    g->inl = in->inl;
    switch (in->op) {
    case IR_CONST: {
        if (isK(g, in->dst)) break;
        if (in->c == 1) {
            movReloc(g, X0, RELOC_DATA, in->imm);
            storeBits(g, in->dst, X0);
        } else if (in->c == 2) { /* adr: a function kept as a value */
            Fixup f = {here(g), (int)in->imm, FIX_ADR};
            QK_PUSH(g->callFixups, g->callCount, g->callCap, f);
            ins(g, 0x10000000u | (uint32_t)X0);
            storeBits(g, in->dst, X0);
        } else if (isFloat(g, in->dst)) {
            movImm(g, X0, in->imm);
            storeBits(g, in->dst, X0);
        } else {
            int d = gprOut(g, in->dst, X0);
            movImm(g, d, in->imm);
            doneGpr(g, in->dst, d);
        }
        break;
    }
    case IR_MOV:
        if (isFloat(g, in->dst)) {
            int d = fprOut(g, in->dst, D0);
            loadF(g, d, in->a);
            doneFpr(g, in->dst, d);
        } else {
            int d = gprOut(g, in->dst, X0);
            loadBits(g, d, in->a);
            doneGpr(g, in->dst, d);
        }
        break;
    case IR_ADD: case IR_SUB: case IR_MUL: {
        int a = gprIn(g, in->a, X0);
        int d = gprOut(g, in->dst, X0);
        if (in->cond) { /* checked: past the limits, a panic */
            int b = gprIn(g, in->b, X1);
            if (in->op == IR_MUL) {
                ins(g, 0x9B407C00u | (uint32_t)b << 16 | (uint32_t)a << 5 | (uint32_t)X17); /* smulh x17, a, b */
                mul3(g, d, a, b);
                ins(g, 0xEB80001Fu | (uint32_t)d << 16 | 63u << 10 | (uint32_t)X17 << 5); /* cmp x17, d, asr #63 */
                coldJump(g, NE, PANIC_OVERFLOW, in->line, -1, -1);
            } else {
                ins(g, (in->op == IR_ADD ? 0xAB000000u : 0xEB000000u) | (uint32_t)b << 16 | (uint32_t)a << 5 | (uint32_t)d); /* adds / subs */
                coldJump(g, 6 /* vs */, PANIC_OVERFLOW, in->line, -1, -1);
            }
            doneGpr(g, in->dst, d);
            break;
        }
        if (in->op != IR_MUL && isK(g, in->b) && kOf(g, in->b) > -4096 && kOf(g, in->b) < 4096) {
            addImm(g, d, a, in->op == IR_ADD ? kOf(g, in->b) : -kOf(g, in->b));
        } else if (in->op == IR_MUL && isK(g, in->b) && kOf(g, in->b) > 0 && (kOf(g, in->b) & (kOf(g, in->b) - 1)) == 0) {
            int n = 0; /* x * 2^n: lsl */
            while ((INT64_C(1) << n) < kOf(g, in->b)) n++;
            ins(g, 0xD3400000u | (uint32_t)((64 - n) & 63) << 16 | (uint32_t)(63 - n) << 10 | (uint32_t)a << 5 | (uint32_t)d);
        } else if (in->op == IR_MUL && isK(g, in->b) && (kOf(g, in->b) == 3 || kOf(g, in->b) == 5 || kOf(g, in->b) == 9)) {
            add3(g, d, a, a, kOf(g, in->b) == 3 ? 1 : kOf(g, in->b) == 5 ? 2 : 3); /* x + x * 2^n */
        } else {
            int b = gprIn(g, in->b, X1);
            if (in->op == IR_ADD) add3(g, d, a, b, 0);
            else if (in->op == IR_SUB) sub3(g, d, a, b);
            else mul3(g, d, a, b);
        }
        doneGpr(g, in->dst, d);
        break;
    }
    case IR_DIV: case IR_MOD: { /* floored: the quotient rounds down, the remainder takes the divisor's sign */
        if (isK(g, in->b) && kOf(g, in->b) >= 1) { /* by a constant: a multiplication by its magic number */
            divideByConstant(g, in);
            break;
        }
        loadBits(g, X0, in->a);
        loadBits(g, X1, in->b);
        if (!(isK(g, in->b) && kOf(g, in->b) != 0)) { /* cbz x1: division by zero */
            Cold c = {here(g), PANIC_DIVIDE, in->line, -1, -1, g->inl};
            QK_PUSH(g->cold, g->coldCount, g->coldCap, c);
            ins(g, 0xB4000000u | (uint32_t)X1);
        }
        ins(g, 0x9AC00C00u | (uint32_t)X1 << 16 | (uint32_t)X0 << 5 | (uint32_t)X2); /* sdiv x2, x0, x1 */
        ins(g, 0x9B008000u | (uint32_t)X1 << 16 | (uint32_t)X0 << 10 | (uint32_t)X2 << 5 | (uint32_t)X3); /* msub x3 = x0 - x2*x1 */
        ins(g, 0xB4000000u | 5u << 5 | (uint32_t)X3);                                   /* cbz x3, past the adjustment */
        ins(g, 0xCA000000u | (uint32_t)X1 << 16 | (uint32_t)X3 << 5 | (uint32_t)X4);    /* eor x4, x3, x1 */
        ins(g, 0xB6F80000u | 3u << 5 | (uint32_t)X4);                                   /* tbz x4, #63, +3 words */
        ins(g, 0xD1000400u | (uint32_t)X2 << 5 | (uint32_t)X2);                         /* sub x2, x2, #1 */
        add3(g, X3, X3, X1, 0);                                                         /* add x3, x3, x1 */
        storeBits(g, in->dst, in->op == IR_DIV ? X2 : X3);
        break;
    }
    case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SAR: { /* and, orr, eor, lslv, asrv (the count modulo 64) */
        static const uint32_t ops[] = {0x8A000000u, 0xAA000000u, 0xCA000000u, 0x9AC02000u, 0x9AC02800u};
        int a = gprIn(g, in->a, X0), b = gprIn(g, in->b, X1), d = gprOut(g, in->dst, X0);
        ins(g, ops[in->op - IR_AND] | (uint32_t)b << 16 | (uint32_t)a << 5 | (uint32_t)d);
        doneGpr(g, in->dst, d);
        break;
    }
    case IR_NEG: {
        int a = gprIn(g, in->a, X0), d = gprOut(g, in->dst, X0);
        sub3(g, d, XZR, a);
        doneGpr(g, in->dst, d);
        break;
    }
    case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV: case IR_FMIN: case IR_FMAX: {
        static const uint32_t ops[] = {0x1E602800u, 0x1E603800u, 0x1E600800u, 0x1E601800u};
        uint32_t op = in->op == IR_FMIN ? 0x1E605800u : in->op == IR_FMAX ? 0x1E604800u : ops[in->op - IR_FADD];
        int a = fprIn(g, in->a, D0), b = fprIn(g, in->b, D1), d = fprOut(g, in->dst, D0);
        ins(g, op | (uint32_t)b << 16 | (uint32_t)a << 5 | (uint32_t)d);
        doneFpr(g, in->dst, d);
        break;
    }
    case IR_FNEG: case IR_SQRT: {
        int a = fprIn(g, in->a, D0), d = fprOut(g, in->dst, D0);
        ins(g, (in->op == IR_FNEG ? 0x1E614000u : 0x1E61C000u) | (uint32_t)a << 5 | (uint32_t)d);
        doneFpr(g, in->dst, d);
        break;
    }
    case IR_CMP: {
        compare(g, in);
        int d = gprOut(g, in->dst, X0);
        cset(g, d, intCond[in->cond]);
        doneGpr(g, in->dst, d);
        break;
    }
    case IR_FCMP: {
        fcompare(g, in);
        int d = gprOut(g, in->dst, X0);
        cset(g, d, floatCond[in->cond]);
        doneGpr(g, in->dst, d);
        break;
    }
    case IR_NOT: {
        int a = gprIn(g, in->a, X0);
        ins(g, 0xF100001Fu | (uint32_t)a << 5); /* cmp a, #0 */
        int d = gprOut(g, in->dst, X0);
        cset(g, d, EQ);
        doneGpr(g, in->dst, d);
        break;
    }
    case IR_I2F: {
        int a = gprIn(g, in->a, X0), d = fprOut(g, in->dst, D0);
        ins(g, 0x9E620000u | (uint32_t)a << 5 | (uint32_t)d); /* scvtf */
        doneFpr(g, in->dst, d);
        break;
    }
    case IR_F2I: {
        int a = fprIn(g, in->a, D0), d = gprOut(g, in->dst, X0);
        ins(g, 0x9E780000u | (uint32_t)a << 5 | (uint32_t)d); /* fcvtzs */
        if (in->cond) { /* too big: the limits (saturated); NaN: unordered with itself */
            ins(g, 0x1E602000u | (uint32_t)a << 16 | (uint32_t)a << 5); /* fcmp a, a */
            coldJump(g, 6 /* vs */, PANIC_TOO_BIG, in->line, -1, -1);
            ins(g, 0xF100041Fu | (uint32_t)d << 5); /* cmp d, #1: overflows on the minimum */
            coldJump(g, 6, PANIC_TOO_BIG, in->line, -1, -1);
            ins(g, 0xB100041Fu | (uint32_t)d << 5); /* cmn d, #1: overflows on the maximum */
            coldJump(g, 6, PANIC_TOO_BIG, in->line, -1, -1);
        }
        doneGpr(g, in->dst, d);
        break;
    }
    case IR_ROUND: { /* nearest (ties to even), floor, ceil: as SSE4.1's roundsd */
        static const uint32_t ops[] = {0x1E644000u, 0x1E654000u, 0x1E64C000u};
        int a = fprIn(g, in->a, D0), d = fprOut(g, in->dst, D0);
        ins(g, ops[in->imm >= 0 && in->imm <= 2 ? in->imm : 0] | (uint32_t)a << 5 | (uint32_t)d);
        doneFpr(g, in->dst, d);
        break;
    }
    case IR_LOAD: case IR_STORE: {
        if (in->imm == 8 && in->c == 0 && !isK(g, in->b)) { /* ldr / str v, [a, b, lsl #3] */
            int base = gprIn(g, in->a, X1), idx = gprIn(g, in->b, X2);
            int v = in->dst;
            bool load = in->op == IR_LOAD, fp = isFloat(g, v) && inReg(g, v);
            int r = inReg(g, v) ? (fp ? fprOf(g, v) : gprOf(g, v)) : X0;
            if (!load && !inReg(g, v)) loadBits(g, X0, v);
            uint32_t op = fp ? (load ? 0xFC607800u : 0xFC207800u) : (load ? 0xF8607800u : 0xF8207800u);
            ins(g, op | (uint32_t)idx << 16 | (uint32_t)base << 5 | (uint32_t)r);
            if (load && !inReg(g, v)) storeBits(g, v, X0);
            break;
        }
        int64_t off;
        int base = addressOf(g, in, &off);
        int v = in->dst;
        bool load = in->op == IR_LOAD;
        if (isFloat(g, v) && inReg(g, v)) access(g, load, true, fprOf(g, v), base, off);
        else if (!isFloat(g, v) && inReg(g, v)) access(g, load, false, gprOf(g, v), base, off);
        else if (load) {
            access(g, true, false, X0, base, off);
            storeBits(g, v, X0);
        } else {
            loadBits(g, X0, v);
            access(g, false, false, X0, base, off);
        }
        break;
    }
    case IR_BOUNDS: { /* a (index) < b (length), unsigned */
        int a = gprIn(g, in->a, X0);
        if (isK(g, in->b) && kOf(g, in->b) >= 0 && kOf(g, in->b) < 4096) ins(g, 0xF100001Fu | (uint32_t)kOf(g, in->b) << 10 | (uint32_t)a << 5);
        else cmpRR(g, a, gprIn(g, in->b, X1));
        coldJump(g, HS, (int)in->imm, in->line, in->a, in->b);
        break;
    }
    case IR_LABEL: g->labelAt[in->imm] = here(g); break;
    case IR_JMP: bLabel(g, (int)in->imm); break;
    case IR_JZ: case IR_JNZ: {
        int a = gprIn(g, in->a, X0);
        fixup(g, (int)in->imm, FIX_CBZ, (in->op == IR_JZ ? 0xB4000000u : 0xB5000000u) | (uint32_t)a);
        break;
    }
    case IR_CALL:
        for (int i = 0; i < in->argCount; i++) {
            loadBits(g, X16, in->args[i]);
            access(g, false, false, X16, SP, 8 * i);
        }
        if (in->a >= 0) { /* blr: a function kept as a value */
            loadBits(g, X16, in->a);
            ins(g, 0xD63F0000u | (uint32_t)X16 << 5);
        } else {
            Fixup f = {here(g), (int)in->imm, FIX_BL};
            QK_PUSH(g->callFixups, g->callCount, g->callCap, f);
            ins(g, 0x94000000u);
        }
        {
            CallSite s = {here(g), in->line, g->fnIndex, (int32_t)in->imm, in->inl};
            QK_PUSH(g->mc->sites, g->mc->siteCount, g->mc->siteCap, s);
        }
        for (int i = 0; i < in->retCount; i++) {
            access(g, true, false, X16, SP, 8 * i);
            storeBits(g, in->rets[i], X16);
        }
        break;
    case IR_RTCALL: {
        const RtFn* f = &qkRuntime[in->imm];
        if (in->imm == RT_FAIL) markPanicFrame(g);
        cArguments(g, in->args, in->argCount, f->args);
        callRuntime(g, (int)in->imm);
        if (in->retCount) {
            if (f->ret == 'F') storeF(g, in->rets[0], D0);
            else storeBits(g, in->rets[0], X0);
        }
        break;
    }
    case IR_HOSTCALL: {
        const IrExtern* x = &g->prog->externs[in->imm];
        bool allFloat = in->retCount > 0;
        for (int i = 0; i < in->retCount; i++) allFloat &= x->retKinds[i] == 'F';
        bool hfa = allFloat && in->retCount <= 4;            /* d0-d3 */
        bool inRegs = hfa || in->retCount <= 2;              /* x0, x1 (a 16-byte record's bytes) */
        int buffer = 8 * stackArgs(x->argKinds, in->argCount); /* past the stack arguments: room for the record */
        cArguments(g, in->args, in->argCount, x->argKinds);
        if (!inRegs) addImm(g, X8, SP, buffer);
        movReloc(g, X16, RELOC_HOST, in->imm);
        ins(g, 0xD63F0000u | (uint32_t)X16 << 5);
        if (!inRegs) {
            for (int i = 0; i < in->retCount; i++) {
                access(g, true, false, X16, SP, buffer + 8 * i);
                storeBits(g, in->rets[i], X16);
            }
        } else if (hfa) {
            for (int i = in->retCount - 1; i >= 0; i--) storeF(g, in->rets[i], i);
        } else if (in->retCount == 1 && x->retKinds[0] == 'F') {
            storeF(g, in->rets[0], D0);
        } else {
            for (int i = in->retCount - 1; i >= 0; i--) storeBits(g, in->rets[i], i); /* x1 first: x0 is read last */
        }
        break;
    }
    case IR_RET:
        for (int i = 0; i < in->argCount; i++) {
            loadBits(g, X16, in->args[i]);
            access(g, false, false, X16, FP, 16 + 8 * i);
        }
        bLabel(g, g->epilogue);
        break;
    case IR_FUEL:
        movReloc(g, X16, RELOC_FUEL, 0);
        access(g, true, false, X17, X16, 0);
        ins(g, 0xF1000400u | (uint32_t)X17 << 5 | (uint32_t)X17); /* subs x17, x17, #1 */
        access(g, false, false, X17, X16, 0);
        coldJump(g, MI, PANIC_FUEL, in->line, -1, -1);
        break;
    case IR_NOP: break;
    }
}

/* A comparison whose only reader is the jump right after it: one compare, one conditional branch. */
static bool fusedBranch(Gen* g, const IrIns* c, const IrIns* j) {
    if ((c->op != IR_CMP && c->op != IR_FCMP) || (j->op != IR_JZ && j->op != IR_JNZ)) return false;
    if (j->a != c->dst || g->uses[c->dst] != 1) return false;
    bool whenTrue = j->op == IR_JNZ;
    g->inl = c->inl;
    if (c->op == IR_CMP) {
        compare(g, c);
        bcondLabel(g, whenTrue ? intCond[c->cond] : intCond[c->cond] ^ 1, (int)j->imm);
    } else {
        fcompare(g, c);
        bcondLabel(g, whenTrue ? floatCond[c->cond] : floatNotCond[c->cond], (int)j->imm);
    }
    return true;
}

/* ---- registers ----------------------------------------------------------------------------------- */

/* Linear scan, as on x86-64: a register belongs to a virtual register only while it is alive (stretched over
 * the loops it is alive across); a value no call crosses takes a register calls may change. */
static void allocate(Gen* g, const IrFn* f, int* slotsOut) {
    int n = f->vregCount ? f->vregCount : 1;
    int* first = qkNew(sizeof(int) * (size_t)n);
    int* last = qkNew(sizeof(int) * (size_t)n);
    for (int v = 0; v < f->vregCount; v++) {
        first[v] = INT32_MAX;
        last[v] = -1;
    }
#define MENTION(v, k)                             \
    do {                                          \
        int v_ = (v);                             \
        if (v_ >= 0) {                            \
            if ((k) < first[v_]) first[v_] = (k); \
            if ((k) > last[v_]) last[v_] = (k);   \
        }                                         \
    } while (0)
    for (int i = 0; i < f->paramCount; i++) MENTION(f->params[i], 0);
    int* labelAt = qkNew(sizeof(int) * (size_t)(f->labelCount + 1));
    for (int k = 0; k < f->count; k++) {
        const IrIns* in = &f->code[k];
        if (in->op == IR_LABEL) {
            labelAt[in->imm] = k;
            continue;
        }
        if (in->op == IR_JMP) continue;
        MENTION(in->dst, k);
        if (in->op != IR_CONST) {
            MENTION(in->a, k);
            MENTION(in->b, k);
        }
        for (int i = 0; i < in->argCount; i++) MENTION(in->args[i], k);
        for (int i = 0; i < in->retCount; i++) MENTION(in->rets[i], k);
    }
#undef MENTION
    for (bool changed = true; changed;) {
        changed = false;
        for (int k = 0; k < f->count; k++) {
            const IrIns* in = &f->code[k];
            if (in->op != IR_JMP && in->op != IR_JZ && in->op != IR_JNZ) continue;
            int top = labelAt[in->imm];
            if (top >= k) continue;
            for (int v = 0; v < f->vregCount; v++)
                if (first[v] < top && last[v] >= top && last[v] < k) {
                    last[v] = k;
                    changed = true;
                }
        }
    }
    int* order = qkNew(sizeof(int) * (size_t)n);
    int count = 0;
    for (int v = 0; v < f->vregCount; v++) {
        g->home[v] = INT32_MIN;
        if (last[v] >= 0 && !(f->isConst && f->isConst[v]) && f->weight[v] > 0) order[count++] = v;
    }
    for (int i = 1; i < count; i++) {
        int v = order[i], j = i - 1;
        while (j >= 0 && first[order[j]] > first[v]) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = v;
    }
    static const int pool[2][24] = {
        {9, 10, 11, 12, 13, 14, 15, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28},
        {16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 8, 9, 10, 11, 12, 13, 14, 15}};
    static const int volatileCount[2] = {7, 16};
    static const int poolSize[2] = {17, 24};
    bool* crosses = qkNew((size_t)n);
    for (int k = 0; k < f->count; k++) {
        int op = f->code[k].op;
        if (op != IR_CALL && op != IR_RTCALL && op != IR_HOSTCALL) continue;
        for (int v = 0; v < f->vregCount; v++)
            if (first[v] < k && last[v] >= k) crosses[v] = true;
    }
    int owner[2][24];
    for (int c = 0; c < 2; c++)
        for (int r = 0; r < 24; r++) owner[c][r] = -1;
    g->savedGpr = g->savedFpr = 0;
    for (int i = 0; i < count; i++) {
        int v = order[i];
        int c = f->kinds[v] == 'F' ? 1 : 0;
        int from = crosses[v] ? volatileCount[c] : 0;
        int pick = -1;
        for (int r = 0; r < poolSize[c]; r++) {
            int o = owner[c][r];
            if (o >= 0 && last[o] < first[v]) owner[c][r] = o = -1;
            if (o < 0 && pick < 0 && r >= from) pick = r;
        }
        if (pick < 0) {
            int lightest = -1;
            for (int r = from; r < poolSize[c]; r++)
                if (lightest < 0 || f->weight[owner[c][r]] < f->weight[owner[c][lightest]]) lightest = r;
            if (lightest >= 0 && f->weight[owner[c][lightest]] < f->weight[v]) {
                g->home[owner[c][lightest]] = INT32_MIN;
                pick = lightest;
            }
        }
        if (pick < 0) continue;
        owner[c][pick] = v;
        g->home[v] = (c ? 32 : 0) + pool[c][pick];
        if (pick >= volatileCount[c]) {
            int saved = pick - volatileCount[c] + 1;
            if (c && saved > g->savedFpr) g->savedFpr = saved;
            if (!c && saved > g->savedGpr) g->savedGpr = saved;
        }
    }
    int slots = 0;
    for (int v = 0; v < f->vregCount; v++)
        if (g->home[v] == INT32_MIN) g->home[v] = -(++slots);
    *slotsOut = slots;
}

/* ---- functions ----------------------------------------------------------------------------------- */

static void genFn(Gen* g, const IrFn* f) {
    g->fn = f;
    g->home = qkNew(sizeof(int) * (size_t)(f->vregCount ? f->vregCount : 1));
    int slots;
    allocate(g, f, &slots);
    int outSlots = f->maxOut > 1 ? f->maxOut : 1;
    for (int k = 0; k < f->count; k++) { /* a host call's stack arguments and the record it returns through x8 */
        const IrIns* in = &f->code[k];
        if (in->op != IR_HOSTCALL && in->op != IR_RTCALL) continue;
        const char* kinds = in->op == IR_HOSTCALL ? g->prog->externs[in->imm].argKinds : qkRuntime[in->imm].args;
        int need = stackArgs(kinds, in->argCount) + in->retCount;
        if (need > outSlots) outSlots = need;
    }
    g->out = 8 * outSlots;
    int saved = 8 * (g->savedGpr + g->savedFpr);
    g->frame = (g->out + 8 * slots + saved + 15) & ~15;
    g->labelAt = qkNew(sizeof(int) * (size_t)(f->labelCount + 1));
    g->epilogue = f->labelCount;
    g->fixupCount = 0;

    /* prologue */
    ins(g, 0xA9BF7BFDu);         /* stp x29, x30, [sp, #-16]! */
    movRR(g, FP, SP);            /* mov x29, sp */
    addImm(g, SP, SP, -g->frame);
    for (int i = 0; i < g->savedGpr; i++) access(g, false, false, 19 + i, FP, -8 * (i + 1));
    for (int i = 0; i < g->savedFpr; i++) access(g, false, true, 8 + i, FP, -8 * (g->savedGpr + i + 1));
    bool calls = false;
    for (int k = 0; k < f->count && !calls; k++) calls = f->code[k].op == IR_CALL;
    g->inl = 0;
    if (calls) { /* too many calls deep: a panic, not a crash */
        movReloc(g, X16, RELOC_STACK_LIMIT, 0);
        access(g, true, false, X16, X16, 0);
        movRR(g, X17, SP);
        cmpRR(g, X17, X16);
        coldJump(g, LO, PANIC_STACK, f->line, -1, -1);
    }
    for (int i = 0; i < f->paramCount; i++) {
        int v = f->params[i];
        if (inReg(g, v) && isFloat(g, v)) access(g, true, true, fprOf(g, v), FP, 16 + 8 * i);
        else if (inReg(g, v)) access(g, true, false, gprOf(g, v), FP, 16 + 8 * i);
        else {
            access(g, true, false, X16, FP, 16 + 8 * i);
            storeBits(g, v, X16);
        }
    }
    g->uses = qkNew(sizeof(int) * (size_t)(f->vregCount ? f->vregCount : 1));
    for (int k = 0; k < f->count; k++) {
        const IrIns* in = &f->code[k];
        if (in->op == IR_LABEL || in->op == IR_JMP) continue;
        if (in->op == IR_STORE && in->dst >= 0) g->uses[in->dst]++;
        if (in->op != IR_CONST) {
            if (in->a >= 0) g->uses[in->a]++;
            if (in->b >= 0) g->uses[in->b]++;
        }
        for (int i = 0; i < in->argCount; i++) g->uses[in->args[i]]++;
    }
    for (int k = 0; k < f->count; k++) {
        if (k + 1 < f->count && fusedBranch(g, &f->code[k], &f->code[k + 1])) {
            k++;
            continue;
        }
        genIns(g, &f->code[k]);
    }

    /* epilogue */
    g->labelAt[g->epilogue] = here(g);
    for (int i = 0; i < g->savedGpr; i++) access(g, true, false, 19 + i, FP, -8 * (i + 1));
    for (int i = 0; i < g->savedFpr; i++) access(g, true, true, 8 + i, FP, -8 * (g->savedGpr + i + 1));
    movRR(g, SP, FP);
    ins(g, 0xA8C17BFDu); /* ldp x29, x30, [sp], #16 */
    ins(g, 0xD65F03C0u); /* ret */
    emitCold(g);

    for (int i = 0; i < g->fixupCount; i++) {
        Fixup* fx = &g->fixups[i];
        int32_t rel = (g->labelAt[fx->target] - fx->at) / 4;
        uint32_t w = wordAt(g, fx->at);
        if (fx->kind == FIX_B) w |= (uint32_t)rel & 0x3FFFFFF;
        else w |= ((uint32_t)rel & 0x7FFFF) << 5;
        patch(g, fx->at, w);
    }
}

/* ---- the way in from C, and the way out of a panic ------------------------------------------------ */

/* int64_t enter(QkEnter* e): keeps every callee-saved register (a panic abandons the frames that would
 * restore them), keeps its stack pointer where the escape finds it, copies the argument slots to the outgoing
 * area, calls the function, copies the result slots back, returns 0. void escape(void): from the runtime's
 * panic, back onto that stack: enter returns 1. Frame below x29: x19-x28, d8-d15, the outer call's escape. */
static void genTrampoline(Gen* g) {
    while (here(g) % 16) ins(g, 0xD503201Fu);
    g->mc->enter = here(g);
    ins(g, 0xA9BF7BFDu); /* stp x29, x30, [sp, #-16]! */
    movRR(g, FP, SP);
    addImm(g, SP, SP, -160);
    for (int i = 0; i < 10; i++) access(g, false, false, 19 + i, FP, -8 * (i + 1));
    for (int i = 0; i < 8; i++) access(g, false, true, 8 + i, FP, -8 * (11 + i));
    movReloc(g, X16, RELOC_ESCAPE, 0);
    access(g, true, false, X17, X16, 0);
    access(g, false, false, X17, FP, -152); /* the outer call's escape stack, kept */
    movRR(g, X17, SP);
    access(g, false, false, X17, X16, 0);   /* ours */
    movRR(g, X19, X0);                      /* e */
    /* the outgoing area: 8 bytes per slot (arguments or results, the larger), 16 aligned */
    access(g, true, false, X1, X19, 16);
    access(g, true, false, X2, X19, 32);
    cmpRR(g, X1, X2);
    ins(g, 0x9A800000u | (uint32_t)X2 << 16 | (uint32_t)GE << 12 | (uint32_t)X1 << 5 | (uint32_t)X1); /* csel x1, x1, x2, ge */
    ins(g, 0xD37DF021u);                                        /* lsl x1, x1, #3 */
    addImm(g, X1, X1, 31);
    ins(g, 0xD344FC21u);                                        /* lsr x1, x1, #4 */
    ins(g, 0xD37CEC21u);                                        /* lsl x1, x1, #4 */
    ins(g, 0xCB2163FFu);                                        /* sub sp, sp, x1 */
    /* arguments */
    access(g, true, false, X2, X19, 8);
    access(g, true, false, X3, X19, 16);
    movImm(g, X4, 0);
    int loop1 = here(g);
    cmpRR(g, X4, X3);
    int exit1 = here(g);
    ins(g, 0x54000000u | GE);
    ins(g, 0xF8647845u);                                        /* ldr x5, [x2, x4, lsl #3] */
    ins(g, 0xF8247BE5u);                                        /* str x5, [sp, x4, lsl #3] */
    ins(g, 0x91000484u);                                        /* add x4, x4, #1 */
    ins(g, 0x14000000u | ((uint32_t)((loop1 - here(g)) / 4) & 0x3FFFFFF));
    patch(g, exit1, wordAt(g, exit1) | ((uint32_t)((here(g) - exit1) / 4) & 0x7FFFF) << 5);
    access(g, true, false, X16, X19, 0);
    ins(g, 0xD63F0000u | (uint32_t)X16 << 5);                  /* blr x16 */
    /* results */
    access(g, true, false, X2, X19, 24);
    access(g, true, false, X3, X19, 32);
    movImm(g, X4, 0);
    int loop2 = here(g);
    cmpRR(g, X4, X3);
    int exit2 = here(g);
    ins(g, 0x54000000u | GE);
    ins(g, 0xF8647BE5u);                                        /* ldr x5, [sp, x4, lsl #3] */
    ins(g, 0xF8247845u);                                        /* str x5, [x2, x4, lsl #3] */
    ins(g, 0x91000484u);
    ins(g, 0x14000000u | ((uint32_t)((loop2 - here(g)) / 4) & 0x3FFFFFF));
    patch(g, exit2, wordAt(g, exit2) | ((uint32_t)((here(g) - exit2) / 4) & 0x7FFFF) << 5);
    movImm(g, X0, 0);
    int exit = here(g);
    movReloc(g, X16, RELOC_ESCAPE, 0); /* the outer call's escape stack back */
    access(g, true, false, X17, FP, -152);
    access(g, false, false, X17, X16, 0);
    for (int i = 0; i < 10; i++) access(g, true, false, 19 + i, FP, -8 * (i + 1));
    for (int i = 0; i < 8; i++) access(g, true, true, 8 + i, FP, -8 * (11 + i));
    movRR(g, SP, FP);
    ins(g, 0xA8C17BFDu); /* ldp x29, x30, [sp], #16 */
    ins(g, 0xD65F03C0u);

    while (here(g) % 16) ins(g, 0xD503201Fu);
    g->mc->escape = here(g);
    movReloc(g, X16, RELOC_ESCAPE, 0);
    access(g, true, false, X17, X16, 0);
    movRR(g, SP, X17);
    addImm(g, FP, SP, 160);
    movImm(g, X0, 1);
    ins(g, 0x14000000u | ((uint32_t)((exit - here(g)) / 4) & 0x3FFFFFF));
}

bool qkGenArm64(const IrProgram* p, MachineCode* out) {
    memset(out, 0, sizeof *out);
    Gen g = {0};
    g.mc = out;
    g.b = &out->code;
    g.prog = p;
    g.fnAt = qkNew(sizeof(int) * (size_t)(p->count ? p->count : 1));
    for (int i = 0; i < p->count; i++) {
        while (here(&g) % 16) ins(&g, 0xD503201Fu);
        g.fnAt[i] = here(&g);
        g.fnIndex = i;
        genFn(&g, &p->fns[i]);
    }
    genTrampoline(&g);
    for (int i = 0; i < g.callCount; i++) {
        Fixup* fx = &g.callFixups[i];
        int32_t rel = g.fnAt[fx->target] - fx->at;
        uint32_t w = wordAt(&g, fx->at);
        if (fx->kind == FIX_BL) w |= (uint32_t)(rel / 4) & 0x3FFFFFF;
        else w |= ((uint32_t)rel & 3) << 29 | (((uint32_t)rel >> 2) & 0x7FFFF) << 5; /* adr: within a megabyte */
        patch(&g, fx->at, w);
    }
    out->fnCount = p->count;
    out->fnAt = qkNew(sizeof(int32_t) * (size_t)(p->count ? p->count : 1));
    for (int i = 0; i < p->count; i++) out->fnAt[i] = g.fnAt[i];
    return true;
}
