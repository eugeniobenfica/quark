/* Quark — the x86-64 backend: Windows x64, or System V (Linux, macOS) for the calls into C. No assembler, no
 * linker: instructions are encoded here, byte by byte.
 *
 * Registers: the most used virtual registers (loops weigh 8x per level) own a callee-saved register for the
 * whole function — rbx, rsi, rdi, r12-r15 for ints, xmm6-xmm15 for floats (System V: rbx, r12-r15, and
 * xmm8-xmm15, saved around calls into C, which may change every xmm); the rest live in the frame.
 * rax, rcx, rdx, r8-r11 and xmm0-xmm5 are scratch.
 *
 * Calls between Quark functions: arguments go to [rsp + 8i] of the caller (its outgoing area), the callee
 * reads them at [rbp + 16 + 8i] and writes its results there before returning; the caller reads them back.
 * Calls to the runtime and the host follow the platform: Windows x64 (rcx, rdx, r8, r9 / xmm0-3 by position,
 * 32 bytes of shadow space), or System V (rdi, rsi, rdx, rcx, r8, r9 for ints and xmm0-7 for floats, each
 * counted on its own). */
#include "qk.h"

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
    int inl;     /* the inlined calls it is in */
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
    bool sysv;        /* System V calls into C */
    int xmmFirst;     /* the first callee-saved xmm: 6 (Windows), 8 (System V) */
    int* first;       /* each virtual register's first and last mention */
    int* last;
    int at;           /* the instruction being generated */
    int spillBase;    /* System V: frame slots where live xmm registers wait out a call into C */
    int fnIndex;      /* the function being generated */
    int inl;          /* the instruction's inlined calls */
} Gen;

int qkAbi =
#ifdef _WIN32
    QK_ABI_WIN64;
#else
    QK_ABI_SYSV;
#endif

/* The processor and system the code is made for: the one quark runs on, unless quark object --target says */
int qkTargetArch =
#if defined(__aarch64__) || defined(_M_ARM64)
    QK_ARCH_ARM64;
#else
    QK_ARCH_X64;
#endif
int qkTargetOs =
#if defined(_WIN32)
    QK_OS_WINDOWS;
#elif defined(__APPLE__)
    QK_OS_MACOS;
#else
    QK_OS_LINUX;
#endif

bool qkGenCode(const IrProgram* p, MachineCode* out) {
    if (qkTargetArch == QK_ARCH_ARM64) return qkGenArm64(p, out);
    return qkGenX64(p, out);
}

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
                     OP_IMUL[] = {0x0F, 0xAF}, OP_CMP[] = {0x3B}, OP_TEST[] = {0x85}, OP_XOR[] = {0x33},
                     OP_AND[] = {0x23}, OP_OR[] = {0x0B};

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
    if (qkObjectMode) { /* mov reg, [rip + disp32]: the address from the table (the object file's relocation) */
        rex(g, true, reg, 0, 0, false);
        byte(g, 0x8B);
        byte(g, 0x05 | (reg & 7) << 3);
        Reloc r = {here(g), kind, target};
        QK_PUSH(g->mc->relocs, g->mc->relocCount, g->mc->relocCap, r);
        i32(g, 0);
        return;
    }
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
        int digit = op[0] == 0x03 ? 0 : op[0] == 0x2B ? 5 : op[0] == 0x3B ? 7 : op[0] == 0x33 ? 6 : op[0] == 0x23 ? 4 : op[0] == 0x0B ? 1 : -1;
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

/* The panic's frame, for its stack of calls: qkPanicFrame = rbp (through r11, which nothing here holds). */
static void markPanicFrame(Gen* g) {
    movReloc(g, R11, RELOC_PANIC_FRAME, 0);
    rm(g, true, OP_MOV_STORE, 1, RBP, R11, 0);
}

/* panic(code, line, a, b) — rax holds a, rdx holds b, set by the caller; line carries the inlined calls above
 * its 32 bits */
static void panic(Gen* g, int code, int64_t line) {
    markPanicFrame(g);
    if (g->sysv) { /* rdi, rsi, rdx, rcx */
        movRR(g, RCX, RDX);
        movRR(g, RDX, RAX);
        movImm(g, RDI, code);
        movImm(g, RSI, line);
    } else {
        movRR(g, R8, RAX);
        movRR(g, R9, RDX);
        movImm(g, RCX, code);
        movImm(g, RDX, line);
    }
    callRuntime(g, RT_PANIC);
}

/* System V: C may change every xmm, so the callee-saved ones that hold a value still needed after the call
 * wait in the frame. save: before the call (true), after it (false). */
static void spillAcrossC(Gen* g, bool save) {
    if (!g->sysv) return;
    for (int v = 0; v < g->fn->vregCount; v++) {
        if (g->home[v] < 16 + g->xmmFirst || g->first[v] >= g->at || g->last[v] <= g->at) continue;
        int x = g->home[v] - 16;
        int32_t disp = -(g->slotBase + 8 * (g->spillBase + x - g->xmmFirst + 1));
        sseRM(g, 0xF2, false, save ? 0x11 : 0x10, x, RBP, disp); /* movsd */
    }
}

/* The arguments of a call into C, from their homes to where the platform wants them. kinds: 'I' / 'F' per
 * argument. hidden: System V, a pointer to room for the results goes first (in rdi). */
static void cArguments(Gen* g, const int* args, int count, const char* kinds, int hiddenBuffer) {
    static const int winGpr[] = {RCX, RDX, R8, R9};
    static const int sysvGpr[] = {RDI, RSI, RDX, RCX, R8, R9};
    if (!g->sysv) {
        int hidden = hiddenBuffer >= 0 ? 1 : 0;
        for (int i = 0; i < count; i++) { /* the stack ones first: they go through rax */
            int pos = i + hidden;
            if (pos < 4) continue;
            loadBits(g, RAX, args[i]);
            rm(g, true, OP_MOV_STORE, 1, RAX, RSP, 8 * pos);
        }
        for (int i = 0; i < count; i++) {
            int pos = i + hidden;
            if (pos >= 4) continue;
            if (kinds[i] == 'F') loadF(g, pos, args[i]);
            else loadBits(g, winGpr[pos], args[i]);
        }
        if (hidden) { /* lea rcx, [rsp + buffer] */
            static const uint8_t LEA[] = {0x8D};
            rm(g, true, LEA, 1, RCX, RSP, hiddenBuffer);
        }
        return;
    }
    int ints = hiddenBuffer >= 0 ? 1 : 0, floats = 0, stack = 0;
    for (int i = 0; i < count; i++) { /* past the registers: on the stack, in order */
        bool f = kinds[i] == 'F';
        if (f ? floats++ < 8 : ints++ < 6) continue;
        loadBits(g, RAX, args[i]);
        rm(g, true, OP_MOV_STORE, 1, RAX, RSP, 8 * stack++);
    }
    ints = hiddenBuffer >= 0 ? 1 : 0;
    floats = 0;
    for (int i = 0; i < count; i++) {
        if (kinds[i] == 'F') {
            if (floats < 8) loadF(g, floats, args[i]);
            floats++;
        } else {
            if (ints < 6) loadBits(g, sysvGpr[ints], args[i]);
            ints++;
        }
    }
    if (hiddenBuffer >= 0) { /* lea rdi, [rsp + buffer] */
        static const uint8_t LEA[] = {0x8D};
        rm(g, true, LEA, 1, RDI, RSP, hiddenBuffer);
    }
}

static void coldJump(Gen* g, int cc, int code, int line, int a, int b) {
    byte(g, 0x0F);
    byte(g, 0x80 | cc);
    Cold c = {here(g), code, line, a, b, g->inl};
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
        panic(g, c->code, c->line | (int64_t)c->inl << 32);
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

/* ---- SIMD (release.c's vector loops) --------------------------------------------------------------- *
 * AVX2: four lanes in ymm registers (VEX.256); else two in xmm (VEX.128 with AVX, SSE2 / SSE4.2 without). */

static void memTail(Gen* g, int reg, Mem m) {
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

/* three-byte VEX: L 0 (128) / 1 (256); pp 1: 66, 3: F2; map 1: 0F, 2: 0F38, 3: 0F3A */
static void vexPrefix(Gen* g, int L, int pp, int map, int w, int reg, int vvvv, int index, int base) {
    byte(g, 0xC4);
    byte(g, (((reg >> 3) & 1) ^ 1) << 7 | (((index >> 3) & 1) ^ 1) << 6 | (((base >> 3) & 1) ^ 1) << 5 | map);
    byte(g, w << 7 | ((~vvvv) & 15) << 3 | L << 2 | pp);
}
static void vexRR(Gen* g, int L, int pp, int map, int w, int op, int reg, int vvvv, int rmReg) {
    vexPrefix(g, L, pp, map, w, reg, vvvv, 0, rmReg);
    byte(g, op);
    byte(g, 0xC0 | (reg & 7) << 3 | (rmReg & 7));
}
static void vexMem(Gen* g, int L, int pp, int map, int w, int op, int reg, int vvvv, Mem m) {
    vexPrefix(g, L, pp, map, w, reg, vvvv, m.index >= 0 ? m.index : 0, m.base);
    byte(g, op);
    memTail(g, reg, m);
}
/* legacy SSE, 66 prefix: op reg, rm (map 1: 0F, 2: 0F 38) */
static void sse66(Gen* g, int map, int op, int reg, int rmReg) {
    byte(g, 0x66);
    rex(g, false, reg, 0, rmReg, false);
    byte(g, 0x0F);
    if (map == 2) byte(g, 0x38);
    byte(g, op);
    byte(g, 0xC0 | (reg & 7) << 3 | (rmReg & 7));
}

static int vecReg(Gen* g, int v) {
    if (!inReg(g, v)) { /* release.c keeps few enough of them to stay in registers */
        fprintf(stderr, "quark: internal error: a vector register has no register\n");
        abort();
    }
    return xmmOf(g, v);
}

static void vmov(Gen* g, int d, int s) { /* the whole register */
    if (d == s) return;
    if (qkUseAvx) vexRR(g, qkSimdLanes == 4, 1, 1, 0, 0x28, d, 0, s); /* vmovapd */
    else sse66(g, 1, 0x28, d, s);                                     /* movapd */
}

/* d = a op b, lane by lane (66-prefixed op in map 0F or 0F38) */
static void vbin(Gen* g, int map, int op, int d, int a, int b, bool commutes) {
    if (qkUseAvx) {
        vexRR(g, qkSimdLanes == 4, 1, map, 0, op, d, a, b);
        return;
    }
    if (d == b && d != a) {
        if (commutes) {
            sse66(g, map, op, d, a);
            return;
        }
        vmov(g, 0, a);
        sse66(g, map, op, 0, b);
        vmov(g, d, 0);
        return;
    }
    vmov(g, d, a);
    sse66(g, map, op, d, b);
}

static void genVector(Gen* g, const IrIns* ins) {
    int L = qkSimdLanes == 4;
    switch (ins->op) {
    case IR_VLOAD: case IR_VSTORE: { /* movupd */
        Mem m = memOf(g, ins);
        int x = vecReg(g, ins->dst);
        int op = ins->op == IR_VLOAD ? 0x10 : 0x11;
        if (qkUseAvx) vexMem(g, L, 1, 1, 0, op, x, 0, m);
        else memOp(g, 0x66, false, (const uint8_t[]){0x0F, (uint8_t)op}, 2, x, m);
        break;
    }
    case IR_VSPLAT: {
        int d = vecReg(g, ins->dst);
        int s;
        if (isFloat(g, ins->a) && inReg(g, ins->a)) {
            s = xmmOf(g, ins->a);
        } else { /* through rax: movq d, rax */
            loadBits(g, RAX, ins->a);
            if (qkUseAvx) vexRR(g, 0, 1, 1, 1, 0x6E, d, 0, RAX);
            else sseRR(g, 0x66, true, 0x6E, d, RAX);
            s = d;
        }
        if (L) vexRR(g, 1, 1, 2, 0, isFloat(g, ins->a) ? 0x19 : 0x59, d, 0, s); /* vbroadcastsd / vpbroadcastq */
        else if (qkUseAvx) vexRR(g, 0, 1, 1, 0, 0x14, d, s, s);                 /* vunpcklpd d, s, s */
        else {
            vmov(g, d, s);
            sse66(g, 1, 0x14, d, d); /* unpcklpd */
        }
        break;
    }
    case IR_VLANE: {
        int s = vecReg(g, ins->a), l = ins->c;
        if (L && l >= 2) { /* vextractf128 xmm0, ymm, 1 */
            vexRR(g, 1, 1, 3, 0, 0x19, s, 0, 0);
            byte(g, 1);
            s = 0;
            l -= 2;
        }
        bool fl = isFloat(g, ins->dst);
        int t = fl && inReg(g, ins->dst) ? xmmOf(g, ins->dst) : 0;
        if (l == 0) {
            if (fl && t != s) {
                if (qkUseAvx) vexRR(g, 0, 1, 1, 0, 0x28, t, 0, s);
                else sse66(g, 1, 0x28, t, s);
            }
            if (!fl) t = s;
        } else if (qkUseAvx) {
            vexRR(g, 0, 1, 1, 0, 0x15, t, s, s); /* vunpckhpd t, s, s */
        } else {
            if (t != s) sse66(g, 1, 0x28, t, s);
            sse66(g, 1, 0x15, t, t); /* unpckhpd */
        }
        if (fl) {
            if (!inReg(g, ins->dst)) {
                if (qkUseAvx) vexMem(g, 0, 3, 1, 0, 0x11, 0, 0, (Mem){RBP, -1, 1, slotDisp(g, ins->dst)}); /* vmovsd [slot], xmm0 */
                else storeF(g, ins->dst, 0);
            }
        } else {
            if (qkUseAvx) vexRR(g, 0, 1, 1, 1, 0x7E, t, 0, RAX); /* vmovq rax, xmm */
            else sseRR(g, 0x66, true, 0x7E, t, RAX);
            storeBits(g, ins->dst, RAX);
        }
        break;
    }
    case IR_VEND:
        if (L) { /* vzeroupper: no penalty for the SSE code after */
            byte(g, 0xC5);
            byte(g, 0xF8);
            byte(g, 0x77);
        }
        break;
    case IR_VOP: {
        int d = vecReg(g, ins->dst), a = vecReg(g, ins->a), b = ins->b >= 0 ? vecReg(g, ins->b) : 0;
        switch (ins->imm) {
        case IR_FADD: vbin(g, 1, 0x58, d, a, b, true); break;
        case IR_FSUB: vbin(g, 1, 0x5C, d, a, b, false); break;
        case IR_FMUL: vbin(g, 1, 0x59, d, a, b, true); break;
        case IR_FDIV: vbin(g, 1, 0x5E, d, a, b, false); break;
        case IR_FMIN: vbin(g, 1, 0x5D, d, a, b, false); break; /* as minsd: b when unordered */
        case IR_FMAX: vbin(g, 1, 0x5F, d, a, b, false); break;
        case IR_ADD: vbin(g, 1, 0xD4, d, a, b, true); break;   /* paddq */
        case IR_SUB: vbin(g, 1, 0xFB, d, a, b, false); break;  /* psubq */
        case IR_AND: vbin(g, 1, 0xDB, d, a, b, true); break;   /* pand */
        case IR_OR: vbin(g, 1, 0xEB, d, a, b, true); break;    /* por */
        case IR_XOR: vbin(g, 1, 0xEF, d, a, b, true); break;   /* pxor */
        case IR_NOT: vbin(g, 1, 0xDF, d, b, a, false); break;  /* pandn: ~b & a */
        case IR_SQRT:
            if (qkUseAvx) vexRR(g, L, 1, 1, 0, 0x51, d, 0, a);
            else sse66(g, 1, 0x51, d, a);
            break;
        case IR_SHL: /* psllq d, imm */
            if (qkUseAvx) vexRR(g, L, 1, 1, 0, 0x73, 6, d, a);
            else {
                vmov(g, d, a);
                sse66(g, 1, 0x73, 6, d);
            }
            byte(g, ins->c & 63);
            break;
        case IR_FCMP: { /* cmppd: eq 0, lt 1, le 2, neq (true when unordered) 4 */
            static const int pred[] = {0, 4, 1, 2, 1, 2};
            int x = a, y = b;
            if (ins->cond == C_GT || ins->cond == C_GE) x = b, y = a;
            vbin(g, 1, 0xC2, d, x, y, false);
            byte(g, pred[ins->cond]);
            break;
        }
        case IR_CMP: { /* pcmpeqq / pcmpgtq, then negated with all ones for != <= >= */
            int c = ins->cond;
            bool gt = c != C_EQ && c != C_NE;
            int x = a, y = b;
            if (c == C_LT || c == C_GE) x = b, y = a;
            vbin(g, 2, gt ? 0x37 : 0x29, d, x, y, !gt);
            if (c == C_NE || c == C_LE || c == C_GE) {
                vbin(g, 1, 0x76, 0, 0, 0, true); /* pcmpeqd xmm0, xmm0: all ones */
                vbin(g, 1, 0xEF, d, d, 0, true);
            }
            break;
        }
        }
        break;
    }
    }
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
    if (ins->cond & DIV_NONNEG) { /* n >= 0: truncated is floored, nothing to fix */
        if (!quotient) {
            if (fits32(d)) {
                rex(g, true, RDX, 0, RDX, false); /* imul rdx, rdx, d */
                byte(g, 0x69);
                byte(g, 0xC0 | (RDX & 7) << 3 | (RDX & 7));
                i32(g, (int32_t)d);
            } else {
                movImm(g, R11, d);
                rr(g, true, OP_IMUL, 2, RDX, R11);
            }
            movRR(g, RAX, R8);
            rr(g, true, OP_SUB, 1, RAX, RDX);
            storeBits(g, ins->dst, RAX);
            return;
        }
        storeBits(g, ins->dst, RDX);
        return;
    }
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
    if (ld->op != IR_LOAD || add->op != IR_ADD || st->op != IR_STORE || add->cond) return false;
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
    g->inl = ins->inl;
    switch (ins->op) {
    case IR_CONST:
        if (isK(g, ins->dst)) break;
        if (ins->c == 1) {
            movReloc(g, RAX, 1, ins->imm);
        } else if (ins->c == 2) { /* lea rax, [rip + function]: a function kept as a value */
            byte(g, 0x48);
            byte(g, 0x8D);
            byte(g, 0x05);
            Fixup fx = {here(g), (int)ins->imm};
            QK_PUSH(g->callFixups, g->callCount, g->callCap, fx);
            i32(g, 0);
        } else if (inReg(g, ins->dst) && !isFloat(g, ins->dst)) {
            movImm(g, gprOf(g, ins->dst), ins->imm);
            break;
        } else {
            movImm(g, RAX, ins->imm);
        }
        storeBits(g, ins->dst, RAX);
        break;
    case IR_MOV:
        if (g->fn->kinds[ins->dst] == 'V') {
            vmov(g, vecReg(g, ins->dst), vecReg(g, ins->a));
        } else if (isFloat(g, ins->dst) && inReg(g, ins->dst)) {
            loadF(g, xmmOf(g, ins->dst), ins->a);
        } else if (!isFloat(g, ins->dst) && inReg(g, ins->dst)) {
            loadBits(g, gprOf(g, ins->dst), ins->a);
        } else {
            loadBits(g, RAX, ins->a);
            storeBits(g, ins->dst, RAX);
        }
        break;
    case IR_ADD: case IR_SUB: case IR_MUL: {
        if (!ins->cond && leaFor(g, ins)) break; /* lea sets no flags: a checked one is a real add */
        const uint8_t* op = ins->op == IR_ADD ? OP_ADD : ins->op == IR_SUB ? OP_SUB : OP_IMUL;
        int opLen = ins->op == IR_MUL ? 2 : 1;
        int d = inReg(g, ins->dst) && (!inReg(g, ins->b) || gprOf(g, ins->b) != gprOf(g, ins->dst)) ? gprOf(g, ins->dst) : RAX;
        loadBits(g, d, ins->a);
        aluWith(g, op, opLen, d, ins->b);
        if (ins->cond) coldJump(g, CC_O, PANIC_OVERFLOW, ins->line, -1, -1); /* checked: past the limits */
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
        if (ins->cond & DIV_NONNEG) { /* a >= 0, b >= 1: xor edx, edx; div rcx */
            rr(g, false, OP_XOR, 1, RDX, RDX);
            rex(g, true, 0, 0, RCX, false);
            byte(g, 0xF7);
            byte(g, 0xF0 | RCX);
            storeBits(g, ins->dst, ins->op == IR_DIV ? RAX : RDX);
            break;
        }
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
    case IR_AND: case IR_OR: case IR_XOR: {
        const uint8_t* op = ins->op == IR_AND ? OP_AND : ins->op == IR_OR ? OP_OR : OP_XOR;
        int d = inReg(g, ins->dst) && (!inReg(g, ins->b) || gprOf(g, ins->b) != gprOf(g, ins->dst)) ? gprOf(g, ins->dst) : RAX;
        loadBits(g, d, ins->a);
        aluWith(g, op, 1, d, ins->b);
        if (d == RAX) storeBits(g, ins->dst, RAX);
        break;
    }
    case IR_SHL: case IR_SAR: { /* shl / sar rax, cl (or an immediate); the count modulo 64, as the processor takes it */
        int digit = ins->op == IR_SHL ? 4 : 7;
        loadBits(g, RAX, ins->a);
        if (isK(g, ins->b)) {
            rex(g, true, 0, 0, RAX, false);
            byte(g, 0xC1);
            byte(g, 0xC0 | digit << 3 | RAX);
            byte(g, (int)(kOf(g, ins->b) & 63));
        } else {
            loadBits(g, RCX, ins->b);
            rex(g, true, 0, 0, RAX, false);
            byte(g, 0xD3);
            byte(g, 0xC0 | digit << 3 | RAX);
        }
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
        if (ins->cond) { /* too big or NaN give 0x8000000000000000, the one value d - 1 overflows on */
            rex(g, true, 0, 0, d, false); /* cmp d, 1 */
            byte(g, 0x83);
            byte(g, 0xF8 | (d & 7));
            byte(g, 1);
            coldJump(g, CC_O, PANIC_TOO_BIG, ins->line, -1, -1);
        }
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
        if (ins->a >= 0) { /* call r11: a function kept as a value */
            loadBits(g, R11, ins->a);
            byte(g, 0x41);
            byte(g, 0xFF);
            byte(g, 0xD3);
        } else {
            byte(g, 0xE8);
            Fixup f = {here(g), (int)ins->imm};
            QK_PUSH(g->callFixups, g->callCount, g->callCap, f);
            i32(g, 0);
        }
        {
            CallSite s = {here(g), ins->line, g->fnIndex, (int32_t)ins->imm, ins->inl};
            QK_PUSH(g->mc->sites, g->mc->siteCount, g->mc->siteCap, s);
        }
        for (int i = 0; i < ins->retCount; i++) {
            rm(g, true, OP_MOV_LOAD, 1, RAX, RSP, 8 * i);
            storeBits(g, ins->rets[i], RAX);
        }
        break;
    case IR_RTCALL: {
        const RtFn* f = &qkRuntime[ins->imm];
        if (ins->imm == RT_FAIL) markPanicFrame(g);
        spillAcrossC(g, true);
        cArguments(g, ins->args, ins->argCount, f->args, -1);
        callRuntime(g, (RtId)ins->imm);
        if (ins->retCount) {
            if (f->ret == 'F') storeF(g, ins->rets[0], 0);
            else storeBits(g, ins->rets[0], RAX);
        }
        spillAcrossC(g, false);
        break;
    }
    case IR_HOSTCALL: {
        /* Windows x64: one result in rax / xmm0, several through a hidden first argument pointing at room after
         * the arguments. System V: up to two slots in rax, rdx / xmm0, xmm1 (each in the next of its kind),
         * more through the hidden pointer. */
        const IrExtern* x = &g->prog->externs[ins->imm];
        int hidden = ins->retCount > (g->sysv ? 2 : 1) ? 1 : 0;
        int positions = ins->argCount + hidden;
        int buffer = 8 * (positions > 4 ? positions : 4);
        spillAcrossC(g, true);
        cArguments(g, ins->args, ins->argCount, x->argKinds, hidden ? buffer : -1);
        movReloc(g, RAX, RELOC_HOST, ins->imm);
        byte(g, 0xFF); /* call rax */
        byte(g, 0xD0);
        if (hidden) {
            for (int i = 0; i < ins->retCount; i++) {
                rm(g, true, OP_MOV_LOAD, 1, RCX, RSP, buffer + 8 * i);
                storeBits(g, ins->rets[i], RCX);
            }
        } else if (g->sysv) {
            int reg[2], ints = 0, floats = 0;
            for (int i = 0; i < ins->retCount; i++) reg[i] = x->retKinds[i] == 'F' ? floats++ : ints++ ? RDX : RAX;
            for (int i = ins->retCount - 1; i >= 0; i--) { /* the second first: xmm1 may be the first one's home */
                if (x->retKinds[i] == 'F') storeF(g, ins->rets[i], reg[i]);
                else storeBits(g, ins->rets[i], reg[i]);
            }
        } else if (ins->retCount) {
            if (x->retKinds[0] == 'F') storeF(g, ins->rets[0], 0);
            else storeBits(g, ins->rets[0], RAX);
        }
        spillAcrossC(g, false);
        break;
    }
    case IR_RET:
        for (int i = 0; i < ins->argCount; i++) {
            loadBits(g, RAX, ins->args[i]);
            rm(g, true, OP_MOV_STORE, 1, RAX, RBP, 16 + 8 * i);
        }
        jmpLabel(g, g->epilogue);
        break;
    case IR_VLOAD: case IR_VSTORE: case IR_VSPLAT: case IR_VOP: case IR_VLANE: case IR_VEND:
        genVector(g, ins);
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
    /* a value no call crosses takes a volatile register first (nothing to save), the others a callee-saved one;
     * System V: rsi and rdi carry C's arguments, and xmm6-7 too, so they hold nothing */
    static const int winList[2][15] = {{R10, RBX, RSI, RDI, R12, R13, R14, R15},
                                       {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}};
    static const int sysvList[2][15] = {{R10, RBX, R12, R13, R14, R15}, {1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15}};
    const int(*regList)[15] = g->sysv ? sysvList : winList;
    static const int volatileCount[2] = {1, 5};
    const int poolSize[2] = {g->sysv ? 6 : 8, g->sysv ? 13 : 15};
    bool* crosses = qkNew((size_t)n);
    for (int k = 0; k < f->count; k++) {
        if (f->code[k].op != IR_CALL && f->code[k].op != IR_RTCALL && f->code[k].op != IR_HOSTCALL) continue;
        for (int v = 0; v < f->vregCount; v++)
            if (first[v] < k && last[v] >= k) crosses[v] = true;
    }
    int owner[2][15];
    for (int c = 0; c < 2; c++)
        for (int r = 0; r < 15; r++) owner[c][r] = -1;
    *gprUsed = *xmmUsed = 0;
    for (int i = 0; i < count; i++) {
        int v = order[i];
        int c = f->kinds[v] == 'F' || f->kinds[v] == 'V' ? 1 : 0;
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
            int saved = c ? reg - g->xmmFirst + 1 : pick - volatileCount[c] + 1;
            if (c && saved > *xmmUsed) *xmmUsed = saved;
            if (!c && saved > *gprUsed) *gprUsed = saved;
        }
    }
    int slots = 0;
    for (int v = 0; v < f->vregCount; v++)
        if (g->home[v] == INT32_MIN) g->home[v] = -(++slots);
    g->spillBase = slots;
    if (g->sysv) slots += 8; /* xmm8-15 across calls into C */
    *slotsOut = slots;
    g->first = first;
    g->last = last;
}

static void genFn(Gen* g, const IrFn* f) {
    g->fn = f;
    g->home = qkNew(sizeof(int) * (size_t)(f->vregCount ? f->vregCount : 1));
    static const int winGprs[] = {RBX, RSI, RDI, R12, R13, R14, R15};
    static const int sysvGprs[] = {RBX, R12, R13, R14, R15};
    const int* gprs = g->sysv ? sysvGprs : winGprs;
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
    bool calls = false;
    for (int k = 0; k < f->count && !calls; k++) calls = f->code[k].op == IR_CALL;
    if (calls) { /* too many calls deep (a function that calls itself without end): a panic, not a crash */
        movReloc(g, R11, RELOC_STACK_LIMIT, 0);
        rm(g, true, OP_CMP, 1, RSP, R11, 0);
        coldJump(g, CC_B, PANIC_STACK, f->line, -1, -1);
    }
    for (int i = 0; i < g->xmmCount; i++) { /* movups [rbp - xmmBase - 16i], xmm(first + i) */
        int x = g->xmmFirst + i;
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
        if ((ins->op == IR_STORE || ins->op == IR_VSTORE) && ins->dst >= 0) g->uses[ins->dst]++;
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
        g->at = k;
        genIns(g, &f->code[k]);
        int op = f->code[k].op;
        if (op != IR_LABEL) g->afterJump = op == IR_JMP || op == IR_RET;
    }

    /* epilogue */
    g->labelAt[g->epilogue] = here(g);
    for (int i = 0; i < g->xmmCount; i++) {
        int x = g->xmmFirst + i;
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

/* ---- the way in from C, and the way out of a panic ------------------------------------------------ */

static void jumpBack(Gen* g, int cc, int target) { /* a short jump to an earlier place; cc < 0: always */
    int rel = target - (here(g) + 2);
    if (cc < 0) byte(g, 0xEB);
    else byte(g, 0x70 | cc);
    byte(g, (int)(int8_t)rel);
}

/* int64_t enter(QkEnter* e) — Windows x64. Saves every callee-saved register (a panic abandons the frames
 * that would restore them), keeps its stack pointer where the escape finds it, copies the argument slots to
 * the outgoing area, calls the function, copies the result slots back, returns 0.
 * void escape(void) — from the runtime's panic: back onto that stack, return 1 from enter.
 * Frame: rbp-8..-56 pushed GPRs, rbp-216..-56 xmm6-15, rbp-224 the escape stack saved by an outer call. */
static void genTrampoline(Gen* g) {
    static const int saved[] = {RBX, RSI, RDI, R12, R13, R14, R15};
    static const uint8_t CMOVL[] = {0x0F, 0x4C};
    while (g->b->len % 16) byte(g, 0xCC);
    g->mc->enter = here(g);
    byte(g, 0x55); /* push rbp */
    movRR(g, RBP, RSP);
    for (int i = 0; i < 7; i++) {
        rex(g, false, 0, 0, saved[i], false);
        byte(g, 0x50 | (saved[i] & 7));
    }
    rex(g, true, 0, 0, RSP, false); /* sub rsp, 168 */
    byte(g, 0x81);
    byte(g, 0xEC);
    i32(g, 168);
    for (int i = 0; i < 10; i++) { /* movups [rbp - 216 + 16i], xmm(6+i) */
        int x = 6 + i;
        rex(g, false, x, 0, RBP, false);
        byte(g, 0x0F);
        byte(g, 0x11);
        byte(g, 0x80 | (x & 7) << 3 | RBP);
        i32(g, -216 + 16 * i);
    }
    movReloc(g, R11, RELOC_ESCAPE, 0);
    rm(g, true, OP_MOV_LOAD, 1, RAX, R11, 0); /* the outer call's escape stack, kept */
    rm(g, true, OP_MOV_STORE, 1, RAX, RBP, -224);
    rm(g, true, OP_MOV_STORE, 1, RSP, R11, 0); /* ours */
    movRR(g, RBX, g->sysv ? RDI : RCX); /* e */
    /* the outgoing area: 8 bytes per slot (arguments or results, the larger), 32 at least, 16 aligned */
    rm(g, true, OP_MOV_LOAD, 1, RAX, RBX, 16);
    rm(g, true, OP_MOV_LOAD, 1, RDX, RBX, 32);
    rr(g, true, OP_CMP, 1, RAX, RDX);
    rr(g, true, CMOVL, 2, RAX, RDX);
    shiftImm(g, RAX, 4, 3);
    rex(g, true, 0, 0, RAX, false); /* add rax, 47 */
    byte(g, 0x83);
    byte(g, 0xC0);
    byte(g, 47);
    rex(g, true, 0, 0, RAX, false); /* and rax, -16 */
    byte(g, 0x83);
    byte(g, 0xE0);
    byte(g, 0xF0);
    rr(g, true, OP_SUB, 1, RSP, RAX);
    /* arguments */
    rm(g, true, OP_MOV_LOAD, 1, RSI, RBX, 8);
    rm(g, true, OP_MOV_LOAD, 1, RCX, RBX, 16);
    rr(g, false, OP_XOR, 1, RDX, RDX);
    int loop1 = here(g);
    rr(g, true, OP_CMP, 1, RDX, RCX);
    int done1 = jccShort(g, CC_GE);
    memOp(g, 0, true, OP_MOV_LOAD, 1, RAX, (Mem){RSI, RDX, 8, 0});
    memOp(g, 0, true, OP_MOV_STORE, 1, RAX, (Mem){RSP, RDX, 8, 0});
    rex(g, true, 0, 0, RDX, false); /* inc rdx */
    byte(g, 0xFF);
    byte(g, 0xC2);
    jumpBack(g, -1, loop1);
    land(g, done1);
    byte(g, 0xFF); /* call qword [rbx] */
    byte(g, 0x13);
    /* results */
    rm(g, true, OP_MOV_LOAD, 1, RDI, RBX, 24);
    rm(g, true, OP_MOV_LOAD, 1, RCX, RBX, 32);
    rr(g, false, OP_XOR, 1, RDX, RDX);
    int loop2 = here(g);
    rr(g, true, OP_CMP, 1, RDX, RCX);
    int done2 = jccShort(g, CC_GE);
    memOp(g, 0, true, OP_MOV_LOAD, 1, RAX, (Mem){RSP, RDX, 8, 0});
    memOp(g, 0, true, OP_MOV_STORE, 1, RAX, (Mem){RDI, RDX, 8, 0});
    rex(g, true, 0, 0, RDX, false);
    byte(g, 0xFF);
    byte(g, 0xC2);
    jumpBack(g, -1, loop2);
    land(g, done2);
    rr(g, false, OP_XOR, 1, RAX, RAX);
    int exit = here(g);
    movReloc(g, R11, RELOC_ESCAPE, 0); /* the outer call's escape stack back */
    rm(g, true, OP_MOV_LOAD, 1, RCX, RBP, -224);
    rm(g, true, OP_MOV_STORE, 1, RCX, R11, 0);
    for (int i = 0; i < 10; i++) {
        int x = 6 + i;
        rex(g, false, x, 0, RBP, false);
        byte(g, 0x0F);
        byte(g, 0x10);
        byte(g, 0x80 | (x & 7) << 3 | RBP);
        i32(g, -216 + 16 * i);
    }
    static const uint8_t LEA[] = {0x8D};
    rm(g, true, LEA, 1, RSP, RBP, -56);
    for (int i = 6; i >= 0; i--) {
        rex(g, false, 0, 0, saved[i], false);
        byte(g, 0x58 | (saved[i] & 7));
    }
    byte(g, 0x5D); /* pop rbp */
    byte(g, 0xC3);

    while (g->b->len % 16) byte(g, 0xCC);
    g->mc->escape = here(g);
    movReloc(g, R11, RELOC_ESCAPE, 0);
    rm(g, true, OP_MOV_LOAD, 1, RSP, R11, 0);
    rm(g, true, LEA, 1, RBP, RSP, 224);
    movImm(g, RAX, 1);
    byte(g, 0xE9);
    i32(g, exit - (here(g) + 4));
}

bool qkGenX64(const IrProgram* p, MachineCode* out) {
    memset(out, 0, sizeof *out);
    Gen g = {0};
    g.mc = out;
    g.b = &out->code;
    g.prog = p;
    g.sysv = qkAbi == QK_ABI_SYSV;
    g.xmmFirst = g.sysv ? 8 : 6;
    g.fnAt = qkNew(sizeof(int) * (size_t)(p->count ? p->count : 1));
    for (int i = 0; i < p->count; i++) {
        while (g.b->len % 16) byte(&g, 0xCC);
        g.fnAt[i] = here(&g);
        g.fnIndex = i;
        genFn(&g, &p->fns[i]);
    }
    genTrampoline(&g);
    for (int i = 0; i < g.callCount; i++) {
        int32_t rel = g.fnAt[g.callFixups[i].target] - (g.callFixups[i].at + 4);
        memcpy(g.b->data + g.callFixups[i].at, &rel, 4);
    }
    out->fnCount = p->count;
    out->fnAt = qkNew(sizeof(int32_t) * (size_t)(p->count ? p->count : 1));
    for (int i = 0; i < p->count; i++) out->fnAt[i] = g.fnAt[i];
    return true;
}

/* For testing the System V code on Windows: one stub per runtime function, called the System V way, which
 * calls the function the Windows way. Its arguments wait in the stub's frame, then go where Windows wants
 * them; rbx, rbp and r12-r15 are kept by both conventions, and everything else System V lets change. */
bool qkGenSysvAdapters(QkBuf* out, int32_t* at) {
    Gen g = {0};
    g.b = out;
    static const int sysvGpr[] = {RDI, RSI, RDX, RCX, R8, R9};
    static const int winGpr[] = {RCX, RDX, R8, R9};
    for (int id = 0; id < RT_COUNT; id++) {
        const RtFn* f = &qkRuntime[id];
        while (g.b->len % 16) byte(&g, 0xCC);
        at[id] = here(&g);
        if (!f->addr) continue;
        byte(&g, 0x55); /* push rbp */
        movRR(&g, RBP, RSP);
        rex(&g, true, 0, 0, RSP, false); /* sub rsp, 176: 14 saved arguments, then the outgoing area */
        byte(&g, 0x81);
        byte(&g, 0xEC);
        i32(&g, 176);
        for (int i = 0; i < 6; i++) rm(&g, true, OP_MOV_STORE, 1, sysvGpr[i], RBP, -8 * (i + 1));
        for (int i = 0; i < 8; i++) sseRM(&g, 0xF2, false, 0x11, i, RBP, -56 - 8 * (i + 1));
        int ints = 0, floats = 0;
        for (int i = 0; f->args[i]; i++) {
            bool isF = f->args[i] == 'F';
            int32_t from = isF ? -56 - 8 * (++floats) : -8 * (++ints);
            if (i < 4) {
                if (isF) sseRM(&g, 0xF2, false, 0x10, i, RBP, from);
                else rm(&g, true, OP_MOV_LOAD, 1, winGpr[i], RBP, from);
            } else {
                rm(&g, true, OP_MOV_LOAD, 1, RAX, RBP, from);
                rm(&g, true, OP_MOV_STORE, 1, RAX, RSP, 8 * i);
            }
        }
        movImm(&g, RAX, (int64_t)(intptr_t)f->addr);
        byte(&g, 0xFF); /* call rax */
        byte(&g, 0xD0);
        movRR(&g, RSP, RBP);
        byte(&g, 0x5D); /* pop rbp */
        byte(&g, 0xC3);
    }
    return true;
}
