// Records (records.qk in C): arrays of structs that hold malloc'ed arrays, rewritten in place.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef struct { long op; long* args; long nargs; long* rets; long nrets; } Ins;
typedef struct { Ins* code; long n; } Fn;
static long* arr(long n) { return malloc(n * sizeof(long)); }
int main(void) {
    struct timespec t0, t1; timespec_get(&t0, TIME_UTC);
    Fn fns[60];
    for (int s = 0; s < 60; s++) {
        fns[s].n = 8000; fns[s].code = malloc(8000 * sizeof(Ins));
        for (long i = 0; i < 8000; i++) {
            Ins* x = &fns[s].code[i]; x->op = i % 13;
            x->args = arr(3); x->args[0] = i; x->args[1] = i + s; x->args[2] = i * 2; x->nargs = 3;
            x->rets = arr(1); x->rets[0] = i + 1; x->nrets = 1;
        }
    }
    for (int r = 0; r < 40; r++)
        for (int f = 0; f < 60; f++)
            for (long k = 0; k < fns[f].n; k++) {
                Ins* x = &fns[f].code[k];
                if (x->op % 3 == 0) {
                    long* a = arr(x->nargs + 1); memcpy(a, x->args, x->nargs * sizeof(long)); a[x->nargs] = x->rets[0];
                    free(x->args); x->args = a; x->nargs++; x->op++;
                } else x->args[0]++;
            }
    long checksum = 0;
    for (int f = 0; f < 60; f++) for (long k = 0; k < fns[f].n; k++) checksum += fns[f].code[k].op + fns[f].code[k].nargs + fns[f].code[k].args[0];
    timespec_get(&t1, TIME_UTC);
    printf("checksum %ld\nms %f\n", checksum, (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
}
