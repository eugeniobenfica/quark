/* A host that links a Quark program compiled ahead of time (quark object): no machine code is made while it
 * runs. Prints what the tests compare. */
#include "../../include/quark.h"

#include <stdio.h>

extern QkStaticProgram demo;

static double hostScale(void) { return 2.0; }

int main(void) {
    QkVm* vm = qkVmNew();
    qkVmPrelude(vm, "extern fn hostScale() -> Float\n");
    qkVmHost(vm, "hostScale", (void*)hostScale);
    QkModule* m = qkModuleStatic(vm, &demo);
    if (!m) {
        printf("no module: %s\n", qkLastError(vm));
        return 1;
    }
    QkValue args[2] = {{.f = 3}, {.f = 4}}, out[1];
    qkCall(m, qkFind(m, "area"), args, 2, out, 1);
    printf("area %.1f\n", out[0].f);
    QkValue times = {.i = 2};
    qkCall(m, qkFind(m, "greet"), &times, 1, NULL, 0);
    QkValue n = {.i = 10};
    qkCall(m, qkFind(m, "total"), &n, 1, out, 1);
    printf("total %lld\n", (long long)out[0].i);
    QkValue ab[2] = {{.i = 1}, {.i = 0}};
    if (!qkCall(m, qkFind(m, "divide"), ab, 2, out, 1)) printf("divide stopped: %s\n", qkLastError(vm));
    qkModuleFree(m);
    qkVmFree(vm);
    return 0;
}
