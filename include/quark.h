/* Quark — the embedding API: a C program (an engine, an editor) compiles Quark, calls it, gives it host
 * functions, and turns programs into node graphs and back. Link build/libquark.a; nothing else is needed.
 *
 *   QkVm* vm = qkVmNew();
 *   qkVmPrelude(vm, "extern type Object\n"
 *                   "extern fn moveBy(o: Object, dx: Float, dy: Float)\n");
 *   qkVmHost(vm, "moveBy", (void*)moveBy);            // void moveBy(int64_t o, double dx, double dy)
 *   QkModule* m = qkCompile(vm, "ship.qk", text, len);
 *   if (!m) for (int i = 0; i < qkDiagnosticCount(vm); i++) show(qkDiagnosticAt(vm, i));
 *   int update = qkFind(m, "update");                  // fn update(self: Object, dt: Float)
 *   QkValue args[2] = {{.i = objectId}, {.f = 1.0 / 60}};
 *   if (!qkCall(m, update, args, 2, NULL, 0)) log(qkLastError(vm));   // a panic never ends the host
 *
 * Values cross as 8-byte slots (QkValue). A type takes as many slots as it has parts, in order:
 *   Int, Bool, an enum, an extern type ........ 1 (.i)        Float ..... 1 (.f)
 *   Vec2 / Vec3 / Vec4 ......................... 2 / 3 / 4 (.f) Text ...... 2 (.p the bytes, .i the length)
 *   [T] ........................................ 2 (.p the elements, .i the count; each element is T's slots)
 *   a record ................................... its fields' slots, one after the other
 *
 * Host functions (extern fn) are plain C functions: each slot is one argument (int64_t, or double for Float
 * slots); one result slot is returned as int64_t or double, several as a struct of slots returned by value
 * (the Windows x64 convention passes it through a hidden pointer, which Quark does).
 *
 * One call at a time per process (the runtime's arena, budget and panic exit are shared). Memory a call
 * allocates (arrays it returns) stays valid until the next call. */
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
/* At most this many loop steps per call (0: no limit); past it a call stops with "ran too long". */
void qkVmFuel(QkVm* vm, int64_t steps);
/* 0: use AVX when the processor has it (the default); 1: never. */
void qkVmNoAvx(QkVm* vm, bool noAvx);

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
