/* Quark — the x86-64 backend (Windows x64 calling convention for the runtime). No assembler, no linker:
 * instructions are encoded here, byte by byte.
 *
 * Registers: the most used virtual registers (loops weigh 8x per level) own a callee-saved register for the
 * whole function — rbx, rsi, rdi, r12-r15 for ints, xmm6-xmm15 for floats; the rest live in the frame.
 * rax, rcx, rdx, r8-r11 and xmm0-xmm5 are scratch.
 *
 * Calls between Quark functions: arguments go to [rsp + 8i] of the caller (its outgoing area), the callee
 * reads them at [rbp + 16 + 8i] and writes its results there before returning; the caller reads them back.
 * Calls to the runtime follow Windows x64 (rcx, rdx, r8, r9 / xmm0-3, 32 bytes of shadow space). */
#include "quark.h"

#include <stdio.h>
#include <stdlib.h>

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };

typedef struct Fixup {
    int at;     /* the rel32 field */
    int target; /* label, or function index */
} Fixup;

typedef struct Cold {
    int at;      /* the rel32 of the jump to it */
    int code, line;
    int a, b;    /* registers to report (-1: none) */
} Cold;

typedef struct Gen {
    MachineCode* mc;
    QkBuf* b;
    const IrFn* fn;
    const IrProgram* prog;
    int* home;        /* >= 0: register (GPR 0-15, XMM 16-31); < 0: slot -(n+1) */
    int pushed;       /* callee-saved GPRs pushed */
    int savedGpr[8];
    int savedXmm[10];
    int xmmCount;
    int slotBase;     /* bytes below rbp where slots start */
    int xmmBase;
    int* labelAt;
    Fixup* fixups;
    int fixupCount, fixupCap;
    int epilogue;     /* label */
    struct Cold* cold; /* panics, placed after the function so loops stay short and fall through */
    int coldCount, coldCap;
    int* fnAt;        /* each function's offset */
    int* uses;        /* how often each virtual register is read */
    char* loopTop;    /* labels a jump goes back to */
    bool afterJump;   /* the last instruction was an unconditional jump */
    Fixup* callFixups;
    int callCount, callCap;
} Gen;

/* ---- encoding ------------------------------------------------------------------------------------ */

static void byte(Gen* g, int v) { qkBufByte(g->b, (uint8_t)v); }
static void i32(Gen* g, int32_t v) { qkBufAdd(g->b, &v, 4); }
static void i64(Gen* g, int64_t v) { qkBufAdd(g->b, &v, 8); }
static int here(Gen* g) { return (int)g->b->len; }

static void rex(Gen* g, bool w, int reg, int index, int base, bool force) {
    int r = 0x40 | (w ? 8 : 0) | ((reg >> 3) & 1) << 2 | ((index >> 3) & 1) << 1 | ((base >> 3) & 1);
    if (r != 0x40 || force) byte(g, r);
}

/* op reg, rm (both registers) */
static void rr(Gen* g, bool w, const uint8_t* op, int opLen, int reg, int rm) {
    rex(g, w, reg, 0, rm, false);
    for (int i = 0; i < opLen; i++) byte(g, op[i]);
    byte(g, 0xC0 | (reg & 7) << 3 | (rm & 7));
}

/* op reg, [base + disp32] */
static void rm(Gen* g, bool w, const uint8_t* op, int opLen, int reg, int base, int32_t disp) {
    rex(g, w, reg, 0, base, false);
    for (int i = 0; i < opLen; i++) byte(g, op[i]);
    byte(g, 0x80 | (reg & 7) << 3 | (base & 7));
    if ((base & 7) == RSP) byte(g, 0x24);
    i32(g, disp);
}

/* SSE: prefix, then REX, then 0F op */
static void sseRR(Gen* g, int prefix, bool w, int op, int reg, int rmReg) {
    if (prefix) byte(g, prefix);
    rex(g, w, reg, 0, rmReg, false);
    byte(g, 0x0F);
    byte(g, op);
    byte(g, 0xC0 | (reg & 7) << 3 | (rmReg & 7));
}

static void sseRM(Gen* g, int prefix, bool w, int op, int reg, int base, int32_t disp) {
    if (prefix) byte(g, prefix);
    rex(g, w, reg, 0, base, false);
    byte(g, 0x0F);
    byte(g, op);
    byte(g, 0x80 | (reg & 7) << 3 | (base & 7));
    if ((base & 7) == RSP) byte(g, 0x24);
    i32(g, disp);
}

static const uint8_t OP_MOV_LOAD[] = {0x8B}, OP_MOV_STORE[] = {0x89}, OP_ADD[] = {0x03}, OP_SUB[] = {0x2B},
                     OP_IMUL[] = {0x0F, 0xAF}, OP_CMP[] = {0x3B}, OP_TEST[] = {0x85}, OP_XOR[] = {0x33};

static void movRR(Gen* g, int dst, int src) {
    if (dst != src) rr(g, true, OP_MOV_LOAD, 1, dst, src);
}

static void movImm(Gen* g, int reg, int64_t v) {
    if (v >= INT32_MIN && v <= INT32_MAX) { /* mov r/m64, imm32 (sign extended) */
        rex(g, true, 0, 0, reg, false);
        byte(g, 0xC7);
        byte(g, 0xC0 | (reg & 7));
        i32(g, (int32_t)v);
    } else {
        rex(g, true, 0, 0, reg, false);
        byte(g, 0xB8 | (reg & 7));
        i64(g, v);
    }
}

/* mov reg, imm64 with an address patched at load time */
static void movReloc(Gen* g, int reg, int kind, int64_t target) {
    rex(g, true, 0, 0, reg, false);
    byte(g, 0xB8 | (reg & 7));
    Reloc r = {here(g), kind, target};
    QK_PUSH(g->mc->relocs, g->mc->relocCount, g->mc->relocCap, r);
    i64(g, 0);
}

static void jccLabel(Gen* g, int cc, int target) { /* cc: the low nibble of 0F 8x */
    byte(g, 0x0F);
    byte(g, 0x80 | cc);
    Fixup f = {here(g), target};
    QK_PUSH(g->fixups, g->fixupCount, g->fixupCap, f);
    i32(g, 0);
}

static void jmpLabel(Gen* g, int target) {
    byte(g, 0xE9);
    Fixup f = {here(g), target};
    QK_PUSH(g->fixups, g->fixupCount, g->fixupCap, f);
    i32(g, 0);
}

/* a short forward jump, patched by `land` */
static int jccShort(Gen* g, int cc) {
    byte(g, 0x70 | cc);
    byte(g, 0);
    return here(g);
}
static int jmpShort(Gen* g) {
    byte(g, 0xEB);
    byte(g, 0);
    return here(g);
}
static void land(Gen* g, int after) { g->b->data[after - 1] = (char)(here(g) - after); }

enum { CC_O = 0, CC_B = 2, CC_AE = 3, CC_E = 4, CC_NE = 5, CC_BE = 6, CC_A = 7, CC_S = 8, CC_NS = 9, CC_P = 10, CC_NP = 11,
       CC_L = 12, CC_GE = 13, CC_LE = 14, CC_G = 15 };

static void setcc(Gen* g, int cc, int reg8) { /* setcc r8; reg8 < 4 (al, cl, dl, bl) */
    byte(g, 0x0F);
    byte(g, 0x90 | cc);
    byte(g, 0xC0 | reg8);
}

static void movzxEaxAl(Gen* g) {
    byte(g, 0x0F);
    byte(g, 0xB6);
    byte(g, 0xC0);
}

/* ---- where a virtual register lives -------------------------------------------------------------- */

static bool isK(Gen* g, int v) { return g->fn->isConst && g->fn->isConst[v]; }
static int64_t kOf(Gen* g, int v) { return g->fn->constValue[v]; }
static bool fits32(int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }
static bool inReg(Gen* g, int v) { return !isK(g, v) && g->home[v] >= 0; }
static int gprOf(Gen* g, int v) { return g->home[v]; }
static int xmmOf(Gen* g, int v) { return g->home[v] - 16; }
static int32_t slotDisp(Gen* g, int v) { return -(g->slotBase + 8 * (-g->home[v])); }
static bool isFloat(Gen* g, int v) { return g->fn->kinds[v] == 'F'; }

/* Load the 64 bits of any virtual register into a GPR. */
static void loadBits(Gen* g, int reg, int v) {
    if (isK(g, v)) {
        movImm(g, reg, kOf(g, v));
    } else if (inReg(g, v)) {
        if (isFloat(g, v)) sseRR(g, 0x66, true, 0x7E, xmmOf(g, v), reg); /* movq reg, xmm */
        else movRR(g, reg, gprOf(g, v));
    } else {
        rm(g, true, OP_MOV_LOAD, 1, reg, RBP, slotDisp(g, v));
    }
}

static void storeBits(Gen* g, int v, int reg) {
    if (inReg(g, v)) {
        if (isFloat(g, v)) sseRR(g, 0x66, true, 0x6E, xmmOf(g, v), reg); /* movq xmm, reg */
        else movRR(g, gprOf(g, v), reg);
    } else {
        rm(g, true, OP_MOV_STORE, 1, reg, RBP, slotDisp(g, v));
    }
}

static void loadF(Gen* g, int xmm, int v) {
    if (inReg(g, v)) {
        if (xmmOf(g, v) != xmm) sseRR(g, 0, false, 0x28, xmm, xmmOf(g, v)); /* movaps */
    } else {
        sseRM(g, 0xF2, false, 0x10, xmm, RBP, slotDisp(g, v)); /* movsd xmm, m64 */
    }
}

static void storeF(Gen* g, int v, int xmm) {
    if (inReg(g, v)) {
        if (xmmOf(g, v) != xmm) sseRR(g, 0, false, 0x28, xmmOf(g, v), xmm);
    } else {
        sseRM(g, 0xF2, false, 0x11, xmm, RBP, slotDisp(g, v)); /* movsd m64, xmm */
    }
}

/* op reg, v — v from its register or its slot */
static void aluWith(Gen* g, const uint8_t* op, int opLen, int reg, int v) {
    if (isK(g, v)) {
        int64_t k = kOf(g, v);
        int digit = op[0] == 0x03 ? 0 : op[0] == 0x2B ? 5 : op[0] == 0x3B ? 7 : op[0] == 0x33 ? 6 : -1;
        if (!fits32(k) || (opLen == 1 && digit < 0)) { /* through a scratch register */
            movImm(g, R11, k);
            rr(g, true, op, opLen, reg, R11);
            return;
        }
        if (opLen == 2) { /* imul reg, reg, imm32 */
            rex(g, true, reg, 0, reg, false);
            byte(g, 0x69);
            byte(g, 0xC0 | (reg & 7) << 3 | (reg & 7));
            i32(g, (int32_t)k);
            return;
        }
        rex(g, true, 0, 0, reg, false);
        if (k >= -128 && k <= 127) {
            byte(g, 0x83);
            byte(g, 0xC0 | digit << 3 | (reg & 7));
            byte(g, (int)(int8_t)k);
        } else {
            byte(g, 0x81);
            byte(g, 0xC0 | digit << 3 | (reg & 7));
            i32(g, (int32_t)k);
        }
        return;
    }
    if (inReg(g, v)) rr(g, true, op, opLen, reg, gprOf(g, v));
    else rm(g, true, op, opLen, reg, RBP, slotDisp(g, v));
}

static void sseWith(Gen* g, int prefix, int op, int xmm, int v) {
    if (inReg(g, v)) sseRR(g, prefix, false, op, xmm, xmmOf(g, v));
    else sseRM(g, prefix, false, op, xmm, RBP, slotDisp(g, v));
}

/* AVX, scalar double (VEX.F2.0F op): d = a op v, v from its register or its slot. Three-byte VEX. */
static void vexF2(Gen* g, int op, int d, int a, int v) {
    int rmReg = inReg(g, v) ? xmmOf(g, v) : RBP;
    byte(g, 0xC4);
    byte(g, (((d >> 3) & 1) ^ 1) << 7 | 1 << 6 | (((rmReg >> 3) & 1) ^ 1) << 5 | 0x01); /* R X B, map 0F */
    byte(g, ((~a) & 15) << 3 | 0x03);                                                  /* W0, vvvv, L0, pp F2 */
    byte(g, op);
    if (inReg(g, v)) {
        byte(g, 0xC0 | (d & 7) << 3 | (rmReg & 7));
    } else {
        byte(g, 0x80 | (d & 7) << 3 | RBP);
        i32(g, slotDisp(g, v));
    }
}

/* ---- calls --------------------------------------------------------------------------------------- */

static void callRuntime(Gen* g, RtId id) {
    movReloc(g, RAX, 0, id);
    byte(g, 0xFF); /* call rax */
    byte(g, 0xD0);
}

/* panic(code, line, a, b) — rax holds a, rdx holds b, set by the caller */
static void panic(Gen* g, int code, int line) {
    movRR(g, R8, RAX);
    movRR(g, R9, RDX);
    movImm(g, RCX, code);
    movImm(g, RDX, line);
    callRuntime(g, RT_PANIC);
}

static void coldJump(Gen* g, int cc, int code, int line, int a, int b) {
    byte(g, 0x0F);
    byte(g, 0x80 | cc);
    Cold c = {here(g), code, line, a, b};
    QK_PUSH(g->cold, g->coldCount, g->coldCap, c);
    i32(g, 0);
}

static void emitCold(Gen* g) {
    for (int i = 0; i < g->coldCount; i++) {
        Cold* c = &g->cold[i];
        int32_t rel = here(g) - (c->at + 4);
        memcpy(g->b->data + c->at, &rel, 4);
        if (c->a >= 0) loadBits(g, RAX, c->a);
        else rr(g, false, OP_XOR, 1, RAX, RAX);
        if (c->b >= 0) loadBits(g, RDX, c->b);
        else rr(g, false, OP_XOR, 1, RDX, RDX);
        panic(g, c->code, c->line);
    }
    g->coldCount = 0;
}

/* ---- one instruction ----------------------------------------------------------------------------- */

/* A memory operand [base + index * scale + disp], scale 1, 2, 4 or 8 (else the index is pre-multiplied). */
typedef struct Mem {
    int base, index; /* index < 0: none */
    int scale;
    int32_t disp;
} Mem;

static void memOp(Gen* g, int prefix, bool w, const uint8_t* op, int opLen, int reg, Mem m) {
    if (prefix) byte(g, prefix);
    rex(g, w, reg, m.index >= 0 ? m.index : 0, m.base, false);
    for (int i = 0; i < opLen; i++) byte(g, op[i]);
    if (m.index < 0) {
        byte(g, 0x80 | (reg & 7) << 3 | (m.base & 7));
        if ((m.base & 7) == RSP) byte(g, 0x24);
    } else {
        int ss = m.scale == 8 ? 3 : m.scale == 4 ? 2 : m.scale == 2 ? 1 : 0;
        byte(g, 0x84 | (reg & 7) << 3);
        byte(g, ss << 6 | (m.index & 7) << 3 | (m.base & 7));
    }
    i32(g, m.disp);
}

static Mem memOf(Gen* g, const IrIns* ins) {
    Mem m;
    m.base = inReg(g, ins->a) ? gprOf(g, ins->a) : RCX;
    if (m.base == RCX) loadBits(g, RCX, ins->a);
    m.disp = ins->c;
    m.scale = 1;
    int64_t scale = ins->imm;
    if (isK(g, ins->b) && fits32(kOf(g, ins->b) * scale + ins->c)) {
        m.index = -1;
        m.disp = (int32_t)(kOf(g, ins->b) * scale + ins->c);
        return m;
    }
    if (scale == 1 || scale == 2 || scale == 4 || scale == 8) {
        m.scale = (int)scale;
        m.index = inReg(g, ins->b) ? gprOf(g, ins->b) : RDX;
        if (m.index == RDX) loadBits(g, RDX, ins->b);
        return m;
    }
    loadBits(g, RDX, ins->b); /* rdx = index * scale */
    rex(g, true, RDX, 0, RDX, false);
    byte(g, 0x69);
    byte(g, 0xC0 | (RDX & 7) << 3 | (RDX & 7));
    i32(g, (int32_t)scale);
    m.index = RDX;
    return m;
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
        if (r1 >= anc) {
            q1++;
            r1 -= anc;
        }
        q2 *= 2;
        r2 *= 2;
        if (r2 >= ad) {
            q2++;
            r2 -= ad;
        }
        delta = ad - r2;
    } while (q1 < delta || (q1 == delta && r1 == 0));
    *m = (int64_t)(q2 + 1);
    *shift = p - 64;
}

static void shiftImm(Gen* g, int reg, int digit, int n) { /* shl /4, shr /5, sar /7 */
    rex(g, true, 0, 0, reg, false);
    byte(g, 0xC1);
    byte(g, 0xC0 | digit << 3 | (reg & 7));
    byte(g, n);
}

/* x / d and x % d by a positive constant, floored like every Int division: a multiplication by d's magic
 * number (or a shift and a mask for a power of two) instead of a division that takes 40 cycles or more. */
static void divideByConstant(Gen* g, const IrIns* ins) {
    int64_t d = kOf(g, ins->b);
    bool quotient = ins->op == IR_DIV;
    loadBits(g, R8, ins->a); /* n */
    if (d == 1) {
        if (quotient) movRR(g, RDX, R8);
        else rr(g, false, OP_XOR, 1, RDX, RDX);
        storeBits(g, ins->dst, RDX);
        return;
    }
    if ((d & (d - 1)) == 0) {
        int k = 0;
        while ((INT64_C(1) << k) < d) k++;
        movRR(g, RDX, R8);
        if (quotient) {
            shiftImm(g, RDX, 7, k); /* sar: rounds down */
        } else if (fits32(d - 1)) { /* and rdx, d - 1 */
            rex(g, true, 0, 0, RDX, false);
            byte(g, 0x81);
            byte(g, 0xE0 | (RDX & 7));
            i32(g, (int32_t)(d - 1));
        } else {
            movImm(g, R11, d - 1);
            rr(g, true, (const uint8_t[]){0x23}, 1, RDX, R11);
        }
        storeBits(g, ins->dst, RDX);
        return;
    }
    int64_t m;
    int shift;
    magic(d, &m, &shift);
    movImm(g, RAX, m);
    rex(g, true, 0, 0, R8, false); /* imul r8: rdx:rax = rax * r8 */
    byte(g, 0xF7);
    byte(g, 0xE8 | (R8 & 7));
    if (m < 0) rr(g, true, OP_ADD, 1, RDX, R8);
    if (shift > 0) shiftImm(g, RDX, 7, shift);
    movRR(g, RAX, R8); /* + 1 when n < 0: truncated toward zero */
    shiftImm(g, RAX, 5, 63);
    rr(g, true, OP_ADD, 1, RDX, RAX);
    /* r = n - q * d; when r < 0, q - 1 and r + d: rounded down */
    movRR(g, RCX, RDX);
    if (fits32(d)) {
        rex(g, true, RCX, 0, RCX, false);
        byte(g, 0x69);
        byte(g, 0xC0 | (RCX & 7) << 3 | (RCX & 7));
        i32(g, (int32_t)d);
    } else {
        movImm(g, R11, d);
        rr(g, true, OP_IMUL, 2, RCX, R11);
    }
    movRR(g, RAX, R8);
    rr(g, true, OP_SUB, 1, RAX, RCX);
    movRR(g, RCX, RAX);
    rr(g, true, OP_TEST, 1, RCX, RCX);
    int fine = jccShort(g, CC_NS);
    rex(g, true, 0, 0, RDX, false); /* dec rdx */
    byte(g, 0xFF);
    byte(g, 0xC8 | (RDX & 7));
    if (fits32(d)) {
        rex(g, true, 0, 0, RCX, false); /* add rcx, d */
        byte(g, 0x81);
        byte(g, 0xC0 | (RCX & 7));
        i32(g, (int32_t)d);
    } else {
        movImm(g, R11, d);
        rr(g, true, OP_ADD, 1, RCX, R11);
    }
    land(g, fine);
    storeBits(g, ins->dst, quotient ? RDX : RCX);
}

/* x + k, x * 2^n, x * 3, 5, 9 into another register: one lea or shift, no copy first. */
static bool leaFor(Gen* g, const IrIns* ins) {
    if (!inReg(g, ins->a) || !isK(g, ins->b) || !inReg(g, ins->dst)) return false;
    int64_t k = kOf(g, ins->b);
    int a = gprOf(g, ins->a), d = gprOf(g, ins->dst);
    static const uint8_t LEA[] = {0x8D};
    if ((ins->op == IR_ADD || ins->op == IR_SUB) && fits32(k) && fits32(-k)) {
        Mem m = {a, -1, 1, (int32_t)(ins->op == IR_ADD ? k : -k)};
        memOp(g, 0, true, LEA, 1, d, m);
        return true;
    }
    if (ins->op != IR_MUL) return false;
    if (k == 3 || k == 5 || k == 9) {
        Mem m = {a, a, (int)k - 1, 0};
        memOp(g, 0, true, LEA, 1, d, m);
        return true;
    }
    if (k > 1 && k < (INT64_C(1) << 31) && (k & (k - 1)) == 0) {
        int n = 0;
        while ((INT64_C(1) << n) < k) n++;
        movRR(g, d, a);
        rex(g, true, 0, 0, d, false); /* shl d, n */
        byte(g, 0xC1);
        byte(g, 0xE0 | (d & 7));
        byte(g, n);
        return true;
    }
    return false;
}

/* x = [m]; x = x + k; [m] = x, with x read nowhere else: add qword [m], k */
static bool addToMemory(Gen* g, const IrIns* ld, const IrIns* add, const IrIns* st) {
    if (ld->op != IR_LOAD || add->op != IR_ADD || st->op != IR_STORE) return false;
    int v = ld->dst;
    if (isFloat(g, v) || add->dst != v || add->a != v || st->dst != v || g->uses[v] != 2) return false;
    if (!isK(g, add->b) || kOf(g, add->b) < -128 || kOf(g, add->b) > 127) return false;
    if (st->a != ld->a || st->b != ld->b || st->imm != ld->imm || st->c != ld->c) return false;
    Mem m = memOf(g, ld);
    static const uint8_t ADD_IMM8[] = {0x83};
    memOp(g, 0, true, ADD_IMM8, 1, 0, m); /* /0 */
    byte(g, (int)(int8_t)kOf(g, add->b));
    return true;
}

static void genIns(Gen* g, const IrIns* ins) {
    switch (ins->op) {
    case IR_CONST:
        if (isK(g, ins->dst)) break;
        if (ins->c == 1) {
            movReloc(g, RAX, 1, ins->imm);
        } else if (inReg(g, ins->dst) && !isFloat(g, ins->dst)) {
            movImm(g, gprOf(g, ins->dst), ins->imm);
            break;
        } else {
            movImm(g, RAX, ins->imm);
        }
        storeBits(g, ins->dst, RAX);
        break;
    case IR_MOV:
        if (isFloat(g, ins->dst) && inReg(g, ins->dst)) {
            loadF(g, xmmOf(g, ins->dst), ins->a);
        } else if (!isFloat(g, ins->dst) && inReg(g, ins->dst)) {
            loadBits(g, gprOf(g, ins->dst), ins->a);
        } else {
            loadBits(g, RAX, ins->a);
            storeBits(g, ins->dst, RAX);
        }
        break;
    case IR_ADD: case IR_SUB: case IR_MUL: {
        if (leaFor(g, ins)) break;
        const uint8_t* op = ins->op == IR_ADD ? OP_ADD : ins->op == IR_SUB ? OP_SUB : OP_IMUL;
        int opLen = ins->op == IR_MUL ? 2 : 1;
        int d = inReg(g, ins->dst) && (!inReg(g, ins->b) || gprOf(g, ins->b) != gprOf(g, ins->dst)) ? gprOf(g, ins->dst) : RAX;
        loadBits(g, d, ins->a);
        aluWith(g, op, opLen, d, ins->b);
        if (d == RAX) storeBits(g, ins->dst, RAX);
        break;
    }
    case IR_DIV: case IR_MOD: {
        if (isK(g, ins->b) && kOf(g, ins->b) >= 1) {
            divideByConstant(g, ins);
            break;
        }
        /* floored: the quotient rounds down, the remainder takes the divisor's sign */
        loadBits(g, RAX, ins->a);
        loadBits(g, RCX, ins->b);
        rr(g, true, OP_TEST, 1, RCX, RCX);
        coldJump(g, CC_E, PANIC_DIVIDE, ins->line, -1, -1);
        /* cmp rcx, -1 */
        rex(g, true, 0, 0, RCX, false);
        byte(g, 0x83);
        byte(g, 0xF8 | RCX);
        byte(g, 0xFF);
        int normal = jccShort(g, CC_NE);
        if (ins->op == IR_DIV) { /* neg rax: wraps at the minimum */
            rex(g, true, 0, 0, RAX, false);
            byte(g, 0xF7);
            byte(g, 0xD8);
        } else {
            rr(g, false, OP_XOR, 1, RAX, RAX);
        }
        int done = jmpShort(g);
        land(g, normal);
        rex(g, true, 0, 0, 0, false); /* cqo */
        byte(g, 0x99);
        rex(g, true, 0, 0, RCX, false); /* idiv rcx */
        byte(g, 0xF7);
        byte(g, 0xF8 | RCX);
        /* if rdx != 0 and (rdx ^ rcx) < 0: adjust */
        rr(g, true, OP_TEST, 1, RDX, RDX);
        int exact = jccShort(g, CC_E);
        movRR(g, R8, RDX);
        rr(g, true, (const uint8_t[]){0x33}, 1, R8, RCX); /* xor r8, rcx */
        int sameSign = jccShort(g, CC_NS);
        if (ins->op == IR_DIV) { /* dec rax */
            rex(g, true, 0, 0, RAX, false);
            byte(g, 0xFF);
            byte(g, 0xC8);
        } else { /* add rdx, rcx */
            rr(g, true, OP_ADD, 1, RDX, RCX);
        }
        land(g, sameSign);
        land(g, exact);
        if (ins->op == IR_MOD) movRR(g, RAX, RDX);
        land(g, done);
        storeBits(g, ins->dst, RAX);
        break;
    }
    case IR_NEG:
        loadBits(g, RAX, ins->a);
        rex(g, true, 0, 0, RAX, false);
        byte(g, 0xF7);
        byte(g, 0xD8);
        storeBits(g, ins->dst, RAX);
        break;
    case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV: case IR_FMIN: case IR_FMAX: {
        static const int ops[] = {0x58, 0x5C, 0x59, 0x5E};
        int op = ins->op == IR_FMIN ? 0x5D : ins->op == IR_FMAX ? 0x5F : ops[ins->op - IR_FADD];
        if (qkUseAvx) { /* three operands: no copy of the first one */
            int x = inReg(g, ins->a) ? xmmOf(g, ins->a) : 0;
            if (x == 0) loadF(g, 0, ins->a);
            int d = inReg(g, ins->dst) ? xmmOf(g, ins->dst) : 0;
            vexF2(g, op, d, x, ins->b);
            if (d == 0) storeF(g, ins->dst, 0);
            break;
        }
        int d = inReg(g, ins->dst) && (!inReg(g, ins->b) || xmmOf(g, ins->b) != xmmOf(g, ins->dst)) ? xmmOf(g, ins->dst) : 0;
        loadF(g, d, ins->a);
        sseWith(g, 0xF2, op, d, ins->b);
        if (d == 0) storeF(g, ins->dst, 0);
        break;
    }
    case IR_FNEG:
        loadBits(g, RAX, ins->a);
        rex(g, true, 0, 0, RAX, false); /* btc rax, 63 */
        byte(g, 0x0F);
        byte(g, 0xBA);
        byte(g, 0xF8);
        byte(g, 63);
        storeBits(g, ins->dst, RAX);
        break;
    case IR_SQRT:
        sseWith(g, 0xF2, 0x51, 0, ins->a);
        storeF(g, ins->dst, 0);
        break;
    case IR_CMP: {
        static const int ccs[] = {CC_E, CC_NE, CC_L, CC_LE, CC_G, CC_GE};
        int ra = inReg(g, ins->a) ? gprOf(g, ins->a) : RAX;
        if (ra == RAX) loadBits(g, RAX, ins->a);
        aluWith(g, OP_CMP, 1, ra, ins->b);
        setcc(g, ccs[ins->cond], 0);
        movzxEaxAl(g);
        storeBits(g, ins->dst, RAX);
        break;
    }
    case IR_FCMP: {
        int c = ins->cond;
        int x = ins->a, y = ins->b;
        if (c == C_LT || c == C_LE) { /* a < b is b > a: false when either is NaN */
            x = ins->b;
            y = ins->a;
        }
        loadF(g, 0, x);
        sseWith(g, 0x66, 0x2E, 0, y); /* ucomisd */
        if (c == C_EQ) {
            setcc(g, CC_E, 0);
            setcc(g, CC_NP, 1);
            byte(g, 0x20); /* and al, cl */
            byte(g, 0xC8);
        } else if (c == C_NE) {
            setcc(g, CC_NE, 0);
            setcc(g, CC_P, 1);
            byte(g, 0x08); /* or al, cl */
            byte(g, 0xC8);
        } else {
            setcc(g, c == C_LT || c == C_GT ? CC_A : CC_AE, 0);
        }
        movzxEaxAl(g);
        storeBits(g, ins->dst, RAX);
        break;
    }
    case IR_NOT:
        loadBits(g, RAX, ins->a);
        rr(g, true, OP_TEST, 1, RAX, RAX);
        setcc(g, CC_E, 0);
        movzxEaxAl(g);
        storeBits(g, ins->dst, RAX);
        break;
    case IR_I2F: { /* straight into the destination register when it has one */
        int src = inReg(g, ins->a) ? gprOf(g, ins->a) : RAX;
        if (src == RAX) loadBits(g, RAX, ins->a);
        int x = inReg(g, ins->dst) ? xmmOf(g, ins->dst) : 0;
        sseRR(g, 0, false, 0x57, x, x); /* xorps x, x: cvtsi2sd keeps the upper half, a false dependency */
        sseRR(g, 0xF2, true, 0x2A, x, src); /* cvtsi2sd x, src */
        if (x == 0) storeF(g, ins->dst, 0);
        break;
    }
    case IR_F2I: {
        int x = inReg(g, ins->a) ? xmmOf(g, ins->a) : 0;
        if (x == 0) loadF(g, 0, ins->a);
        int d = inReg(g, ins->dst) ? gprOf(g, ins->dst) : RAX;
        sseRR(g, 0xF2, true, 0x2C, d, x); /* cvttsd2si d, x */
        if (d == RAX) storeBits(g, ins->dst, RAX);
        break;
    }
    case IR_ROUND: {
        int x = inReg(g, ins->a) ? xmmOf(g, ins->a) : 0;
        if (x == 0) loadF(g, 0, ins->a);
        int d = inReg(g, ins->dst) ? xmmOf(g, ins->dst) : 0;
        int mode = ins->imm == 1 ? 9 : ins->imm == 2 ? 10 : 8; /* floor, ceil, nearest; no precision exception */
        byte(g, 0x66); /* roundsd d, x, mode */
        rex(g, false, d, 0, x, false);
        byte(g, 0x0F);
        byte(g, 0x3A);
        byte(g, 0x0B);
        byte(g, 0xC0 | (d & 7) << 3 | (x & 7));
        byte(g, mode);
        if (d == 0) storeF(g, ins->dst, 0);
        break;
    }
    case IR_LOAD: case IR_STORE: {
        Mem m = memOf(g, ins);
        int v = ins->dst;
        bool load = ins->op == IR_LOAD;
        if (isFloat(g, v) && inReg(g, v)) { /* movsd straight between memory and the register */
            memOp(g, 0xF2, false, (const uint8_t[]){0x0F, (uint8_t)(load ? 0x10 : 0x11)}, 2, xmmOf(g, v), m);
        } else if (!isFloat(g, v) && inReg(g, v)) {
            memOp(g, 0, true, load ? OP_MOV_LOAD : OP_MOV_STORE, 1, gprOf(g, v), m);
        } else if (load) {
            memOp(g, 0, true, OP_MOV_LOAD, 1, RAX, m);
            storeBits(g, v, RAX);
        } else {
            loadBits(g, RAX, v);
            memOp(g, 0, true, OP_MOV_STORE, 1, RAX, m);
        }
        break;
    }
    case IR_BOUNDS: {
        int ra = inReg(g, ins->a) ? gprOf(g, ins->a) : RAX;
        if (ra == RAX) loadBits(g, RAX, ins->a);
        aluWith(g, OP_CMP, 1, ra, ins->b);
        coldJump(g, CC_AE, (int)ins->imm, ins->line, ins->a, ins->b);
        break;
    }
    case IR_LABEL:
        /* a loop's top, reached only by jumps: pad it to 16 bytes (the padding is never run) */
        if (g->loopTop[ins->imm] && g->afterJump)
            while (g->b->len % 16) byte(g, 0xCC);
        g->labelAt[ins->imm] = here(g);
        break;
    case IR_JMP: jmpLabel(g, (int)ins->imm); break;
    case IR_JZ: case IR_JNZ:
        if (inReg(g, ins->a) && !isFloat(g, ins->a)) {
            rr(g, true, OP_TEST, 1, gprOf(g, ins->a), gprOf(g, ins->a));
        } else {
            loadBits(g, RAX, ins->a);
            rr(g, true, OP_TEST, 1, RAX, RAX);
        }
        jccLabel(g, ins->op == IR_JZ ? CC_E : CC_NE, (int)ins->imm);
        break;
    case IR_CALL:
        for (int i = 0; i < ins->argCount; i++) {
            loadBits(g, RAX, ins->args[i]);
            rm(g, true, OP_MOV_STORE, 1, RAX, RSP, 8 * i);
        }
        byte(g, 0xE8);
        {
            Fixup f = {here(g), (int)ins->imm};
            QK_PUSH(g->callFixups, g->callCount, g->callCap, f);
        }
        i32(g, 0);
        for (int i = 0; i < ins->retCount; i++) {
            rm(g, true, OP_MOV_LOAD, 1, RAX, RSP, 8 * i);
            storeBits(g, ins->rets[i], RAX);
        }
        break;
    case IR_RTCALL: {
        const RtFn* f = &qkRuntime[ins->imm];
        static const int gprArgs[] = {RCX, RDX, R8, R9};
        for (int i = 0; i < ins->argCount; i++) {
            if (f->args[i] == 'F') loadF(g, i, ins->args[i]);
            else loadBits(g, gprArgs[i], ins->args[i]);
        }
        callRuntime(g, (RtId)ins->imm);
        if (ins->retCount) {
            if (f->ret == 'F') storeF(g, ins->rets[0], 0);
            else storeBits(g, ins->rets[0], RAX);
        }
        break;
    }
    case IR_RET:
        for (int i = 0; i < ins->argCount; i++) {
            loadBits(g, RAX, ins->args[i]);
            rm(g, true, OP_MOV_STORE, 1, RAX, RBP, 16 + 8 * i);
        }
        jmpLabel(g, g->epilogue);
        break;
    case IR_FUEL: {
        movReloc(g, RAX, 2, 0);
        rex(g, true, 0, 0, RAX, false); /* dec qword [rax] */
        byte(g, 0xFF);
        byte(g, 0x08);
        coldJump(g, CC_S, PANIC_FUEL, ins->line, -1, -1);
        break;
    }
    }
}

/* A comparison whose only reader is the jump right after it: one compare, one conditional jump. */
static bool fusedBranch(Gen* g, const IrIns* cmpIns, const IrIns* jump) {
    if ((cmpIns->op != IR_CMP && cmpIns->op != IR_FCMP) || (jump->op != IR_JZ && jump->op != IR_JNZ)) return false;
    if (jump->a != cmpIns->dst || g->uses[cmpIns->dst] != 1) return false;
    bool whenTrue = jump->op == IR_JNZ;
    int target = (int)jump->imm;
    int c = cmpIns->cond;
    if (cmpIns->op == IR_CMP) {
        static const int ccs[] = {CC_E, CC_NE, CC_L, CC_LE, CC_G, CC_GE};
        static const int negated[] = {CC_NE, CC_E, CC_GE, CC_G, CC_LE, CC_L};
        int ra = inReg(g, cmpIns->a) ? gprOf(g, cmpIns->a) : RAX;
        if (ra == RAX) loadBits(g, RAX, cmpIns->a);
        aluWith(g, OP_CMP, 1, ra, cmpIns->b);
        jccLabel(g, whenTrue ? ccs[c] : negated[c], target);
        return true;
    }
    int x = cmpIns->a, y = cmpIns->b;
    if (c == C_LT || c == C_LE) {
        x = cmpIns->b;
        y = cmpIns->a;
    }
    int xr = inReg(g, x) ? xmmOf(g, x) : 0;
    if (xr == 0) loadF(g, 0, x);
    sseWith(g, 0x66, 0x2E, xr, y); /* ucomisd */
    if (c == C_EQ || c == C_NE) {
        bool jumpIfEqual = (c == C_EQ) == whenTrue; /* ordered and equal */
        if (jumpIfEqual) {
            int skip = jccShort(g, CC_P);
            jccLabel(g, CC_E, target);
            land(g, skip);
        } else {
            jccLabel(g, CC_P, target);
            jccLabel(g, CC_NE, target);
        }
        return true;
    }
    bool strict = c == C_LT || c == C_GT;
    if (whenTrue) jccLabel(g, strict ? CC_A : CC_AE, target);
    else jccLabel(g, strict ? CC_BE : CC_B, target);
    return true;
}

/* ---- functions ----------------------------------------------------------------------------------- */

/* Linear scan: a register belongs to a virtual register only while it is alive (from its first to its last
 * mention, stretched over the loops it is alive across), so the inner loops of one function do not compete
 * for the same seven registers. When none is free, the lightest (by use, loops weighing more) goes to memory. */
static void allocate(Gen* g, const IrFn* f, int* slotsOut, int* gprUsed, int* xmmUsed) {
    int n = f->vregCount ? f->vregCount : 1;
    int* first = qkNew(sizeof(int) * (size_t)n);
    int* last = qkNew(sizeof(int) * (size_t)n);
    for (int v = 0; v < f->vregCount; v++) {
        first[v] = INT32_MAX;
        last[v] = -1;
    }
#define MENTION(v, k)                          \
    do {                                       \
        int v_ = (v);                          \
        if (v_ >= 0) {                         \
            if ((k) < first[v_]) first[v_] = (k); \
            if ((k) > last[v_]) last[v_] = (k);   \
        }                                      \
    } while (0)
    for (int i = 0; i < f->paramCount; i++) MENTION(f->params[i], 0);
    int* labelAt = qkNew(sizeof(int) * (size_t)(f->labelCount + 1));
    for (int k = 0; k < f->count; k++) {
        const IrIns* ins = &f->code[k];
        if (ins->op == IR_LABEL) {
            labelAt[ins->imm] = k;
            continue;
        }
        if (ins->op == IR_JMP) continue;
        MENTION(ins->dst, k);
        if (ins->op != IR_CONST) {
            MENTION(ins->a, k);
            MENTION(ins->b, k);
        }
        for (int i = 0; i < ins->argCount; i++) MENTION(ins->args[i], k);
        for (int i = 0; i < ins->retCount; i++) MENTION(ins->rets[i], k);
    }
#undef MENTION
    /* alive at a loop's top and read inside it: alive until the jump back */
    for (bool changed = true; changed;) {
        changed = false;
        for (int k = 0; k < f->count; k++) {
            const IrIns* ins = &f->code[k];
            if (ins->op != IR_JMP && ins->op != IR_JZ && ins->op != IR_JNZ) continue;
            int top = labelAt[ins->imm];
            if (top >= k) continue;
            for (int v = 0; v < f->vregCount; v++)
                if (first[v] < top && last[v] >= top && last[v] < k) {
                    last[v] = k;
                    changed = true;
                }
        }
    }
    /* by start */
    int* order = qkNew(sizeof(int) * (size_t)n);
    int count = 0;
    for (int v = 0; v < f->vregCount; v++) {
        g->home[v] = INT32_MIN;
        if (last[v] >= 0 && !(f->isConst && f->isConst[v]) && f->weight[v] > 0) order[count++] = v;
    }
    for (int i = 1; i < count; i++) { /* insertion sort: already nearly in order */
        int v = order[i], j = i - 1;
        while (j >= 0 && first[order[j]] > first[v]) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = v;
    }
    /* a value no call crosses takes a volatile register first (nothing to save), the others a callee-saved one */
    static const int regList[2][15] = {{R10, RBX, RSI, RDI, R12, R13, R14, R15},
                                       {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}};
    static const int volatileCount[2] = {1, 5};
    static const int poolSize[2] = {8, 15};
    bool* crosses = qkNew((size_t)n);
    for (int k = 0; k < f->count; k++) {
        if (f->code[k].op != IR_CALL && f->code[k].op != IR_RTCALL) continue;
        for (int v = 0; v < f->vregCount; v++)
            if (first[v] < k && last[v] >= k) crosses[v] = true;
    }
    int owner[2][15];
    for (int c = 0; c < 2; c++)
        for (int r = 0; r < 15; r++) owner[c][r] = -1;
    *gprUsed = *xmmUsed = 0;
    for (int i = 0; i < count; i++) {
        int v = order[i];
        int c = f->kinds[v] == 'F' ? 1 : 0;
        int from = crosses[v] ? volatileCount[c] : 0;
        int pick = -1;
        for (int r = 0; r < poolSize[c]; r++) {
            int o = owner[c][r];
            if (o >= 0 && last[o] < first[v]) owner[c][r] = o = -1; /* expired */
            if (o < 0 && pick < 0 && r >= from) pick = r;
        }
        if (pick < 0) { /* all taken: the lightest gives way, if lighter than this one */
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
        int reg = regList[c][pick];
        g->home[v] = c ? 16 + reg : reg;
        if (pick >= volatileCount[c]) {
            int saved = c ? reg - 6 + 1 : pick - volatileCount[c] + 1;
            if (c && saved > *xmmUsed) *xmmUsed = saved;
            if (!c && saved > *gprUsed) *gprUsed = saved;
        }
    }
    int slots = 0;
    for (int v = 0; v < f->vregCount; v++)
        if (g->home[v] == INT32_MIN) g->home[v] = -(++slots);
    *slotsOut = slots;
}

static void genFn(Gen* g, const IrFn* f) {
    g->fn = f;
    g->home = qkNew(sizeof(int) * (size_t)(f->vregCount ? f->vregCount : 1));
    static const int gprs[] = {RBX, RSI, RDI, R12, R13, R14, R15};
    int slots, gprUsed, xmmUsed;
    allocate(g, f, &slots, &gprUsed, &xmmUsed);
    g->pushed = gprUsed;
    g->xmmCount = xmmUsed;
    /* frame: pushed GPRs, then slots, then saved XMMs, then the outgoing area at rsp */
    g->slotBase = 8 * g->pushed;
    int xmmArea = 16 * g->xmmCount;
    int out = 8 * (f->maxOut > 4 ? f->maxOut : 4);
    int frame = 8 * slots + xmmArea + out + 16;
    while ((8 * g->pushed + frame) % 16) frame += 8;
    g->xmmBase = g->slotBase + 8 * slots + 16; /* below the slots, 16 aligned enough for movups */
    g->labelAt = qkNew(sizeof(int) * (size_t)(f->labelCount + 1));
    g->epilogue = f->labelCount;
    g->fixupCount = 0;

    /* prologue */
    byte(g, 0x55); /* push rbp */
    movRR(g, RBP, RSP);
    for (int i = 0; i < g->pushed; i++) {
        rex(g, false, 0, 0, gprs[i], false);
        byte(g, 0x50 | (gprs[i] & 7)); /* push */
    }
    rex(g, true, 0, 0, RSP, false); /* sub rsp, frame */
    byte(g, 0x81);
    byte(g, 0xEC);
    i32(g, frame);
    for (int i = 0; i < g->xmmCount; i++) { /* movups [rbp - xmmBase - 16i], xmm(6+i) */
        int x = 6 + i;
        rex(g, false, x, 0, RBP, false);
        byte(g, 0x0F);
        byte(g, 0x11);
        byte(g, 0x80 | (x & 7) << 3 | RBP);
        i32(g, -(g->xmmBase + 16 * i));
    }
    /* parameters: from the caller's outgoing area to their homes */
    for (int i = 0; i < f->paramCount; i++) {
        rm(g, true, OP_MOV_LOAD, 1, RAX, RBP, 16 + 8 * i);
        storeBits(g, f->params[i], RAX);
    }
    g->uses = qkNew(sizeof(int) * (size_t)(f->vregCount ? f->vregCount : 1));
    g->loopTop = qkNew((size_t)f->labelCount + 1);
    {
        int* placed = qkNew(sizeof(int) * (size_t)(f->labelCount + 1));
        for (int k = 0; k < f->count; k++) {
            const IrIns* ins = &f->code[k];
            if (ins->op == IR_LABEL) placed[ins->imm] = k + 1;
            else if ((ins->op == IR_JMP || ins->op == IR_JZ || ins->op == IR_JNZ) && placed[ins->imm]) g->loopTop[ins->imm] = 1;
        }
    }
    for (int k = 0; k < f->count; k++) {
        const IrIns* ins = &f->code[k];
        if (ins->op == IR_LABEL || ins->op == IR_JMP) continue;
        if (ins->op == IR_STORE && ins->dst >= 0) g->uses[ins->dst]++;
        if (ins->op != IR_CONST) {
            if (ins->a >= 0) g->uses[ins->a]++;
            if (ins->b >= 0) g->uses[ins->b]++;
        }
        for (int i = 0; i < ins->argCount; i++) g->uses[ins->args[i]]++;
    }
    g->afterJump = false;
    for (int k = 0; k < f->count; k++) {
        if (k + 1 < f->count && fusedBranch(g, &f->code[k], &f->code[k + 1])) {
            k++;
            g->afterJump = false;
            continue;
        }
        if (k + 2 < f->count && addToMemory(g, &f->code[k], &f->code[k + 1], &f->code[k + 2])) {
            k += 2;
            g->afterJump = false;
            continue;
        }
        genIns(g, &f->code[k]);
        int op = f->code[k].op;
        if (op != IR_LABEL) g->afterJump = op == IR_JMP || op == IR_RET;
    }

    /* epilogue */
    g->labelAt[g->epilogue] = here(g);
    for (int i = 0; i < g->xmmCount; i++) {
        int x = 6 + i;
        rex(g, false, x, 0, RBP, false);
        byte(g, 0x0F);
        byte(g, 0x10);
        byte(g, 0x80 | (x & 7) << 3 | RBP);
        i32(g, -(g->xmmBase + 16 * i));
    }
    rex(g, true, RSP, 0, RBP, false); /* lea rsp, [rbp - 8 * pushed] */
    byte(g, 0x8D);
    byte(g, 0x80 | (RSP & 7) << 3 | RBP);
    i32(g, -8 * g->pushed);
    for (int i = g->pushed - 1; i >= 0; i--) {
        rex(g, false, 0, 0, gprs[i], false);
        byte(g, 0x58 | (gprs[i] & 7)); /* pop */
    }
    byte(g, 0x5D); /* pop rbp */
    byte(g, 0xC3); /* ret */
    emitCold(g);

    for (int i = 0; i < g->fixupCount; i++) {
        int32_t rel = g->labelAt[g->fixups[i].target] - (g->fixups[i].at + 4);
        memcpy(g->b->data + g->fixups[i].at, &rel, 4);
    }
}

bool qkGenX64(const IrProgram* p, MachineCode* out) {
    memset(out, 0, sizeof *out);
    Gen g = {0};
    g.mc = out;
    g.b = &out->code;
    g.prog = p;
    g.fnAt = qkNew(sizeof(int) * (size_t)(p->count ? p->count : 1));
    for (int i = 0; i < p->count; i++) {
        while (g.b->len % 16) byte(&g, 0xCC);
        g.fnAt[i] = here(&g);
        genFn(&g, &p->fns[i]);
    }
    /* the entry: void entry(void), called from C */
    while (g.b->len % 16) byte(&g, 0xCC);
    out->entry = here(&g);
    rex(&g, true, 0, 0, RSP, false); /* sub rsp, 40 */
    byte(&g, 0x83);
    byte(&g, 0xEC);
    byte(&g, 40);
    byte(&g, 0xE8);
    Fixup f = {here(&g), p->mainIndex};
    QK_PUSH(g.callFixups, g.callCount, g.callCap, f);
    i32(&g, 0);
    rex(&g, true, 0, 0, RSP, false); /* add rsp, 40 */
    byte(&g, 0x83);
    byte(&g, 0xC4);
    byte(&g, 40);
    byte(&g, 0xC3);
    for (int i = 0; i < g.callCount; i++) {
        int32_t rel = g.fnAt[g.callFixups[i].target] - (g.callFixups[i].at + 4);
        memcpy(g.b->data + g.callFixups[i].at, &rel, 4);
    }
    return true;
}
