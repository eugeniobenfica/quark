/* The System V calls into a host, tested on Windows: host functions with GCC's sysv_abi, called by code made
 * for System V (Linux, macOS). Ints and floats counted apart, the stack past the registers, records returned
 * in rax / rdx / xmm0 / xmm1 or through a hidden pointer. Prints what Linux would print. */
#include "../../include/quark.h"

#include <stdio.h>
#include <string.h>

extern int qkAbi; /* the compiler's: 1 is System V */

#define SYSV __attribute__((sysv_abi))

typedef struct { double x; int64_t n; } FloatInt;
typedef struct { int64_t a, b, c; } Three;
typedef struct { double x, y; } Pair;

static SYSV int64_t addThree(int64_t a, int64_t b, int64_t c) { return a * 100 + b * 10 + c; }
static SYSV double mix(int64_t a, double b, int64_t c, double d) { return (double)a * b + (double)c * d; }
static SYSV FloatInt floatInt(double x, int64_t n) { return (FloatInt){x * 2, n + 1}; }
static SYSV Pair pair(double x) { return (Pair){x, -x}; }
static SYSV Three three(int64_t a) { return (Three){a, a * 2, a * 3}; }
static SYSV double many(int64_t i1, int64_t i2, int64_t i3, int64_t i4, int64_t i5, int64_t i6, int64_t i7, int64_t i8, double f1,
                        double f2, double f3, double f4, double f5, double f6, double f7, double f8, double f9, double f10) {
    return (double)(i1 + 2 * i2 + 3 * i3 + 4 * i4 + 5 * i5 + 6 * i6 + 7 * i7 + 8 * i8) +
           (f1 + 2 * f2 + 3 * f3 + 4 * f4 + 5 * f5 + 6 * f6 + 7 * f7 + 8 * f8 + 9 * f9 + 10 * f10) / 1000;
}

static const char* prelude =
    "type FloatInt { x: Float, n: Int }\n"
    "type Three { a: Int, b: Int, c: Int }\n"
    "type Pair { x: Float, y: Float }\n"
    "extern fn addThree(a: Int, b: Int, c: Int) -> Int\n"
    "extern fn mix(a: Int, b: Float, c: Int, d: Float) -> Float\n"
    "extern fn floatInt(x: Float, n: Int) -> FloatInt\n"
    "extern fn pair(x: Float) -> Pair\n"
    "extern fn three(a: Int) -> Three\n"
    "extern fn many(i1: Int, i2: Int, i3: Int, i4: Int, i5: Int, i6: Int, i7: Int, i8: Int, f1: Float, f2: Float, f3: Float,"
    " f4: Float, f5: Float, f6: Float, f7: Float, f8: Float, f9: Float, f10: Float) -> Float\n";

static const char* program =
    "fn check(k: Float) {\n"
    "    let kept = k * 3\n"
    "    let fi = floatInt(k, 41)\n"
    "    let p = pair(kept)\n"
    "    let t = three(7)\n"
    "    print(addThree(1, 2, 3), mix(2, 1.5, 3, 0.5), fi, p, t, kept)\n"
    "    print(many(1, 2, 3, 4, 5, 6, 7, 8, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0))\n"
    "}\n";

int main(void) {
    qkAbi = 1;
    QkVm* vm = qkVmNew();
    qkVmPrelude(vm, prelude);
    qkVmHost(vm, "addThree", (void*)addThree);
    qkVmHost(vm, "mix", (void*)mix);
    qkVmHost(vm, "floatInt", (void*)floatInt);
    qkVmHost(vm, "pair", (void*)pair);
    qkVmHost(vm, "three", (void*)three);
    qkVmHost(vm, "many", (void*)many);
    QkModule* m = qkCompile(vm, "sysv.qk", program, strlen(program));
    if (!m) {
        for (int i = 0; i < qkDiagnosticCount(vm); i++) puts(qkDiagnosticAt(vm, i)->message);
        return 1;
    }
    QkValue arg = {.f = 1.25};
    if (!qkCall(m, qkFind(m, "check"), &arg, 1, NULL, 0)) {
        printf("stopped: %s\n", qkLastError(vm));
        return 1;
    }
    fflush(stdout);
    qkModuleFree(m);
    qkVmFree(vm);
    return 0;
}
