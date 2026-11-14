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

void* qkNew(size_t bytes); /* zeroed; freed all at once by qkNewReset (after a compile) */
void qkNewReset(void);
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
/* Where errors go: NULL prints them to stderr (the command line); an embedding host collects them. */
extern void (*qkErrorSink)(const char* path, int line, int col, const char* message);
void qkError(const QkSource* src, int line, int col, const char* fmt, ...);
const char* qkClosest(const char* name, const char* const* candidates, int count);

/* ---- tokens (lexer.c) ---------------------------------------------------------------------------- */

typedef enum TokKind {
    T_EOF, T_NEWLINE, T_NAME, T_TYPENAME, T_INT, T_FLOAT, T_TEXT,
    /* keywords */
    T_FN, T_LET, T_VAR, T_TYPE, T_ENUM, T_IF, T_ELSE, T_MATCH, T_FOR, T_IN, T_WHILE, T_RETURN, T_BREAK, T_CONTINUE,
    T_AND, T_OR, T_NOT, T_TRUE, T_FALSE, T_EXTERN, T_WITH, T_EFFECT, T_USE, T_PUB, T_CONST,
    /* punctuation */
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACKET, T_RBRACKET, T_COMMA, T_DOT, T_DOTDOT, T_COLON, T_ARROW,
    T_FATARROW, T_ASSIGN, T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ, T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE, T_UNDERSCORE, T_QQ, T_QUESTION, T_BANG,
    T_AMP, T_PIPE, T_CARET, T_TILDE, /* & | ^ ~: an Int's bits */
    T_SHL, T_SHR                     /* << >>: made by the parser from < < and > > side by side (Pair<A, Tree<B>> stays) */
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
    bool optional; /* T? */
    bool fallible; /* T!: a T, or an error */
    struct TypeExpr* errType; /* T!ParseError: the error's own type (NULL: a Text, its message) */
    struct TypeExpr* key;     /* a map [key: elem] */
    struct TypeExpr** params; /* a function (A, B) -> elem */
    const char** paramNames;  /* ... with its values' names, when written: (loud: Bool) -> Text */
    int paramCount;
    bool isFn;
    bool isTuple;             /* (Int, Text): its types are params */
    struct TypeExpr** args;   /* a generic type's: Pair<Int, Text> */
    int argCount;
    bool effect;              /* effect (A) -> R: a function that changes the world */
} TypeExpr;

typedef struct Param {
    const char* name;
    TypeExpr* type; /* NULL in a lambda: inferred */
    int line, col;
    Binding* binding;
    Expr* fallback; /* a parameter's default value (fn f(x: Int = 1)) */
    bool boxed;     /* a field whose type holds its own type (enum Expr { Add(a: Expr, b: Expr) }): one slot, the
                     * address of its value's slots in the arena */
} Param;

typedef struct Block {
    Stmt** stmts;
    int count, cap;
    const char* closingComment; /* comments before the closing brace */
} Block;

typedef struct Arm {
    const char* pattern; /* a case name, NULL for '_' */
    Expr* intPattern;    /* or an Int literal (a range's start, NULL for ..b) */
    Expr* intHigh;       /* a range a..b: its end (excluded), NULL for a.. */
    bool isRange;
    Expr* textPattern;   /* or a text literal */
    Expr* guard;         /* pattern if condition => ... */
    Expr* body;
    int caseIndex;       /* resolved */
    const char* comment;
    const char** binds;  /* Circle(r) => ...: names for the case's data, in field order ("_": skipped) */
    int bindCount;
    Binding** bindings;
    struct Arm** subs;   /* a pattern inside, for a field (Some(Circle(r)), Point(x, 0)), or NULL: its name */
} Arm;

typedef enum ExprKind {
    E_INT, E_FLOAT, E_TEXT, E_BOOL, E_NAME, E_CALL, E_FIELD, E_INDEX, E_UNARY, E_BINARY, E_RANGE, E_IF, E_MATCH,
    E_LAMBDA, E_ARRAY, E_BLOCK, E_CASE, /* Color.Red, resolved from E_FIELD */
    E_WITH,   /* a with { field: value }: a copy of a record with fields changed */
    E_INTERP, /* "text {expr} text": parts around args */
    E_TRY,    /* a?: what an optional or a result holds, or leave the function with its None / Err */
    E_MAP     /* [k: v, ...]: args are keys and values, in turn */
} ExprKind;
#define E_KIND_COUNT (E_MAP + 1)

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
    bool typed;       /* checked already (a method call's receiver, checked to find its type's function) */
    const char* qualifier; /* E_CALL of a case with data: Shape.Circle(2) keeps "Shape" */
    const char** parts;    /* E_INTERP: the text around each of args (argCount + 1 pieces, decoded) */
    const char** rawParts; /* ... as written */
    const char** formats;  /* E_INTERP: each value's format ("{x:.2}": ".2"), or NULL */
    /* resolved */
    Binding* binding;
    int builtin;
    Decl* decl;       /* a called function, a constructed record, an enum, a constant */
    int caseIndex;
    Expr* callee;     /* E_CALL of a function kept in a field: that field (button.onPress(3)) */
    Expr** ordered;   /* E_CALL of a function or a case: every parameter's value, defaults filled in */
    int orderedCount;
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
    Expr* opValue;         /* assign with +=... on a record: the new value, target + value through its type's plus */
    Arm* pattern;          /* let Some(x) = value else { ... }: the case and its names */
    Block* elseBody;       /* ... and where it goes when the value is another case */
    const char* leading;   /* comment lines above */
    const char* trailing;  /* comment after it on its line */
    bool blankBefore;
};

typedef enum DeclKind { D_FN, D_TYPE, D_ENUM, D_CONST, D_USE /* use name: another file's pub declarations */ } DeclKind;

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
    bool isExtern;    /* extern fn (no body: the host's) or extern type (a host handle) */
    bool fromPrelude; /* the host's declarations: not printed, not in graphs */
    bool isEffect;    /* it changes the world: inferred from its body (it prints, calls an effect...), or written on
                       * what has no body (extern effect fn, an interface's effect fn) */
    bool isInterface; /* type Speaker { fn speak() -> Text }: functions any type with them can stand for */
    const char* owner;  /* fn Vec2.plus: the type it belongs to ("Vec2"); the name is "Vec2.plus" */
    Param** caseFields; /* D_ENUM: the data each case carries (NULL: none) */
    int* caseFieldCount;
    Type* optionOf;     /* the built-in T? (cases None and Some(value: T)) */
    Type* resultOf;     /* the built-in T! (cases Err(message: Text) and Ok(value: T)) */
    Type* errorOf;      /* ... and what its Err holds: Text, or the error type written (T!ParseError) */
    const char** typeParams; /* fn first<T>(...): its type parameters */
    int typeParamCount;
    bool isTest;        /* test "name" { ... }: run by quark test */
    const char* testName;
    /* specialization: a generic function, or one that takes functions, becomes one instance per use */
    bool isTemplate;    /* generic, or takes a function: checked loosely, lowered only as instances */
    Decl* templateOf;   /* an instance: the template it was made from */
    Type** typeArgs;    /* ... and its types for the template's type parameters */
    bool inlineOnly;    /* an instance holding a caller's lambdas: always inlined where it is called */
    Decl** instances;
    int instanceCount, instanceCap;
    Decl* pristine;     /* a template as parsed, before it was checked: what instances are copied from */
    bool returnsEarly;  /* a bare return made its result nothing (no type was written) */
    bool isPub;         /* pub: seen by the files that use this one */
    const char* alias;  /* use geometry as g: its declarations are g.area, g.Vec2 (NULL: by their own names) */
    bool lifted;        /* a lambda kept as a value, made a function: not in the text, nor in graphs */
    bool inferEffect;   /* ... whose type was not written (let f = (x: Int) => ...): its effects are its body's */
    Expr* folded;       /* D_CONST: its value, worked out when the program is compiled (NULL: worked out where used) */
    int dataAt;         /* ... an array of numbers kept in the program's data: where it is */
    const void* dataIn; /* ... and in which program's data (a prelude's constants serve many) */
    bool impliedSelf;   /* fn Vec2.length(self): self's type (Vec2, or Pair<A, B>) was not written */
    bool impliedTypes;  /* ... nor its type parameters (fn Pair.swap(self) takes Pair's A and B) */
    bool isTuple;       /* (Int, Text): a record of its own, its fields named 0, 1, ... */
    bool isEntry;       /* Entry<K, V> { key, value }: a map's entry, as a loop goes over it */
    struct Capture {    /* ... and the names around it that it uses: their values, copied where it is made */
        Binding* outer;
        Binding* inner;
    } * captures;
    int captureCount, captureCap;
    Binding* env;       /* its last parameter: the address of those values (0 when there are none) */
    bool* valueParams;  /* a template's parameters that take a function and keep it (capture, store, give it
                         * back): given a function value (a closure), not a lambda put in */
    int module;         /* the file it comes from: 0 the program's own, else one of Program.modules */
    const QkSource* src; /* that file (NULL: the program's) */
};

/* A file of the program: the one given (0), and each one a use names. */
typedef struct QkFile {
    const char* path;
    QkSource* src;
    int* uses; /* the modules it uses */
    const char** aliases; /* ... each one's alias (use x as g), or NULL */
    int useCount, useCap, aliasCap;
} QkFile;

typedef struct Program {
    QkSource src;
    Decl** decls;
    int count, cap;
    const char* closingComment;
    QkFile* modules; /* the files: [0] the program's own, then the ones it uses (qkLoadUses) */
    int moduleCount, moduleCap;
} Program;

bool qkParse(const QkSource* src, Program* out);
/* Reads the files the program's uses name (use geometry: geometry.qk next to the file; use shapes.round:
 * shapes/round.qk), and theirs, and adds their declarations (module > 0). path: the program's own file. */
bool qkLoadUses(Program* p, const char* path);
/* How a module's text is read: the file system by default; a host may give its own (NULL: not found). */
extern char* (*qkModuleReader)(const char* path, size_t* len);
TypeExpr* qkParseTypeText(const char* text); /* "[Text: Int?]" -> its TypeExpr (graphs carry types as text) */
Param qkParseMemberText(const char* text);   /* "fn speak(loud: Bool) -> Text": an interface's function */
/* Copies of a syntax tree, unchecked (clone.c): a template's body for each of its instances. */
Expr* qkCloneExpr(const Expr* e);
Block* qkCloneBlock(const Block* b);
Decl* qkCloneDecl(const Decl* d);
Expr* qkNewExpr(ExprKind kind, int line, int col);

/* ---- types and checking (check.c) ---------------------------------------------------------------- */

typedef enum TypeKind {
    TY_ERROR, TY_VOID, TY_INT, TY_FLOAT, TY_BOOL, TY_TEXT, TY_ARRAY, TY_RECORD, TY_ENUM, TY_RANGE, TY_ITER, TY_HANDLE,
    TY_MAP, /* [K: V]: elem is V */
    TY_FN,  /* a function a parameter takes: (A, B) -> R (only for specialization) */
    TY_NEVER /* what never gives a value: exit, panic (it fits wherever a value goes) */
} TypeKind;

struct Type {
    TypeKind kind;
    Type* elem;  /* TY_ARRAY, TY_ITER, TY_MAP (the value), TY_FN (the result) */
    Decl* decl;  /* TY_RECORD, TY_ENUM */
    Type* key;   /* TY_MAP */
    Type** params; /* TY_FN */
    int paramCount;
    const char* typeVar; /* a type parameter (T) while a template is checked: behaves as an error type */
    bool effect;         /* TY_FN: an effect function */
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
    Binding* safeIn; /* a loop variable over 0..len(xs): an index of xs that needs no check */
    Expr* lambda;    /* a function parameter in an instance: the caller's lambda it stands for */
    Binding** lambdaScope; /* ... and the names in reach where that lambda was written */
    int lambdaScopeCount;
    Decl* lambdaFn;  /* ... the function it was written in (effects, returns) */
    bool fnParam;    /* a function's parameter that takes a function: its lambda is put in (called, never kept) */
    Binding* capturedFrom; /* a lambda's copy of a name around it: that name (graphs wire to it) */
    Decl* paramOf;   /* a parameter: its function, and its place */
    int paramIndex;
    int line, col;   /* where it is made (editors: go to definition) */
};

typedef enum Builtin {
    B_NONE,
    B_SQRT, B_SIN, B_COS, B_TAN, B_ASIN, B_ACOS, B_ATAN, B_ATAN2, B_EXP, B_LOG, B_POW, B_ABS, B_FLOOR, B_CEIL, B_ROUND,
    B_TOFLOAT, B_TOINT, B_MIN, B_MAX, B_CLAMP, B_LERP,
    B_LEN, B_FILL, B_MAP, B_FILTER, B_FOLD, B_SUM, B_ANY, B_ALL, B_TOARRAY,
    B_PRINT, B_NOW, B_READFILE, B_WRITEFILE, B_READLINE, B_ARGS,
    B_SOME, B_TOTEXT, B_PUSH, B_SPLIT, B_TRIM, B_PARSEINT, B_PARSEFLOAT,
    B_OK, B_ERR, B_WORDS, B_LINES, B_CONTAINS, B_INDEXOF, B_STARTSWITH, B_ENDSWITH, B_REPLACE, B_UPPER, B_LOWER,
    B_JOIN, B_REPEAT, B_CHARS, B_SORT, B_SORTBY, B_REVERSE, B_FIRST, B_LAST, B_COUNTOF, B_MINBY, B_MAXBY,
    B_KEYS, B_VALUES, B_REMOVE, B_INDEX, B_PRINTERR, B_EXIT, B_ENV, B_FILEEXISTS, B_LISTDIR, B_TIME, B_ASSERT,
    B_PARALLELMAP,
    B_RANDOM, B_RANDOMINT, B_SEEDRANDOM,
    B_SPAWN, B_STEP, B_YIELD, B_WAIT,
    B_TAKE, B_SKIP, B_COUNTBY, B_MEAN, B_PANIC, B_GET,
    B_INTERFACE, /* implicit: a value made an interface (a record of functions), its functions closures over it */
    B_NEVER,     /* implicit: an expression that never gives a value (exit, panic) where a value goes */
    B_ENTRIES,   /* implicit: a map as a loop source, its entries (key, value) tuples */
    B_COUNT
} Builtin;

extern const char* const qkBuiltinNames[B_COUNT];

extern Type qkTypeError, qkTypeVoid, qkTypeInt, qkTypeFloat, qkTypeBool, qkTypeText, qkTypeRange;
/* None and [] before they meet a type: they take the optional / array type they go into */
extern Type qkTypeNone, qkTypeEmpty, qkTypeErr, qkTypeEmptyMap, qkTypeNever;
Type* qkTypeOption(Type* elem); /* T? */
Type* qkTypeResult(Type* elem); /* T! */
Type* qkTypeResultOf(Type* elem, Type* error); /* T!E: an Err holds an E (a Text: T!) */
Type* qkTypeMap(Type* key, Type* value);
Type* qkFallibleValue(const Type* t); /* the T of a T? or a T!, else NULL */
bool qkHasData(const Type* t);  /* an enum with a case carrying data */
Type* qkTypeArray(Type* elem);
Type* qkTypeIter(Type* elem);
bool qkTypeSame(const Type* a, const Type* b);
void qkTypeName(const Type* t, QkBuf* out);
int qkTypeSlots(const Type* t);
int qkFieldSlots(const Param* p); /* a record's or a case's field: its type's slots, or 1 when it is boxed */
void qkTypeSlotKinds(const Type* t, char* out); /* 'I' or 'F' per slot */
bool qkIsIterable(const Type* t);
Type* qkElemType(const Type* t);

/* What a program may call that reaches outside it: NULL, everything; else a list of names and groups, by spaces
 * or commas: a host function ("moveBy"), all of a type's or a prefix's ("Object.*", "net*"), and the built-in
 * groups print, files, env, args, exit, stdin, clock. A call to anything else is an error before it runs. */
extern const char* qkCapabilities;
bool qkCapable(const char* name);

/* main: a program must have fn main() (the command line); a library for a host need not. */
bool qkCheck(Program* p, bool needMain);
/* Puts the host's declarations (another parsed program) before a program's own. */
void qkAddPrelude(Program* p, const Program* prelude);
void qkTypesReset(void); /* after qkNewReset */
Decl* qkFindDecl(Program* p, const char* name);

/* ---- formatting (format.c) ----------------------------------------------------------------------- */

void qkFormat(const Program* p, QkBuf* out);
void qkFormatExpr(const Expr* e, QkBuf* out, int indent);
void qkFormatTypeExpr(const TypeExpr* t, QkBuf* out);
void qkFoldConst(Decl* d, const QkSource* src);
bool qkBuiltinParam(const char* fn, int at, const char* name, QkBuf* expected); /* api.c: a built-in's value's name */ /* consteval.c: a constant's value, worked out now if it can be */
void qkFormatMember(const Param* f, QkBuf* out); /* an interface's function: fn speak(loud: Bool) -> Text */
void qkFormatPattern(const Arm* a, QkBuf* out);

/* ---- JSON (graph.c): graphs, layouts, the language server's messages ----------------------------- */

typedef enum JKind { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JKind;

typedef struct J {
    JKind kind;
    double num;
    bool b;
    const char* str;
    struct J** items; /* array items, or object values */
    const char** keys;
    int count, cap;
} J;

J* qkJsonParse(const char* text, size_t len); /* NULL when it is not JSON; in the compiler's memory */
J* qkJsonGet(const J* o, const char* key);
void qkJsonString(QkBuf* b, const char* s);  /* "s", escaped */

/* ---- node graphs (graph.c) ----------------------------------------------------------------------- */

void qkGraphExport(const Program* p, QkBuf* out);
/* layoutJson (may be NULL): {"key": [x, y]} adds "pos" to the nodes with those keys. */
void qkGraphExportWith(const Program* p, QkBuf* out, const char* layoutJson);
/* The graph key of the statement a line of the canonical text belongs to. */
bool qkKeyAtLine(const Program* p, int line, char* out, size_t size);
bool qkGraphImport(const char* json, size_t len, const char* path, Program* out);

/* ---- intermediate code (lower.c) ----------------------------------------------------------------- */

typedef enum IrOp {
    IR_CONST,     /* dst = imm (an int, or a float's bits); c = 1: the data block's address + imm; c = 2: function imm's
                   * address */
    IR_MOV,       /* dst = a */
    IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD, IR_NEG,          /* ints, wrapping; DIV and MOD check for zero */
    IR_FADD, IR_FSUB, IR_FMUL, IR_FDIV, IR_FNEG, IR_SQRT,
    IR_FMIN, IR_FMAX,
    IR_CMP,       /* dst = a <cond> b, ints */
    IR_FCMP,      /* dst = a <cond> b, floats */
    IR_NOT,       /* dst = a == 0 */
    IR_I2F, IR_F2I, /* F2I truncates; cond = 1: a Float too big for an Int (or NaN) stops with a panic */
    IR_ROUND,     /* dst = round(a) as a float, imm: 0 nearest, 1 floor, 2 ceil */
    IR_LOAD,      /* dst = [a + b * imm + c] (c: a byte offset) */
    IR_STORE,     /* [a + b * imm + c] = dst */
    IR_BOUNDS,    /* a (index) < b (length), unsigned, else panic imm */
    IR_LABEL, IR_JMP,
    IR_JZ, IR_JNZ, /* jump to label imm if a is zero / not zero */
    IR_CALL,      /* rets = function imm (args); a >= 0: the function whose address a holds */
    IR_RTCALL,    /* rets = runtime function imm (args) */
    IR_RET,       /* return args */
    IR_FUEL,      /* one unit of the budget */
    IR_HOSTCALL,  /* rets = host function imm (args), C calling convention */
    IR_NOP,
    IR_AND, IR_OR, IR_XOR, /* ints: their bits */
    IR_SHL, IR_SAR         /* a shifted left / right (keeping its sign) by b, b taken modulo 64 */
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
    int inl;      /* the functions inlined where it is (an InlineCall), 0: none */
} IrIns;

/* Where inlined code came from: callee, inlined at line of the code around it (parent, or the function). */
typedef struct InlineCall {
    int parent;
    const char* callee;
    int line;
} InlineCall;

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
    int line;        /* where the function is declared (a panic on the way in: too deep) */
    char* isConst;   /* set by qkOptimize: an int register defined once, by a constant (an immediate) */
    int64_t* constValue;
} IrFn;

typedef struct IrExtern {
    const char* name;
    char* argKinds; /* 'I' / 'F' per slot */
    int argCount;
    char* retKinds;
    int retCount;
} IrExtern;

typedef struct IrProgram {
    IrFn* fns;
    int count;
    int mainIndex;  /* -1: none */
    QkBuf data;     /* text literals */
    bool fuel;
    IrExtern* externs; /* host functions called, by IR_HOSTCALL's imm */
    int externCount;
    InlineCall* inlines; /* [0] unused */
    int inlineCount, inlineCap;
} IrProgram;

bool qkLower(Program* p, IrProgram* out, bool fuel);
/* Int + - * stop with a panic when they go past the limits, instead of wrapping around (quark test, quark run
 * --checked, qkVmChecked): their IR instructions carry cond = 1. */
extern bool qkChecked;
/* Debug builds (quark debug, quark run --trace): every statement says its line, every name its new value (as
 * print writes it), every function its way in and out; nothing is inlined but lambdas. */
extern bool qkDebug;
/* The debugger's side (runtime.c): breakpoints and stepping (interactive), or a trace of the last values. */
typedef struct QkDebugger {
    bool interactive;   /* stop at breakpoints and steps, and read commands from stdin */
    int* breaks;
    int breakCount, breakCap;
    bool stepping;
    const char* tracePath; /* --trace: the last values, by graph key, written there at the end (JSON) */
} QkDebugger;
extern QkDebugger qkDebugger;
void qkDebugFinish(void); /* writes the trace */
void qkIrDump(const IrProgram* p, QkBuf* out);
/* Folds copies into the instructions that compute them, drops unused code, turns constants into immediates
 * (ints) or hoists them to the start (floats). */
void qkOptimize(IrProgram* p);

/* ---- the runtime the compiled code calls (runtime.c) --------------------------------------------- */

typedef struct RtFn {
    const char* name;
    void* addr;
    const char* args; /* 'I' or 'F' per argument */
    char ret;         /* 'I', 'F' or 'V' */
} RtFn;

typedef enum RtId {
    RT_PANIC, RT_PRINT_INT, RT_PRINT_FLOAT, RT_PRINT_BOOL, RT_PRINT_TEXT, RT_PRINT_SEP, RT_PRINT_END, RT_ALLOC,
    RT_GROW, RT_COPY, RT_NOW, RT_SIN, RT_COS, RT_TAN, RT_ASIN, RT_ACOS, RT_ATAN, RT_ATAN2, RT_EXP, RT_LOG, RT_POW, RT_FUEL_OUT,
    RT_PRINT_HANDLE, RT_TEXT_JOIN, RT_TEXT_COMPARE, RT_TEXT_OF_INT, RT_TEXT_OF_FLOAT, RT_PUSH,
    RT_READ_FILE, RT_WRITE_FILE, RT_READ_LINE, RT_ARGS, RT_SPLIT, RT_TRIM, RT_PARSE_INT, RT_PARSE_FLOAT,
    RT_FAIL, RT_EXIT, RT_PRINT_TO, RT_WORDS, RT_LINES, RT_CHARS, RT_UPPER, RT_LOWER, RT_STARTS_WITH, RT_ENDS_WITH,
    RT_REPLACE, RT_REPEAT, RT_JOIN, RT_FIND, RT_TEXT_FIXED, RT_PAD, RT_ENV, RT_LIST_DIR, RT_FILE_EXISTS, RT_TIME,
    RT_MAP_NEW, RT_MAP_SET, RT_MAP_GET, RT_MAP_REMOVE, RT_MAP_COPY, RT_MAP_LEN, RT_MAP_USED, RT_MAP_ENTRY, RT_MAP_KEYS,
    RT_MAP_VALUES, RT_LOOP_START, RT_LOOP_CHECK, RT_LOOP_COLLECT, RT_TEXT_CUT, RT_ARRAY_JOIN, RT_PARALLEL_MAP, RT_RANDOM, RT_RANDOM_INT, RT_SEED_RANDOM, RT_SPAWN, RT_STEP, RT_YIELD, RT_WAIT,
    RT_CAPTURE_BEGIN, RT_CAPTURE_END, RT_DEBUG_ENTER, RT_DEBUG_LEAVE, RT_DEBUG_LINE, RT_DEBUG_VALUE,
    RT_COUNT
} RtId;

extern const RtFn qkRuntime[RT_COUNT];
extern bool qkUseAvx;      /* the code generator may use AVX (three-operand float instructions) */
bool qkCpuHasAvx(void);
extern const char* qkRunPath; /* for messages */
/* What args() gives a program: the command line after the program's own name (set by quark run / build). */
extern char** qkProgramArgs;
extern int qkProgramArgCount;

enum { PANIC_INDEX = 1, PANIC_DIVIDE = 2, PANIC_FUEL = 3, PANIC_EMPTY = 5, PANIC_SIZE = 6, PANIC_SLICE_END = 7, PANIC_SLICE_START = 8,
       PANIC_STACK = 9, PANIC_TEXT_CUT = 10, PANIC_MEMORY = 11,
       PANIC_OVERFLOW = 12, PANIC_TOO_BIG = 13, PANIC_EMPTY_RANGE = 14, PANIC_TASK = 15 };


/* The lowest the stack may go: a function that calls others checks it on its way in, and a call that would go
 * below it stops with a panic instead of crashing the process (set on every way in from C, from the thread's
 * own stack). */
#define qkStackLimit (qkCtx->stackLimit)

typedef struct QkArena QkArena;

/* What running code needs of its own on each thread: one context per thread that runs Quark (the main one for
 * every thread unless it says otherwise: the workers of parallelMap each have theirs). Compiled code reaches
 * these through the addresses its loader gave it, so code loaded on a worker uses the worker's context. */
typedef struct QkCtx {
    int64_t fuel;              /* loop steps left (quark run --fuel, a host's budget) */
    char panicText[512];       /* a panic's message */
    void* panicFrame;          /* rbp of the Quark function that panicked */
    void* escapeRsp;           /* the trampoline's stack, saved for the escape (0: no call) */
    void (*escapeStub)(void);  /* jumps back into the trampoline: the call returns 1 */
    uintptr_t stackLimit;      /* the lowest the stack may go */
    QkArena* arena;            /* where allocations come from */
    const struct Loaded* running; /* the code being run, and what it knows of itself */
    int32_t runningEntry;      /* the function called (its offset) */
    const char* descBase;      /* a loop's memory being collected: its types' description */
    struct QkTask* task;       /* the task running (spawn, qkTaskStart), or NULL */
    int liveTasks;             /* tasks started and not done: loops keep their memory while there are any */
    uintptr_t taskStackLow;    /* the running task's stack: its lowest address */
} QkCtx;
extern _Thread_local QkCtx* qkCtx;
extern QkCtx qkMainCtx;
#define qkFuel (qkCtx->fuel)
/* A panic during a call through the entry trampoline returns from that call (qkEnter gives 1) with this
 * message; with no call in progress it prints and ends the process (the command line). */
#define qkPanicText (qkCtx->panicText)
#define qkEscapeRsp (qkCtx->escapeRsp)
#define qkEscapeStub (qkCtx->escapeStub)
void qkArenaReset(void);           /* frees what the last call allocated (keeps the memory) */
/* Memory: one arena per machine (a host's QkVm), the program's own otherwise. */
#define qkArena (qkCtx->arena)
QkArena* qkArenaNew(void);
void qkArenaFree(QkArena* a);
QkArena* qkArenaUse(QkArena* a);
/* At most this many bytes of memory for an arena (0: no limit); past it a call stops. */
void qkArenaLimit(QkArena* a, int64_t bytes);   /* makes a the one allocations come from; gives the one before */

/* ---- machine code (x64.c) ------------------------------------------------------------------------ */

enum { RELOC_RUNTIME, RELOC_DATA, RELOC_FUEL, RELOC_HOST, RELOC_ESCAPE, RELOC_PANIC_FRAME, RELOC_STACK_LIMIT };

typedef struct Reloc {
    int32_t at;    /* where the 8-byte address goes */
    int32_t kind;  /* RELOC_*: a runtime function (target: RtId), the data block (+ target), the fuel counter,
                    * a host function (target: extern index), the trampoline's saved stack */
    int64_t target;
} Reloc;

/* A call between Quark functions: where it returns to, its line, who calls whom. A panic walks the frames
 * (every Quark function keeps rbp) and finds each return address here: the stack of calls, with lines. */
typedef struct CallSite {
    int32_t ret;
    int32_t line;
    int32_t caller, callee;
    int32_t inl; /* the call is in inlined code: which */
} CallSite;

typedef struct MachineCode {
    QkBuf code;
    Reloc* relocs;
    int relocCount, relocCap;
    CallSite* sites;
    int siteCount, siteCap;
    int32_t* fnAt;    /* each function's offset */
    int fnCount;
    int32_t enter;    /* int64_t enter(QkEnter*): calls a function with slots, catches panics */
    int32_t escape;   /* void escape(void): back to the trampoline, from a panic */
} MachineCode;

/* What the trampoline gets: the function, its argument slots in, its result slots out. */
typedef struct QkEnter {
    void* fn;
    const uint64_t* args;
    int64_t argCount;
    uint64_t* results;
    int64_t resultCount;
} QkEnter;

/* Object mode (quark object): every address the code needs is read from a table in the data, pc-relative, so the
 * code holds no address a loader would write (Reloc.at: the load's place; the table's entry: the reloc's index). */
extern bool qkObjectMode;
bool qkWriteObject(const MachineCode* mc, const IrProgram* ir, const char* symbol, QkBuf* out); /* object.c */
/* The address a reloc stands for (runtime.c): what qkLoad writes, what qkModuleStatic fills a table with. */
void* qkRelocTarget(int kind, int64_t target, uint8_t* data, void* const* hosts);
/* The machine code of the processor Quark runs on: x86-64 (x64.c) or ARM64 (arm64.c). */
bool qkGenCode(const IrProgram* p, MachineCode* out);
bool qkGenX64(const IrProgram* p, MachineCode* out);
bool qkGenArm64(const IrProgram* p, MachineCode* out);
/* WebAssembly (wasm.c): a module from the same IR (its runtime imported from a JavaScript loader), and quark build
 * --target wasm32's files: the module and the loader beside it (outPath: the .wasm, NULL: next to path). */
bool qkGenWasm(const IrProgram* p, const char* path, int64_t fuel, QkBuf* out);
bool qkBuildWasm(const IrProgram* p, const char* path, int64_t fuel, const char* outPath);
enum { QK_ARCH_X64, QK_ARCH_ARM64 };
enum { QK_OS_WINDOWS, QK_OS_LINUX, QK_OS_MACOS };
extern int qkTargetArch, qkTargetOs;
/* The calling convention for calls into C (the runtime, the host): the platform's own, or System V on Windows
 * for testing that code (quark run --abi sysv), through adapters. */
enum { QK_ABI_WIN64, QK_ABI_SYSV };
extern int qkAbi;
bool qkGenSysvAdapters(QkBuf* out, int32_t* at);

/* ---- the language server (lsp.c) ----------------------------------------------------------------- */

int qkLsp(const char* prelude); /* serves an editor over stdin / stdout until it says exit */

/* Code and data in executable memory, addresses patched (host functions from `hosts`, by extern index). */
typedef struct Loaded {
    uint8_t* code;
    size_t codeLen;
    uint8_t* data;
    int64_t (*enter)(QkEnter* e);
    void (*escape)(void);
    struct Trace* trace; /* names and call sites, when known: a panic's stack of calls */
    /* what it was loaded from: another thread loads its own copy (parallelMap) */
    uint8_t* source;
    size_t dataLen;
    Reloc* relocs;
    int relocCount;
    int32_t enterAt, escapeAt;
    void** hosts;
    int hostCount;
    bool copy; /* a worker's copy: the trace is the original's */
    bool linked; /* linked into the host (an object file): nothing to unmap, nothing to load again */
} Loaded;
bool qkLoad(const uint8_t* code, size_t codeLen, const uint8_t* data, size_t dataLen, const Reloc* relocs, int relocCount,
            int32_t enter, int32_t escape, void* const* hosts, Loaded* out);
void qkUnload(Loaded* l);
/* What a panic needs to name the functions it stopped in (quark run and hosts; built executables have none). */
void qkLoadTrace(Loaded* l, const MachineCode* mc, const IrProgram* ir);
#define qkPanicFrame (qkCtx->panicFrame)
extern bool qkExitEnds;    /* exit(code) ends the process (the command line), else it stops the host's call */
/* Tasks: a function run on a stack of its own, a step at a time (yield() and wait() give the turn back). */
typedef struct QkTask QkTask;
QkTask* qkTaskNew(const Loaded* l, void* fn, const uint64_t* args, int argCount);
int qkTaskStep(QkTask* t);        /* 1: it gave its turn back (more to do), 0: it is done, -1: it stopped (a panic) */
const char* qkTaskError(const QkTask* t);
void qkTaskFree(QkTask* t);
/* Calls through the trampoline; false when it panicked (qkPanicText says why). */
bool qkRunEntry(const Loaded* l, void* fn, const uint64_t* args, int argCount, uint64_t* results, int resultCount);

#endif
