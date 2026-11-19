/* A C program that calls Quark functions by their names (pub extern fn, in an object quark object made), and gives
 * Quark a function of its own: C's types and structs both ways. Prints what the tests compare. */
#include "../../include/quark.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

typedef struct Vec2 {
    float x, y;
} Vec2;
typedef struct Box {
    Vec2 min, max;
    int64_t id;
} Box;

extern QkStaticProgram cexport;
int32_t add(int32_t a, int32_t b);
Vec2 scaled(Vec2 v, double k);
Box grown(Box b, float by);
const char* greeting(const char* name);
int64_t sum(const int32_t* values, int64_t count);
float length(Vec2 v);
float quark_length(float x, float y);

static float hostLength(Vec2 v) { return sqrtf(v.x * v.x + v.y * v.y); }

int main(void) {
    QkVm* vm = qkVmNew();
    qkVmHost(vm, "hostLength", (void*)hostLength);
    QkModule* m = qkModuleStatic(vm, &cexport); /* once: the addresses the code reads, the host's functions */
    if (!m) {
        printf("no module: %s\n", qkLastError(vm));
        return 1;
    }
    printf("add %d %d\n", add(2, 40), add(2000000000, 2000000000)); /* C's int wraps */
    Vec2 v = scaled((Vec2){1.5f, -2}, 2);
    printf("scaled %.2f %.2f\n", v.x, v.y);
    Box b = grown((Box){{0, 0}, {1, 2}, 41}, 0.5f);
    printf("grown %.1f %.1f %.1f %.1f %lld\n", b.min.x, b.min.y, b.max.x, b.max.y, (long long)b.id);
    printf("%s\n", greeting("C"));
    int32_t values[] = {1, -2, 30, 400};
    printf("sum %lld\n", (long long)sum(values, 4));
    printf("length %.1f %.1f\n", length((Vec2){3, 4}), quark_length(6, 8));
    qkModuleFree(m);
    qkVmFree(vm);
    return 0;
}
