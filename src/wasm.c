/* Quark — WebAssembly (wasm32): the same IR the machine code comes from, as a Wasm module, and the small
 * JavaScript loader that runs it (src/wasm.js: Node.js or a browser).
 *
 *   quark build file.qk --target wasm32 [-o file.wasm]   writes file.wasm and file.js (node file.js [args])
 *
 * The runtime is the loader's: printing, texts, maps, files and panics are JavaScript functions the module
 * imports ("rt"), working on its memory with the same layouts the native runtime uses ([length][bytes] texts,
 * [count][slots] arrays, the same map entries). There is no C compiler to Wasm here (Quark needs none), a host
 * that runs Wasm has JavaScript anyway, and text, number formatting and the outside world are what JavaScript
 * already does well. Host functions (extern fn) are imports too ("host"), given by whoever loads the module.
 *
 * Every register is a local (i64 or f64, by its kind). Every Quark function takes and gives i64 slots (a Float's
 * bits), so a function kept as a value (call_indirect, through the table) never meets another signature, and a
 * host can call any function with BigInts. Control flow: the code between labels is a segment; segment k
 * follows the end of block k, so a jump forward is a br out to its block, and a jump back sets pc and goes
 * round the one loop that holds them all, whose br_table enters segment pc. Panics carry the line (and the
 * inlined calls) like the native ones; their stack of calls comes from the engine's stack trace: each frame's
 * code offset finds the call site recorded here, in the module's "quark" section. */
#include "qk.h"

#include <stdio.h>
#include <stdlib.h>

#define DATA_BASE 1024 /* the text literals and constant arrays; below them nothing (address 0 is no address) */

enum { I64 = 0x7E, F64 = 0x7C, I32 = 0x7F };

/* ---- encoding ------------------------------------------------------------------------------------ */

static void u8(QkBuf* b, int v) { qkBufByte(b, (uint8_t)v); }

static void uleb(QkBuf* b, uint64_t v) {
    do {
        uint8_t c = v & 0x7F;
        v >>= 7;
        if (v) c |= 0x80;
        qkBufByte(b, c);
    } while (v);
}

static void sleb(QkBuf* b, int64_t v) {
    for (;;) {
        uint8_t c = v & 0x7F;
        v >>= 7; /* arithmetic */
        bool done = (v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40));
        if (!done) c |= 0x80;
        qkBufByte(b, c);
        if (done) return;
    }
}

static int ulebSize(uint64_t v) {
    int n = 0;
    do {
        v >>= 7;
        n++;
    } while (v);
    return n;
}

static void name(QkBuf* b, const char* s) {
    size_t n = strlen(s);
    uleb(b, n);
    qkBufAdd(b, s, n);
}

static void section(QkBuf* out, int id, const QkBuf* payload) {
    u8(out, id);
    uleb(out, payload->len);
    if (payload->len) qkBufAdd(out, payload->data, payload->len);
}

/* ---- the module's state -------------------------------------------------------------------------- */

typedef struct Site { /* a call between Quark functions: where its call instruction is, for a panic's stack */
    int32_t at, line, caller, callee, inl;
} Site;

typedef struct W {
    const IrProgram* p;
    char** sigs; /* function types: "II:I" (params, results; I i64, F f64, i i32) */
    int sigCount, sigCap;
    int rtFunc[RT_COUNT]; /* each runtime function's import index, -1: not called */
    int* hostFunc;
    int importCount;
    int fuelGlobal; /* -1: none */
    /* the function being made */
    const IrFn* f;
    int fnIndex;
    QkBuf* b;
    int np;         /* its params (locals 0..np-1) */
    int pc, t0, t1; /* locals: the segment to enter (i32), scratch (i64) */
    int segCount;
    int* labelSeg;
    int seg;        /* the segment being made */
    int extra;      /* blocks opened inside it (an if) */
    bool loop;
    Site* sites;
    int siteCount, siteCap;
    /* functions kept as values: the table holds an adapter for each, of one type (maxP slots in, maxR out), so
     * a call through the table (a closure's, with its env last) meets no other signature */
    int maxP, maxR;
} W;

static int sigOf(W* w, const char* s) {
    for (int i = 0; i < w->sigCount; i++)
        if (!strcmp(w->sigs[i], s)) return i;
    char* c = qkDup(s, strlen(s));
    QK_PUSH(w->sigs, w->sigCount, w->sigCap, c);
    return w->sigCount - 1;
}

/* n i64 params, m i64 results: every Quark function's type */
static int slotSig(W* w, int n, int m) {
    char s[512];
    int k = 0;
    for (int i = 0; i < n && k < 250; i++) s[k++] = 'I';
    s[k++] = ':';
    for (int i = 0; i < m && k < 500; i++) s[k++] = 'I';
    s[k] = 0;
    return sigOf(w, s);
}

static int valType(char k) { return k == 'F' ? F64 : k == 'i' ? I32 : I64; }

/* ---- instructions -------------------------------------------------------------------------------- */

static char kindOf(W* w, int v) { return w->f->kinds[v]; }
static int localOf(W* w, int v) { return w->np + v; }

/* the value of register v, as want ('I' or 'F'): its bits as they are */
static void get(W* w, int v, char want) {
    u8(w->b, 0x20);
    uleb(w->b, (uint64_t)localOf(w, v));
    char k = kindOf(w, v);
    if (k == want) return;
    u8(w->b, want == 'F' ? 0xBF : 0xBD); /* f64.reinterpret_i64 / i64.reinterpret_f64 */
}

/* the value on the stack (of kind have) into register v */
static void set(W* w, int v, char have) {
    char k = kindOf(w, v);
    if (k != have) u8(w->b, k == 'F' ? 0xBF : 0xBD);
    u8(w->b, 0x21);
    uleb(w->b, (uint64_t)localOf(w, v));
}

static void getLocal(W* w, int l) {
    u8(w->b, 0x20);
    uleb(w->b, (uint64_t)l);
}
static void setLocal(W* w, int l) {
    u8(w->b, 0x21);
    uleb(w->b, (uint64_t)l);
}
static void i64c(W* w, int64_t v) {
    u8(w->b, 0x42);
    sleb(w->b, v);
}
static void i32c(W* w, int32_t v) {
    u8(w->b, 0x41);
    sleb(w->b, v);
}

static void blockOpen(W* w, int op) { /* block / loop / if, no result */
    u8(w->b, op);
    u8(w->b, 0x40);
    w->extra++;
}
static void blockEnd(W* w) {
    u8(w->b, 0x0B);
    w->extra--;
}

/* panic(code, line, a, b): a and b are registers (-1: 0); line carries the inlined calls above its 32 bits */
static void panic(W* w, int code, const IrIns* ins, int64_t line, int a, int b) {
    i64c(w, code);
    i64c(w, (line & 0xFFFFFFFF) | (int64_t)(ins ? ins->inl : 0) << 32);
    if (a >= 0) get(w, a, 'I');
    else i64c(w, 0);
    if (b >= 0) get(w, b, 'I');
    else i64c(w, 0);
    u8(w->b, 0x10);
    uleb(w->b, (uint64_t)w->rtFunc[RT_PANIC]);
    u8(w->b, 0x00); /* unreachable: the loader's panic never returns */
}

/* if (the i32 on the stack) panic */
static void panicIf(W* w, int code, const IrIns* ins, int a, int b) {
    blockOpen(w, 0x04);
    panic(w, code, ins, ins->line, a, b);
    blockEnd(w);
}

/* a jump to label l: forward, a br out to its segment's block; back, pc and round the loop */
static void jumpTo(W* w, int l) {
    int t = w->labelSeg[l];
    if (t > w->seg) {
        u8(w->b, 0x0C);
        uleb(w->b, (uint64_t)(t - w->seg - 1 + w->extra));
    } else {
        i32c(w, t);
        setLocal(w, w->pc);
        u8(w->b, 0x0C);
        uleb(w->b, (uint64_t)(w->segCount - 1 - w->seg + w->extra));
    }
}

/* the address [a + b * scale + c] as an i32, with what fits in the memarg's offset left there */
static uint32_t address(W* w, const IrIns* ins) {
    const IrFn* f = w->f;
    if (f->isConst && f->isConst[ins->b]) {
        int64_t off = f->constValue[ins->b] * ins->imm + ins->c;
        if (off >= 0 && off < 0x7FFFFFFF) {
            get(w, ins->a, 'I');
            u8(w->b, 0xA7); /* i32.wrap_i64 */
            return (uint32_t)off;
        }
    }
    get(w, ins->a, 'I');
    get(w, ins->b, 'I');
    if (ins->imm != 1) {
        i64c(w, ins->imm);
        u8(w->b, 0x7E); /* i64.mul */
    }
    u8(w->b, 0x7C); /* i64.add */
    if (ins->c) {
        i64c(w, ins->c);
        u8(w->b, 0x7C);
    }
    u8(w->b, 0xA7);
    return 0;
}

static void memarg(W* w, uint32_t offset) {
    uleb(w->b, 3); /* 8-byte aligned (a hint) */
    uleb(w->b, offset);
}

static void callRt(W* w, int id) {
    u8(w->b, 0x10);
    uleb(w->b, (uint64_t)w->rtFunc[id]);
}

static void genIns(W* w, const IrIns* ins) {
    QkBuf* b = w->b;
    switch (ins->op) {
    case IR_CONST:
        if (ins->c == 1) {
            i64c(w, DATA_BASE + ins->imm);
            set(w, ins->dst, 'I');
        } else if (ins->c == 2) { /* a function as a value: its place in the table (0 is none) */
            i64c(w, ins->imm + 1);
            set(w, ins->dst, 'I');
        } else if (kindOf(w, ins->dst) == 'F') {
            u8(b, 0x44); /* f64.const */
            qkBufAdd(b, &ins->imm, 8);
            set(w, ins->dst, 'F');
        } else {
            i64c(w, ins->imm);
            set(w, ins->dst, 'I');
        }
        break;
    case IR_MOV:
        get(w, ins->a, kindOf(w, ins->dst));
        set(w, ins->dst, kindOf(w, ins->dst));
        break;
    case IR_ADD: case IR_SUB: case IR_MUL: {
        int op = ins->op == IR_ADD ? 0x7C : ins->op == IR_SUB ? 0x7D : 0x7E;
        get(w, ins->a, 'I');
        get(w, ins->b, 'I');
        u8(b, op);
        if (!ins->cond) {
            set(w, ins->dst, 'I');
            break;
        }
        /* checked: the result in t0, then whether it went past the limits */
        setLocal(w, w->t0);
        if (ins->op == IR_ADD || ins->op == IR_SUB) { /* add: (a ^ r) & (b ^ r) < 0; sub: (a ^ b) & (a ^ r) < 0 */
            get(w, ins->a, 'I');
            if (ins->op == IR_ADD) getLocal(w, w->t0);
            else get(w, ins->b, 'I');
            u8(b, 0x85); /* i64.xor */
            if (ins->op == IR_ADD) get(w, ins->b, 'I');
            else get(w, ins->a, 'I');
            getLocal(w, w->t0);
            u8(b, 0x85);
            u8(b, 0x83); /* i64.and */
            i64c(w, 0);
            u8(b, 0x53); /* i64.lt_s */
        } else { /* mul: a == -1 ? b == MIN : a != 0 && r / a != b */
            get(w, ins->a, 'I');
            i64c(w, -1);
            u8(b, 0x51); /* i64.eq */
            u8(b, 0x04);
            u8(b, I32); /* if (result i32) */
            get(w, ins->b, 'I');
            i64c(w, INT64_MIN);
            u8(b, 0x51);
            u8(b, 0x05); /* else */
            get(w, ins->a, 'I');
            u8(b, 0x50); /* i64.eqz */
            u8(b, 0x04);
            u8(b, I32);
            i32c(w, 0);
            u8(b, 0x05);
            getLocal(w, w->t0);
            get(w, ins->a, 'I');
            u8(b, 0x7F); /* i64.div_s */
            get(w, ins->b, 'I');
            u8(b, 0x52); /* i64.ne */
            u8(b, 0x0B);
            u8(b, 0x0B);
        }
        panicIf(w, PANIC_OVERFLOW, ins, -1, -1);
        getLocal(w, w->t0);
        set(w, ins->dst, 'I');
        break;
    }
    case IR_DIV: case IR_MOD: {
        /* floored: the quotient rounds down, the remainder takes the divisor's sign; by -1 it wraps (no trap) */
        get(w, ins->b, 'I');
        u8(b, 0x50);
        panicIf(w, PANIC_DIVIDE, ins, -1, -1);
        get(w, ins->b, 'I');
        i64c(w, -1);
        u8(b, 0x51);
        u8(b, 0x04);
        u8(b, I64); /* if (result i64) */
        if (ins->op == IR_DIV) {
            i64c(w, 0);
            get(w, ins->a, 'I');
            u8(b, 0x7D);
        } else {
            i64c(w, 0);
        }
        u8(b, 0x05); /* else */
        get(w, ins->a, 'I');
        get(w, ins->b, 'I');
        u8(b, 0x81); /* i64.rem_s */
        setLocal(w, w->t1);
        get(w, ins->a, 'I');
        get(w, ins->b, 'I');
        u8(b, 0x7F); /* i64.div_s */
        setLocal(w, w->t0);
        /* r != 0 && (r ^ b) < 0: q - 1, r + b */
        getLocal(w, w->t1);
        i64c(w, 0);
        u8(b, 0x52);
        getLocal(w, w->t1);
        get(w, ins->b, 'I');
        u8(b, 0x85);
        i64c(w, 0);
        u8(b, 0x53);
        u8(b, 0x71); /* i32.and */
        u8(b, 0x04);
        u8(b, 0x40);
        getLocal(w, w->t0);
        i64c(w, 1);
        u8(b, 0x7D);
        setLocal(w, w->t0);
        getLocal(w, w->t1);
        get(w, ins->b, 'I');
        u8(b, 0x7C);
        setLocal(w, w->t1);
        u8(b, 0x0B);
        getLocal(w, ins->op == IR_DIV ? w->t0 : w->t1);
        u8(b, 0x0B);
        set(w, ins->dst, 'I');
        break;
    }
    case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SAR: {
        static const int ops[] = {0x83, 0x84, 0x85, 0x86, 0x87}; /* and or xor shl shr_s (the count modulo 64) */
        get(w, ins->a, 'I');
        get(w, ins->b, 'I');
        u8(b, ops[ins->op - IR_AND]);
        set(w, ins->dst, 'I');
        break;
    }
    case IR_NEG:
        i64c(w, 0);
        get(w, ins->a, 'I');
        u8(b, 0x7D);
        set(w, ins->dst, 'I');
        break;
    case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV: {
        static const int ops[] = {0xA0, 0xA1, 0xA2, 0xA3};
        get(w, ins->a, 'F');
        get(w, ins->b, 'F');
        u8(b, ops[ins->op - IR_FADD]);
        set(w, ins->dst, 'F');
        break;
    }
    case IR_FMIN: case IR_FMAX: /* as the processor's minsd / maxsd: a < b ? a : b (b when either is NaN) */
        get(w, ins->a, 'F');
        get(w, ins->b, 'F');
        get(w, ins->a, 'F');
        get(w, ins->b, 'F');
        u8(b, ins->op == IR_FMIN ? 0x63 : 0x64); /* f64.lt / f64.gt */
        u8(b, 0x1B); /* select */
        set(w, ins->dst, 'F');
        break;
    case IR_FNEG:
        get(w, ins->a, 'F');
        u8(b, 0x9A);
        set(w, ins->dst, 'F');
        break;
    case IR_SQRT:
        get(w, ins->a, 'F');
        u8(b, 0x9F);
        set(w, ins->dst, 'F');
        break;
    case IR_CMP: {
        static const int ops[] = {0x51, 0x52, 0x53, 0x57, 0x55, 0x59}; /* eq ne lt_s le_s gt_s ge_s */
        get(w, ins->a, 'I');
        get(w, ins->b, 'I');
        u8(b, ops[ins->cond]);
        u8(b, 0xAD); /* i64.extend_i32_u */
        set(w, ins->dst, 'I');
        break;
    }
    case IR_FCMP: {
        static const int ops[] = {0x61, 0x62, 0x63, 0x65, 0x64, 0x66}; /* eq ne lt le gt ge: false on NaN but ne */
        get(w, ins->a, 'F');
        get(w, ins->b, 'F');
        u8(b, ops[ins->cond]);
        u8(b, 0xAD);
        set(w, ins->dst, 'I');
        break;
    }
    case IR_NOT:
        get(w, ins->a, 'I');
        u8(b, 0x50);
        u8(b, 0xAD);
        set(w, ins->dst, 'I');
        break;
    case IR_I2F:
        get(w, ins->a, 'I');
        u8(b, 0xB9); /* f64.convert_i64_s */
        set(w, ins->dst, 'F');
        break;
    case IR_F2I: { /* as cvttsd2si: too big or NaN give the minimum */
        get(w, ins->a, 'F');
        u8(b, 0xFC);
        uleb(b, 6); /* i64.trunc_sat_f64_s */
        i64c(w, INT64_MIN);
        get(w, ins->a, 'F');
        u8(b, 0x44);
        double lo = -9223372036854775808.0, hi = 9223372036854775808.0;
        qkBufAdd(b, &lo, 8);
        u8(b, 0x66); /* f64.ge */
        get(w, ins->a, 'F');
        u8(b, 0x44);
        qkBufAdd(b, &hi, 8);
        u8(b, 0x63); /* f64.lt */
        u8(b, 0x71);
        u8(b, 0x1B);
        if (ins->cond) {
            setLocal(w, w->t0);
            getLocal(w, w->t0);
            i64c(w, INT64_MIN);
            u8(b, 0x51);
            panicIf(w, PANIC_TOO_BIG, ins, -1, -1);
            getLocal(w, w->t0);
        }
        set(w, ins->dst, 'I');
        break;
    }
    case IR_ROUND:
        get(w, ins->a, 'F');
        u8(b, ins->imm == 1 ? 0x9C : ins->imm == 2 ? 0x9B : 0x9E); /* floor, ceil, nearest (ties to even) */
        set(w, ins->dst, 'F');
        break;
    case IR_LOAD: {
        uint32_t off = address(w, ins);
        bool fl = kindOf(w, ins->dst) == 'F';
        u8(b, fl ? 0x2B : 0x29);
        memarg(w, off);
        set(w, ins->dst, fl ? 'F' : 'I');
        break;
    }
    case IR_STORE: {
        uint32_t off = address(w, ins);
        bool fl = kindOf(w, ins->dst) == 'F';
        get(w, ins->dst, fl ? 'F' : 'I');
        u8(b, fl ? 0x39 : 0x37);
        memarg(w, off);
        break;
    }
    case IR_BOUNDS:
        get(w, ins->a, 'I');
        get(w, ins->b, 'I');
        u8(b, 0x5A); /* i64.ge_u */
        panicIf(w, (int)ins->imm, ins, ins->a, ins->b);
        break;
    case IR_LABEL:
        break; /* a segment's start: placed by genFn */
    case IR_JMP:
        jumpTo(w, (int)ins->imm);
        break;
    case IR_JZ: case IR_JNZ: {
        get(w, ins->a, 'I');
        if (ins->op == IR_JNZ) {
            i64c(w, 0);
            u8(b, 0x52);
        } else {
            u8(b, 0x50);
        }
        int t = w->labelSeg[ins->imm];
        if (t > w->seg) { /* br_if */
            u8(b, 0x0D);
            uleb(b, (uint64_t)(t - w->seg - 1 + w->extra));
        } else {
            blockOpen(w, 0x04);
            jumpTo(w, (int)ins->imm);
            blockEnd(w);
        }
        break;
    }
    case IR_CALL: {
        const IrFn* callee = ins->a < 0 ? &w->p->fns[ins->imm] : NULL;
        int params = callee ? callee->paramCount : ins->argCount;
        int results = callee ? callee->resultCount : ins->retCount;
        for (int i = 0; i < params; i++) {
            if (i < ins->argCount) get(w, ins->args[i], 'I');
            else i64c(w, 0);
        }
        Site s = {(int32_t)b->len, ins->line, w->fnIndex, (int32_t)ins->imm, ins->inl};
        if (callee) {
            u8(b, 0x10);
            uleb(b, (uint64_t)(w->importCount + ins->imm));
        } else { /* through the table: maxP slots in (zeros after the arguments), maxR out */
            for (int i = params; i < w->maxP; i++) i64c(w, 0);
            results = w->maxR;
            get(w, ins->a, 'I');
            u8(b, 0xA7);
            s.at = (int32_t)b->len;
            u8(b, 0x11); /* call_indirect */
            uleb(b, (uint64_t)slotSig(w, w->maxP, w->maxR));
            uleb(b, 0);
            s.callee = ins->imm >= 0 && ins->imm < w->p->count ? (int32_t)ins->imm : -1;
        }
        QK_PUSH(w->sites, w->siteCount, w->siteCap, s);
        for (int i = results - 1; i >= 0; i--) {
            if (i < ins->retCount) set(w, ins->rets[i], 'I');
            else u8(b, 0x1A); /* drop */
        }
        break;
    }
    case IR_RTCALL: {
        const RtFn* r = &qkRuntime[ins->imm];
        int n = (int)strlen(r->args);
        for (int i = 0; i < n; i++) {
            if (i < ins->argCount) get(w, ins->args[i], r->args[i]);
            else if (r->args[i] == 'F') {
                double z = 0;
                u8(b, 0x44);
                qkBufAdd(b, &z, 8);
            } else i64c(w, 0);
        }
        callRt(w, (int)ins->imm);
        if (r->ret != 'V') {
            if (ins->retCount) set(w, ins->rets[0], r->ret);
            else u8(b, 0x1A);
        }
        break;
    }
    case IR_HOSTCALL: {
        const IrExtern* x = &w->p->externs[ins->imm];
        for (int i = 0; i < x->argCount; i++) get(w, ins->args[i], x->argKinds[i]);
        u8(b, 0x10);
        uleb(b, (uint64_t)w->hostFunc[ins->imm]);
        for (int i = x->retCount - 1; i >= 0; i--) {
            if (i < ins->retCount) set(w, ins->rets[i], x->retKinds[i]);
            else u8(b, 0x1A);
        }
        break;
    }
    case IR_RET:
        for (int i = 0; i < w->f->resultCount; i++) {
            if (i < ins->argCount) get(w, ins->args[i], 'I');
            else i64c(w, 0);
        }
        u8(b, 0x0F);
        break;
    case IR_FUEL: /* one unit of the budget: below zero, a panic */
        u8(b, 0x23);
        uleb(b, (uint64_t)w->fuelGlobal);
        i64c(w, 1);
        u8(b, 0x7D);
        u8(b, 0x24);
        uleb(b, (uint64_t)w->fuelGlobal);
        u8(b, 0x23);
        uleb(b, (uint64_t)w->fuelGlobal);
        i64c(w, 0);
        u8(b, 0x53);
        panicIf(w, PANIC_FUEL, ins, -1, -1);
        break;
    case IR_NOP:
        break;
    }
}

/* One function's body: its locals, then the segments in their blocks. */
static void genFn(W* w, int index, QkBuf* body) {
    const IrFn* f = &w->p->fns[index];
    w->f = f;
    w->fnIndex = index;
    w->b = body;
    w->np = f->paramCount;
    w->extra = 0;
    int nv = f->vregCount;
    w->pc = w->np + nv;
    w->t0 = w->pc + 1;
    w->t1 = w->pc + 2;
    /* locals: the registers (runs of one type), pc, two i64 scratch */
    QkBuf decl = {0};
    int groups = 0;
    for (int v = 0; v < nv;) {
        int e = v;
        while (e < nv && (f->kinds[e] == 'F') == (f->kinds[v] == 'F')) e++;
        uleb(&decl, (uint64_t)(e - v));
        u8(&decl, f->kinds[v] == 'F' ? F64 : I64);
        groups++;
        v = e;
    }
    uleb(&decl, 1);
    u8(&decl, I32);
    uleb(&decl, 2);
    u8(&decl, I64);
    groups += 2;
    uleb(body, (uint64_t)groups);
    qkBufAdd(body, decl.data, decl.len);
    qkBufFree(&decl);
    /* the parameters into their registers, the constants into theirs */
    for (int i = 0; i < f->paramCount; i++) {
        getLocal(w, i);
        set(w, f->params[i], 'I');
    }
    if (f->isConst)
        for (int v = 0; v < nv; v++)
            if (f->isConst[v]) {
                i64c(w, f->constValue[v]);
                set(w, v, 'I');
            }
    /* segments: the code before the first label, then from each label on */
    int* segStart = qkNew(sizeof(int) * (size_t)(f->count + 2));
    w->labelSeg = qkNew(sizeof(int) * (size_t)(f->labelCount + 1));
    int segs = 1;
    segStart[0] = 0;
    for (int k = 0; k < f->count; k++)
        if (f->code[k].op == IR_LABEL) {
            if (f->code[k].imm >= 0 && f->code[k].imm <= f->labelCount) w->labelSeg[f->code[k].imm] = segs;
            segStart[segs++] = k;
        }
    segStart[segs] = f->count;
    w->segCount = segs;
    w->loop = false;
    for (int s = 0; s < segs && !w->loop; s++)
        for (int k = segStart[s]; k < segStart[s + 1]; k++) {
            const IrIns* ins = &f->code[k];
            if ((ins->op == IR_JMP || ins->op == IR_JZ || ins->op == IR_JNZ) && w->labelSeg[ins->imm] <= s) w->loop = true;
        }
    if (w->loop) {
        u8(body, 0x03);
        u8(body, 0x40);
    }
    for (int s = segs - 1; s >= 1; s--) {
        u8(body, 0x02);
        u8(body, 0x40);
    }
    if (w->loop) { /* enter segment pc */
        u8(body, 0x02);
        u8(body, 0x40);
        getLocal(w, w->pc);
        u8(body, 0x0E); /* br_table */
        uleb(body, (uint64_t)segs);
        for (int s = 0; s < segs; s++) uleb(body, (uint64_t)s);
        uleb(body, 0);
        u8(body, 0x0B);
    }
    for (int s = 0; s < segs; s++) {
        if (s > 0) u8(body, 0x0B);
        w->seg = s;
        for (int k = segStart[s]; k < segStart[s + 1]; k++) genIns(w, &f->code[k]);
    }
    if (w->loop) u8(body, 0x0B);
    for (int i = 0; i < f->resultCount; i++) i64c(w, 0); /* falling off the end: nothing (the code always returns) */
    u8(body, 0x0B);
}

/* ---- the module ---------------------------------------------------------------------------------- */

static bool supported(int id) {
    switch (id) {
    case RT_SPAWN: case RT_STEP: case RT_YIELD: case RT_WAIT: case RT_CAPTURE_BEGIN: case RT_CAPTURE_END:
    case RT_DEBUG_ENTER: case RT_DEBUG_LEAVE: case RT_DEBUG_LINE: case RT_DEBUG_VALUE:
        return false;
    default: return true;
    }
}

static const char* rtUser(int id) {
    switch (id) {
    case RT_SPAWN: return "spawn";
    case RT_STEP: return "step";
    case RT_YIELD: return "yield";
    case RT_WAIT: return "wait";
    default: return "the debugger";
    }
}

static void sigText(char* s, const char* params, char ret) {
    snprintf(s, 64, "%s:%s", params, ret == 'V' ? "" : ret == 'F' ? "F" : "I");
}

static void typeSection(W* w, QkBuf* out) {
    QkBuf t = {0};
    uleb(&t, (uint64_t)w->sigCount);
    for (int i = 0; i < w->sigCount; i++) {
        const char* s = w->sigs[i];
        const char* colon = strchr(s, ':');
        u8(&t, 0x60);
        uleb(&t, (uint64_t)(colon - s));
        for (const char* c = s; c < colon; c++) u8(&t, valType(*c));
        uleb(&t, strlen(colon + 1));
        for (const char* c = colon + 1; *c; c++) u8(&t, valType(*c));
    }
    section(out, 1, &t);
    qkBufFree(&t);
}

bool qkGenWasm(const IrProgram* p, const char* path, int64_t fuel, QkBuf* out) {
    W w = {0};
    w.p = p;
    w.fuelGlobal = -1;
    if (p->mainIndex < 0) {
        fputs("quark: a program starts at fn main() { ... }\n", stderr);
        return false;
    }
    /* the runtime functions called, and none Wasm has no way to give yet */
    bool used[RT_COUNT] = {0};
    used[RT_PANIC] = true;
    for (int i = 0; i < p->count; i++)
        for (int k = 0; k < p->fns[i].count; k++) {
            const IrIns* ins = &p->fns[i].code[k];
            if (ins->op != IR_RTCALL) continue;
            if (!supported((int)ins->imm)) {
                fprintf(stderr, "%s:%d: %s is not available on wasm32 yet (it needs a stack of its own)\n", path, ins->line,
                        rtUser((int)ins->imm));
                return false;
            }
            used[ins->imm] = true;
        }
    /* imports: the runtime's, then the host's; their types first */
    int* importSig = qkNew(sizeof(int) * (RT_COUNT + (size_t)p->externCount + 1));
    int importCount = 0;
    for (int id = 0; id < RT_COUNT; id++) {
        w.rtFunc[id] = -1;
        if (!used[id]) continue;
        char s[64];
        sigText(s, qkRuntime[id].args, qkRuntime[id].ret);
        importSig[importCount] = sigOf(&w, s);
        w.rtFunc[id] = importCount++;
    }
    w.hostFunc = qkNew(sizeof(int) * (size_t)(p->externCount + 1));
    for (int i = 0; i < p->externCount; i++) {
        const IrExtern* x = &p->externs[i];
        QkBuf s = {0};
        qkBufAdd(&s, x->argKinds, (size_t)x->argCount);
        qkBufByte(&s, ':');
        qkBufAdd(&s, x->retKinds, (size_t)x->retCount);
        qkBufByte(&s, 0);
        importSig[importCount] = sigOf(&w, s.data);
        qkBufFree(&s);
        w.hostFunc[i] = importCount++;
    }
    w.importCount = importCount;
    int* fnSig = qkNew(sizeof(int) * (size_t)(p->count + 1));
    for (int i = 0; i < p->count; i++) fnSig[i] = slotSig(&w, p->fns[i].paramCount, p->fns[i].resultCount);
    /* the functions kept as values, and the widest call through the table */
    bool* taken = qkNew((size_t)p->count + 1);
    for (int i = 0; i < p->count; i++)
        for (int k = 0; k < p->fns[i].count; k++) {
            const IrIns* ins = &p->fns[i].code[k];
            if (ins->op == IR_CONST && ins->c == 2 && ins->imm >= 0 && ins->imm < p->count) taken[ins->imm] = true;
            if (ins->op == IR_CALL && ins->a >= 0) {
                if (ins->argCount > w.maxP) w.maxP = ins->argCount;
                if (ins->retCount > w.maxR) w.maxR = ins->retCount;
            }
        }
    int adapters = 0;
    int* adapterOf = qkNew(sizeof(int) * (size_t)(p->count + 1));
    for (int i = 0; i < p->count; i++) {
        adapterOf[i] = -1;
        if (!taken[i]) continue;
        adapterOf[i] = p->count + adapters++;
        if (p->fns[i].paramCount > w.maxP) w.maxP = p->fns[i].paramCount;
        if (p->fns[i].resultCount > w.maxR) w.maxR = p->fns[i].resultCount;
    }
    int adapterSig = adapters ? slotSig(&w, w.maxP, w.maxR) : 0;
    if (p->fuel) w.fuelGlobal = 0;

    /* the bodies (they may add types: call_indirect) */
    QkBuf* bodies = qkNew(sizeof(QkBuf) * (size_t)(p->count + 1));
    int* siteFirst = qkNew(sizeof(int) * (size_t)(p->count + 1));
    for (int i = 0; i < p->count; i++) {
        siteFirst[i] = w.siteCount;
        genFn(&w, i, &bodies[i]);
    }

    QkBuf s = {0};
    qkBufAdd(out, "\0asm\1\0\0\0", 8);
    typeSection(&w, out);
    /* imports */
    uleb(&s, (uint64_t)importCount);
    for (int id = 0; id < RT_COUNT; id++) {
        if (w.rtFunc[id] < 0) continue;
        name(&s, "rt");
        name(&s, qkRuntime[id].name);
        u8(&s, 0x00);
        uleb(&s, (uint64_t)importSig[w.rtFunc[id]]);
    }
    for (int i = 0; i < p->externCount; i++) {
        name(&s, "host");
        name(&s, p->externs[i].name);
        u8(&s, 0x00);
        uleb(&s, (uint64_t)importSig[w.hostFunc[i]]);
    }
    section(out, 2, &s);
    s.len = 0;
    /* functions */
    uleb(&s, (uint64_t)(p->count + adapters));
    for (int i = 0; i < p->count; i++) uleb(&s, (uint64_t)fnSig[i]);
    for (int i = 0; i < adapters; i++) uleb(&s, (uint64_t)adapterSig);
    section(out, 3, &s);
    s.len = 0;
    /* the table: every function, at its index + 1 */
    uleb(&s, 1);
    u8(&s, 0x70);
    u8(&s, 0x01);
    uleb(&s, (uint64_t)p->count + 1);
    uleb(&s, (uint64_t)p->count + 1);
    section(out, 4, &s);
    s.len = 0;
    /* memory: the data, then the heap the loader hands out */
    uint64_t heapBase = (DATA_BASE + p->data.len + 15) & ~(uint64_t)15;
    uleb(&s, 1);
    u8(&s, 0x01);
    uleb(&s, heapBase / 65536 + 1);
    uleb(&s, 65536);
    section(out, 5, &s);
    s.len = 0;
    /* globals: the fuel */
    if (p->fuel) {
        uleb(&s, 1);
        u8(&s, I64);
        u8(&s, 0x01);
        i64c(&(W){.b = &s}, fuel > 0 ? fuel : INT64_MAX);
        u8(&s, 0x0B);
        section(out, 6, &s);
        s.len = 0;
    }
    /* exports */
    uleb(&s, p->fuel ? 4 : 3);
    name(&s, "memory");
    u8(&s, 0x02);
    uleb(&s, 0);
    name(&s, "table");
    u8(&s, 0x01);
    uleb(&s, 0);
    name(&s, "main");
    u8(&s, 0x00);
    uleb(&s, (uint64_t)(importCount + p->mainIndex));
    if (p->fuel) {
        name(&s, "fuel");
        u8(&s, 0x03);
        uleb(&s, 0);
    }
    section(out, 7, &s);
    s.len = 0;
    /* elements: the table's functions */
    uleb(&s, 1);
    u8(&s, 0x00);
    i32c(&(W){.b = &s}, 1);
    u8(&s, 0x0B);
    uleb(&s, (uint64_t)p->count);
    for (int i = 0; i < p->count; i++) uleb(&s, (uint64_t)(importCount + (adapterOf[i] >= 0 ? adapterOf[i] : i)));
    section(out, 9, &s);
    s.len = 0;
    /* code: each body's place in the module turns its sites' offsets into the module's */
    uleb(&s, (uint64_t)(p->count + adapters));
    size_t* bodyAt = qkNew(sizeof(size_t) * (size_t)(p->count + 1));
    for (int i = 0; i < p->count; i++) {
        uleb(&s, bodies[i].len);
        bodyAt[i] = s.len;
        qkBufAdd(&s, bodies[i].data, bodies[i].len);
        qkBufFree(&bodies[i]);
    }
    for (int i = 0; i < p->count; i++) { /* an adapter: its function's slots in, its results and zeros out */
        if (adapterOf[i] < 0) continue;
        QkBuf a = {0};
        W aw = {.b = &a};
        uleb(&a, 0);
        for (int k = 0; k < p->fns[i].paramCount; k++) getLocal(&aw, k);
        u8(&a, 0x10);
        uleb(&a, (uint64_t)(importCount + i));
        for (int k = p->fns[i].resultCount; k < w.maxR; k++) i64c(&aw, 0);
        u8(&a, 0x0B);
        uleb(&s, a.len);
        qkBufAdd(&s, a.data, a.len);
        qkBufFree(&a);
    }
    size_t payloadAt = out->len + 1 + (size_t)ulebSize(s.len);
    for (int i = 0; i < p->count; i++) {
        int end = i + 1 < p->count ? siteFirst[i + 1] : w.siteCount;
        for (int k = siteFirst[i]; k < end; k++) w.sites[k].at += (int32_t)(payloadAt + bodyAt[i]);
    }
    section(out, 10, &s);
    s.len = 0;
    /* data */
    if (p->data.len) {
        uleb(&s, 1);
        u8(&s, 0x00);
        i32c(&(W){.b = &s}, DATA_BASE);
        u8(&s, 0x0B);
        uleb(&s, p->data.len);
        qkBufAdd(&s, p->data.data, p->data.len);
        section(out, 11, &s);
        s.len = 0;
    }
    /* names: the engine's stack traces say them */
    QkBuf names = {0};
    name(&names, "name");
    QkBuf sub = {0};
    uleb(&sub, (uint64_t)p->count);
    for (int i = 0; i < p->count; i++) {
        uleb(&sub, (uint64_t)(importCount + i));
        name(&sub, p->fns[i].name);
    }
    u8(&names, 1);
    uleb(&names, sub.len);
    qkBufAdd(&names, sub.data, sub.len);
    section(out, 0, &names);
    qkBufFree(&names);
    qkBufFree(&sub);
    /* "quark": what the loader needs (JSON): the path for messages, the heap, each function's name and line, the
     * call sites, the inlined calls */
    QkBuf j = {0};
    qkBufPuts(&j, "{\"path\": ");
    qkJsonString(&j, path);
    qkBufPrintf(&j, ", \"heap\": %llu, \"imports\": %d, \"main\": %d, \"fns\": [", (unsigned long long)heapBase, importCount,
                p->mainIndex);
    for (int i = 0; i < p->count; i++) {
        if (i) qkBufPuts(&j, ", ");
        qkJsonString(&j, p->fns[i].name);
    }
    qkBufPuts(&j, "], \"lines\": [");
    for (int i = 0; i < p->count; i++) qkBufPrintf(&j, "%s%d", i ? ", " : "", p->fns[i].line);
    qkBufPuts(&j, "], \"sites\": [");
    for (int k = 0; k < w.siteCount; k++) {
        const Site* t = &w.sites[k];
        qkBufPrintf(&j, "%s[%d, %d, %d, %d, %d]", k ? ", " : "", t->at, t->line, t->caller, t->callee, t->inl);
    }
    qkBufPuts(&j, "], \"inlines\": [");
    for (int k = 0; k < p->inlineCount; k++) {
        const InlineCall* c = &p->inlines[k];
        qkBufPrintf(&j, "%s[%d, ", k ? ", " : "", c->parent);
        qkJsonString(&j, c->callee ? c->callee : "");
        qkBufPrintf(&j, ", %d]", c->line);
    }
    qkBufPuts(&j, "]}");
    name(&s, "quark");
    qkBufAdd(&s, j.data, j.len);
    section(out, 0, &s);
    qkBufFree(&j);
    qkBufFree(&s);
    return true;
}

/* ---- the loader ---------------------------------------------------------------------------------- */

/* src/wasm.js, a string a line (build.sh puts it in, as it does the standard library) */
extern const char* const qkWasmLoaderLines[];

/* quark build file.qk --target wasm32 -o out.wasm: the module, and out.js beside it */
bool qkBuildWasm(const IrProgram* p, const char* path, int64_t fuel, const char* outPath) {
    QkBuf mod = {0};
    if (!qkGenWasm(p, path, fuel, &mod)) return false;
    char wasmPath[1024], jsPath[1024];
    if (outPath) {
        snprintf(wasmPath, sizeof wasmPath, "%s", outPath);
    } else {
        snprintf(wasmPath, sizeof wasmPath, "%s", path);
        char* dot = strrchr(wasmPath, '.');
        if (dot && !strchr(dot, '/') && !strchr(dot, '\\')) *dot = '\0';
        strncat(wasmPath, ".wasm", sizeof wasmPath - strlen(wasmPath) - 1);
    }
    snprintf(jsPath, sizeof jsPath, "%s", wasmPath);
    char* dot = strrchr(jsPath, '.');
    if (dot && !strchr(dot, '/') && !strchr(dot, '\\')) *dot = '\0';
    strncat(jsPath, ".js", sizeof jsPath - strlen(jsPath) - 1);
    if (!qkWriteFile(wasmPath, mod.data, mod.len)) {
        fprintf(stderr, "quark: cannot write %s\n", wasmPath);
        return false;
    }
    QkBuf js = {0};
    const char* base = wasmPath;
    for (const char* c = wasmPath; *c; c++)
        if (*c == '/' || *c == '\\') base = c + 1;
    for (int i = 0; qkWasmLoaderLines[i]; i++) {
        const char* line = qkWasmLoaderLines[i];
        const char* at = strstr(line, "@WASM@"); /* the module's file name, next to the loader */
        if (at) {
            qkBufAdd(&js, line, (size_t)(at - line));
            qkBufPuts(&js, base);
            qkBufPuts(&js, at + 6);
        } else {
            qkBufPuts(&js, line);
        }
        qkBufByte(&js, '\n');
    }
    if (!qkWriteFile(jsPath, js.data, js.len)) {
        fprintf(stderr, "quark: cannot write %s\n", jsPath);
        return false;
    }
    fprintf(stderr, "quark: wrote %s (%zu bytes) and %s (node %s)\n", wasmPath, mod.len, jsPath, jsPath);
    qkBufFree(&mod);
    qkBufFree(&js);
    return true;
}

