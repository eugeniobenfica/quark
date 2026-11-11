/* Quark — a small, compiled language whose programs are also node graphs.
 *
 * One header for the whole compiler: source text -> tokens (lexer.c) -> syntax tree (parser.c) -> types
 * (check.c) -> a small register IR with fused loops (lower.c) -> x86-64 machine code (x64.c), run in memory
 * or written into a standalone executable (main.c). The syntax tree also prints back as canonical text
 * (format.c) and travels to and from a node graph (graph.c). No dependency: no C compiler, no LLVM. */
#ifndef QUARK_H
#define QUARK_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- utilities (common.c) ------------------------------------------------------------------------ */

typedef struct QkBuf {
    char* data;
    size_t len, cap;
} QkBuf;

void qkBufAdd(QkBuf* b, const void* p, size_t n);
void qkBufPuts(QkBuf* b, const char* s);
void qkBufPrintf(QkBuf* b, const char* fmt, ...);
void qkBufByte(QkBuf* b, uint8_t v);
void qkBufFree(QkBuf* b);

void* qkNew(size_t bytes); /* zeroed; the compiler is short lived, so nothing is freed */
char* qkDup(const char* s, size_t n);
char* qkReadFile(const char* path, size_t* len);
bool qkWriteFile(const char* path, const void* data, size_t len);

/* Grows a vector held as (T* items, int count, int cap). */
#define QK_PUSH(items, count, cap, value)                                          \
    do {                                                                           \
        if ((count) == (cap)) {                                                    \
            int newCap_ = (cap) ? (cap) * 2 : 8;                                   \
            void* grown_ = qkNew(sizeof(*(items)) * (size_t)newCap_);              \
            if (count) memcpy(grown_, (items), sizeof(*(items)) * (size_t)(count)); \
            (items) = grown_;                                                      \
            (cap) = newCap_;                                                       \
        }                                                                          \
        (items)[(count)++] = (value);                                              \
    } while (0)

typedef struct QkSource {
    const char* path;
    const char* text;
    size_t len;
} QkSource;

extern int qkErrorCount;
void qkError(const QkSource* src, int line, int col, const char* fmt, ...);
const char* qkClosest(const char* name, const char* const* candidates, int count);

/* ---- tokens (lexer.c) ---------------------------------------------------------------------------- */

typedef enum TokKind {
    T_EOF, T_NEWLINE, T_NAME, T_TYPENAME, T_INT, T_FLOAT, T_TEXT,
    /* keywords */
    T_FN, T_LET, T_VAR, T_TYPE, T_ENUM, T_IF, T_ELSE, T_MATCH, T_FOR, T_IN, T_WHILE, T_RETURN, T_BREAK, T_CONTINUE,
    T_AND, T_OR, T_NOT, T_TRUE, T_FALSE,
    /* punctuation */
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACKET, T_RBRACKET, T_COMMA, T_DOT, T_DOTDOT, T_COLON, T_ARROW,
    T_FATARROW, T_ASSIGN, T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ, T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE, T_UNDERSCORE
} TokKind;

typedef struct Token {
    TokKind kind;
    const char* start;
    int len;
    int line, col;
    bool blankBefore; /* an empty line came before it */
} Token;

typedef struct Comment {
    int line;
    bool ownLine; /* alone on its line (a leading comment), or after code (a trailing one) */
    const char* text; /* without the slashes */
} Comment;

typedef struct Lexed {
    Token* tokens;
    int count, cap;
    Comment* comments;
    int commentCount, commentCap;
} Lexed;

void qkLex(const QkSource* src, Lexed* out);
const char* qkTokName(TokKind k);
extern const char* const qkKeywords[];
extern const int qkKeywordCount;

/* ---- syntax tree (parser.c) ---------------------------------------------------------------------- */

typedef struct Type Type;
typedef struct Expr Expr;
typedef struct Stmt Stmt;
typedef struct Decl Decl;
typedef struct Binding Binding;

typedef struct TypeExpr {
    const char* name; /* NULL for an array: [elem] */
    struct TypeExpr* elem;
    int line, col;
} TypeExpr;

typedef struct Param {
    const char* name;
    TypeExpr* type; /* NULL in a lambda: inferred */
    int line, col;
    Binding* binding;
} Param;

typedef struct Block {
    Stmt** stmts;
    int count, cap;
    const char* closingComment; /* comments before the closing brace */
} Block;

typedef struct Arm {
    const char* pattern; /* a case name, NULL for '_' */
    Expr* intPattern;    /* or an Int literal */
    Expr* body;
    int caseIndex;       /* resolved */
    const char* comment;
} Arm;

typedef enum ExprKind {
    E_INT, E_FLOAT, E_TEXT, E_BOOL, E_NAME, E_CALL, E_FIELD, E_INDEX, E_UNARY, E_BINARY, E_RANGE, E_IF, E_MATCH,
    E_LAMBDA, E_ARRAY, E_BLOCK, E_CASE /* Mode.Waveform, resolved from E_FIELD */
} ExprKind;

struct Expr {
    ExprKind kind;
    int line, col;
    Type* type;
    /* literals: the text as written (numbers keep 1_000 and 0.50) */
    const char* raw;
    int64_t i;
    double f;
    const char* text; /* E_TEXT: decoded */
    int textLen;
    const char* name; /* E_NAME, E_CALL callee, E_FIELD field */
    int op;           /* E_UNARY, E_BINARY: a TokKind */
    Expr* a;          /* operand, target, condition, subject */
    Expr* b;          /* second operand, index */
    Expr** args;      /* E_CALL (a method call keeps its receiver as args[0]), E_ARRAY */
    const char** argNames;
    int argCount, argCap;
    bool method;      /* written a.f(b) */
    bool implicit;    /* added by the checker (a conversion): not printed, not a node */
    Block* thenBlock; /* E_IF */
    Expr* elseExpr;   /* E_IF: a block or another if */
    Block* block;     /* E_BLOCK */
    Arm* arms;        /* E_MATCH */
    int armCount, armCap;
    Param* params;    /* E_LAMBDA */
    int paramCount;
    Expr* body;
    Expr* sugar;      /* a function name given where a lambda goes: what was written */
    /* resolved */
    Binding* binding;
    int builtin;
    Decl* decl;       /* a called function, a constructed record, an enum */
    int caseIndex;
};

typedef enum StmtKind { S_LET, S_VAR, S_ASSIGN, S_EXPR, S_FOR, S_WHILE, S_RETURN, S_BREAK, S_CONTINUE } StmtKind;

struct Stmt {
    StmtKind kind;
    int line, col;
    const char* name;      /* let, var, for */
    const char* indexName; /* for x, i in ... */
    TypeExpr* typeAnn;
    Expr* target;          /* assign */
    int op;                /* assign: T_ASSIGN, T_PLUSEQ... */
    Expr* value;           /* let, var, assign, expr, return, while condition */
    Expr* iter;            /* for */
    Block* body;           /* for, while */
    Binding* binding;
    Binding* indexBinding;
    const char* leading;   /* comment lines above */
    const char* trailing;  /* comment after it on its line */
    bool blankBefore;
};

typedef enum DeclKind { D_FN, D_TYPE, D_ENUM } DeclKind;

struct Decl {
    DeclKind kind;
    const char* name;
    int line, col;
    Param* params; /* function parameters, record fields */
    int paramCount, paramCap;
    TypeExpr* ret;
    Expr* body;    /* E_BLOCK, or an expression after '=' */
    const char** cases;
    int caseCount, caseCap;
    const char* leading;
    const char* trailing;
    /* resolved */
    Type* type;    /* the record or enum type */
    Type* retType;
    int state;     /* 0 unchecked, 1 checking, 2 done */
    int index;
};

typedef struct Program {
    QkSource src;
    Decl** decls;
    int count, cap;
    const char* closingComment;
} Program;

bool qkParse(const QkSource* src, Program* out);
Expr* qkNewExpr(ExprKind kind, int line, int col);

/* ---- types and checking (check.c) ---------------------------------------------------------------- */

typedef enum TypeKind {
    TY_ERROR, TY_VOID, TY_INT, TY_FLOAT, TY_BOOL, TY_TEXT, TY_VEC, TY_ARRAY, TY_RECORD, TY_ENUM, TY_RANGE, TY_ITER
} TypeKind;

struct Type {
    TypeKind kind;
    int n;       /* TY_VEC: 2, 3, 4 */
    Type* elem;  /* TY_ARRAY, TY_ITER */
    Decl* decl;  /* TY_RECORD, TY_ENUM */
};

struct Binding {
    const char* name;
    Type* type;
    bool mutable;
    bool isParam;
    int uses;
    int iterUses;    /* uses as the source of a loop (map, sum, for...) */
    bool readsVar;   /* its value reads a var: cannot be moved to where it is used */
    bool lazy;       /* a loop used once: fused into its one use, never stored */
    Expr* init;
    int depth;       /* loop depth where it lives */
    int* regs;       /* lowering: its slots */
    int nodeId;      /* graph: the node that carries it */
};

typedef enum Builtin {
    B_NONE,
    B_SQRT, B_SIN, B_COS, B_TAN, B_ASIN, B_ACOS, B_ATAN, B_ATAN2, B_EXP, B_LOG, B_POW, B_ABS, B_FLOOR, B_CEIL, B_ROUND,
    B_TOFLOAT, B_TOINT, B_MIN, B_MAX, B_CLAMP, B_LERP, B_LENGTH, B_DOT,
    B_LEN, B_FILL, B_MAP, B_FILTER, B_FOLD, B_SUM, B_HISTOGRAM, B_ANY, B_ALL, B_TOARRAY,
    B_PRINT, B_NOW,
    B_VEC2, B_VEC3, B_VEC4,
    B_COUNT
} Builtin;

extern const char* const qkBuiltinNames[B_COUNT];

extern Type qkTypeError, qkTypeVoid, qkTypeInt, qkTypeFloat, qkTypeBool, qkTypeText, qkTypeRange;
Type* qkTypeVec(int n);
Type* qkTypeArray(Type* elem);
Type* qkTypeIter(Type* elem);
bool qkTypeSame(const Type* a, const Type* b);
void qkTypeName(const Type* t, QkBuf* out);
int qkTypeSlots(const Type* t);
void qkTypeSlotKinds(const Type* t, char* out); /* 'I' or 'F' per slot */
bool qkIsIterable(const Type* t);
Type* qkElemType(const Type* t);

bool qkCheck(Program* p);
Decl* qkFindDecl(Program* p, const char* name);

/* ---- formatting (format.c) ----------------------------------------------------------------------- */

void qkFormat(const Program* p, QkBuf* out);
void qkFormatExpr(const Expr* e, QkBuf* out, int indent);
void qkFormatTypeExpr(const TypeExpr* t, QkBuf* out);

/* ---- node graphs (graph.c) ----------------------------------------------------------------------- */

void qkGraphExport(const Program* p, QkBuf* out);
bool qkGraphImport(const char* json, size_t len, const char* path, Program* out);

/* ---- intermediate code (lower.c) ----------------------------------------------------------------- */

typedef enum IrOp {
    IR_CONST,     /* dst = imm (an int, or a float's bits); c = 1: the data block's address + imm */
    IR_MOV,       /* dst = a */
    IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD, IR_NEG,          /* ints, wrapping; DIV and MOD check for zero */
    IR_FADD, IR_FSUB, IR_FMUL, IR_FDIV, IR_FNEG, IR_SQRT,
    IR_FMIN, IR_FMAX,
    IR_CMP,       /* dst = a <cond> b, ints */
    IR_FCMP,      /* dst = a <cond> b, floats */
    IR_NOT,       /* dst = a == 0 */
    IR_I2F, IR_F2I, /* F2I truncates */
    IR_ROUND,     /* dst = round(a) as a float, imm: 0 nearest, 1 floor, 2 ceil */
    IR_LOAD,      /* dst = [a + b * imm + c] (c: a byte offset) */
    IR_STORE,     /* [a + b * imm + c] = dst */
    IR_BOUNDS,    /* a (index) < b (length), unsigned, else panic imm */
    IR_LABEL, IR_JMP,
    IR_JZ, IR_JNZ, /* jump to label imm if a is zero / not zero */
    IR_CALL,      /* rets = function imm (args) */
    IR_RTCALL,    /* rets = runtime function imm (args) */
    IR_RET,       /* return args */
    IR_FUEL,      /* one unit of the budget */
    IR_NOP
} IrOp;

enum { C_EQ, C_NE, C_LT, C_LE, C_GT, C_GE };

typedef struct IrIns {
    uint8_t op;
    uint8_t cond;
    int dst, a, b, c;
    int64_t imm;
    int* args;
    int argCount;
    int* rets;
    int retCount;
    int line;
} IrIns;

typedef struct IrFn {
    const char* name;
    IrIns* code;
    int count, cap;
    char* kinds;     /* per virtual register: 'I' or 'F' */
    double* weight;  /* how much it is used, loops counting more */
    int vregCount, vregCap;
    int* params;     /* the parameters' registers, in order */
    int paramCount;
    int resultCount;
    int labelCount;
    int maxOut;      /* slots of the largest call's arguments or results */
    char* isConst;   /* set by qkOptimize: an int register defined once, by a constant (an immediate) */
    int64_t* constValue;
} IrFn;

typedef struct IrProgram {
    IrFn* fns;
    int count;
    int mainIndex;
    QkBuf data;       /* text literals */
    bool fuel;
} IrProgram;

bool qkLower(Program* p, IrProgram* out, bool fuel);
void qkIrDump(const IrProgram* p, QkBuf* out);
/* Folds copies into the instructions that compute them, drops unused code, turns constants into immediates
 * (ints) or hoists them to the start (floats). */
void qkOptimize(IrProgram* p);

/* ---- the runtime the compiled code calls (runtime.c) --------------------------------------------- */

typedef struct RtFn {
    const char* name;
    void* addr;
    const char* args; /* 'I' or 'F' per argument (at most 4) */
    char ret;         /* 'I', 'F' or 'V' */
} RtFn;

typedef enum RtId {
    RT_PANIC, RT_PRINT_INT, RT_PRINT_FLOAT, RT_PRINT_BOOL, RT_PRINT_TEXT, RT_PRINT_SEP, RT_PRINT_END, RT_ALLOC,
    RT_GROW, RT_COPY, RT_NOW, RT_SIN, RT_COS, RT_TAN, RT_ASIN, RT_ACOS, RT_ATAN, RT_ATAN2, RT_EXP, RT_LOG, RT_POW, RT_FUEL_OUT,
    RT_COUNT
} RtId;

extern const RtFn qkRuntime[RT_COUNT];
extern int64_t qkFuel;
extern bool qkUseAvx;      /* the code generator may use AVX (three-operand float instructions) */
bool qkCpuHasAvx(void);
extern const char* qkRunPath; /* for messages */

enum { PANIC_INDEX = 1, PANIC_DIVIDE = 2, PANIC_FUEL = 3, PANIC_HISTOGRAM = 4, PANIC_EMPTY = 5, PANIC_SIZE = 6 };

/* ---- machine code (x64.c) ------------------------------------------------------------------------ */

typedef struct Reloc {
    int32_t at;    /* where the 8-byte address goes */
    int32_t kind;  /* 0: a runtime function (target: RtId), 1: the data block (target: offset) */
    int64_t target;
} Reloc;

typedef struct MachineCode {
    QkBuf code;
    Reloc* relocs;
    int relocCount, relocCap;
    int32_t entry; /* offset of the entry stub: void entry(void) */
} MachineCode;

bool qkGenX64(const IrProgram* p, MachineCode* out);
/* Copies code and data into executable memory, patches addresses, returns the entry point. */
void (*qkLoad(const uint8_t* code, size_t codeLen, const uint8_t* data, size_t dataLen, const Reloc* relocs,
              int relocCount, int32_t entry))(void);

#endif
