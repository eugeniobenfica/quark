/* Quark — the embedding API: any C program (an application, an editor, a server, a tool) compiles Quark, calls it, gives it host
 * functions, and turns programs into node graphs and back. Link build/libquark.a; nothing else is needed.
 *
 *   QkVm* vm = qkVmNew();
 *   qkVmPrelude(vm, "extern type Item\n"
 *                   "extern effect fn moveBy(i: Item, dx: Float, dy: Float)\n");
 *   qkVmHost(vm, "moveBy", (void*)moveBy);            // void moveBy(int64_t i, double dx, double dy)
 *   QkModule* m = qkCompile(vm, "item.qk", text, len);
 *   if (!m) for (int i = 0; i < qkDiagnosticCount(vm); i++) show(qkDiagnosticAt(vm, i));
 *   int tick = qkFind(m, "tick");                      // fn tick(item: Item, dt: Float)
 *   QkValue args[2] = {{.i = itemId}, {.f = 1.0 / 60}};
 *   if (!qkCall(m, tick, args, 2, NULL, 0)) log(qkLastError(vm));   // a panic never ends the host
 *
 * Values cross as 8-byte slots (QkValue). A type takes as many slots as it has parts, in order:
 *   Int, Bool, an enum, an extern type ........ 1 (.i)        Float ..... 1 (.f)
 *   Text ....................................... 2 (.p the bytes, .i the length)
 *   [T] ........................................ 2 (.p the elements, .i the count; each element is T's slots)
 *   a record ................................... its fields' slots, one after the other
 *
 * Host functions (extern fn) are plain C functions: each slot is one argument (int64_t, or double for Float
 * slots); one result slot is returned as int64_t or double, several as a struct of slots returned by value
 * (Windows x64 passes it through a hidden pointer, System V in registers or through that pointer; Quark
 * follows the platform's convention).
 *
 * One call at a time per process (the budget and the panic exit are shared). Each machine has its own
 * memory: what a call allocates (arrays and texts it returns) stays valid until that machine's next call. */
#ifndef QUARK_PUBLIC_H
#define QUARK_PUBLIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QUARK_VERSION 1 /* the language and this API; grows only by additions */

typedef struct QkVm QkVm;         /* host functions, settings, the last errors */
typedef struct QkModule QkModule; /* a compiled program, ready to call */

typedef union QkValue {
    int64_t i;
    double f;
    const void* p;
} QkValue;

typedef struct QkDiagnostic {
    int line, col;     /* in the text compiled (for a graph: its canonical text) */
    char key[96];      /* the graph node at that place, when the text came from a graph ("update/speed") */
    char message[320];
} QkDiagnostic;

/* ---- the machine ---------------------------------------------------------------------------------- */

QkVm* qkVmNew(void);
void qkVmFree(QkVm* vm);
/* Quark declarations every program sees: extern types and extern fns, records and enums the host shares. */
bool qkVmPrelude(QkVm* vm, const char* text);
/* The C function behind `extern fn name`. Compiling a program that calls an extern with none fails. */
void qkVmHost(QkVm* vm, const char* name, void* function);
/* How `use name` reads name.qk (next to the program's name): the file system by default; a host may give its
 * own reader, which returns malloc'd text (and its length) or NULL. */
extern char* (*qkModuleReader)(const char* path, size_t* len);
/* What the programs compiled from now on may call that reaches outside them (NULL, the default: everything).
 * A list of names, by spaces or commas: host functions ("moveBy say"), all of a type's or a prefix's
 * ("Object.*", "net*"), and the built-in groups print, files, env, args, exit, stdin, clock. A program that calls
 * anything else does not compile: "moveBy is not given to this program". One machine per level of trust, or a
 * list set before each qkCompile: a third-party plugin gets less than the application's own scripts. */
void qkVmCapabilities(QkVm* vm, const char* list);
/* Int + - * stop the call with a panic when they go past the limits (overflow), instead of wrapping around
 * (false, the default: as fast as C's). For development builds, say. */
void qkVmChecked(QkVm* vm, bool checked);
/* At most this many bytes of memory for the machine's programs (0: no limit, the default); past it a call stops
 * with "used more memory than it is given", and the host goes on. */
void qkVmMemoryLimit(QkVm* vm, int64_t bytes);
/* At most this many loop steps per call (0: no limit); past it a call stops with "ran too long". */
void qkVmFuel(QkVm* vm, int64_t steps);
/* 0: use AVX when the processor has it (the default); 1: never. */
void qkVmNoAvx(QkVm* vm, bool noAvx);
/* true: compile with the optimizing tier (SSA, range analysis, loop motion, SIMD...), slower to compile, faster
 * to run; false (the default): the quick tier, a fraction of a millisecond. */
void qkVmRelease(QkVm* vm, bool release);

int qkDiagnosticCount(const QkVm* vm);
const QkDiagnostic* qkDiagnosticAt(const QkVm* vm, int i);
const char* qkLastError(const QkVm* vm); /* why the last call stopped */

/* ---- programs ------------------------------------------------------------------------------------- */

/* NULL when it has errors (see the diagnostics). Compiling again and swapping modules is hot reload:
 * a program keeps no state between calls, so the new one goes on where the old one stopped. */
QkModule* qkCompile(QkVm* vm, const char* name, const char* text, size_t len);
QkModule* qkCompileGraph(QkVm* vm, const char* name, const char* json, size_t len);
void qkModuleFree(QkModule* m);

int qkFunctionCount(const QkModule* m);
int qkFind(const QkModule* m, const char* name); /* -1 when missing */
const char* qkFunctionName(const QkModule* m, int fn);
const char* qkFunctionSignature(const QkModule* m, int fn); /* "fn update(self: Object, dt: Float)" */
int qkParamSlots(const QkModule* m, int fn);
int qkResultSlots(const QkModule* m, int fn);
/* 'I' or 'F' per slot, parameters then results: "IF" for (Object, Float) */
const char* qkSlotKinds(const QkModule* m, int fn);

/* Runs a function. False when it stopped (an index outside an array, a division by zero, the budget...):
 * qkLastError says why, and the host goes on. */
bool qkCall(QkModule* m, int fn, const QkValue* args, int argSlots, QkValue* results, int resultSlots);

/* A program compiled ahead of time (quark object file.qk -o file.o --symbol name), linked into the host: for
 * platforms where nothing may be made executable while running (iOS, consoles). Declare it
 *     extern QkStaticProgram name;
 * and qkModuleStatic gives the module (its host functions from qkVmHost, as for qkCompile; NULL when one is
 * missing: qkLastError says which). parallelMap runs on one thread there. */
typedef struct QkStaticFn {
    const char* name;
    int64_t at, params, results;
    const char* kinds;
} QkStaticFn;
typedef struct QkStaticProgram {
    uint64_t magic;
    const uint8_t* code;
    int64_t codeLen, enter, escape;
    void** table;
    int64_t tableCount;
    const int64_t* relocs; /* each table entry's kind and target */
    uint8_t* data;
    int64_t dataLen, fnCount;
    const QkStaticFn* fns;
    int64_t externCount;
    const char* const* externs;
} QkStaticProgram;
QkModule* qkModuleStatic(QkVm* vm, QkStaticProgram* p);
/* A function as a task (a coroutine: a long job, a conversation, a workflow that waits): it runs a step at a time,
 * on a stack of its own. qkTaskResume runs it until it calls yield() or wait(seconds), or ends: 1 it gave its turn
 * back (resume it when the host is ready), 0 it is done, -1 it stopped (qkLastError says why). It gives nothing. */
typedef struct QkTaskHandle QkTaskHandle;
QkTaskHandle* qkTaskStart(QkModule* m, int fn, const QkValue* args, int argSlots);
int qkTaskResume(QkTaskHandle* t);
void qkTaskEnd(QkTaskHandle* t);

/* ---- text and graphs (malloc'd text, free with qkFree; NULL on errors, see the diagnostics) --------- */

char* qkFormatText(QkVm* vm, const char* text, size_t len);
/* The program as a node graph. `layout` (may be NULL): {"key": [x, y], ...} adds "pos" to those nodes. */
char* qkTextToGraph(QkVm* vm, const char* text, size_t len, const char* layout);
char* qkGraphToText(QkVm* vm, const char* json, size_t len);
/* Every node an editor can offer: built-in functions with their pins, the prelude's externs and types,
 * a program's own functions and types (text may be NULL). */
char* qkCatalog(QkVm* vm, const char* text, size_t len);
void qkFree(void* p);

#ifdef __cplusplus
}
#endif

#endif
