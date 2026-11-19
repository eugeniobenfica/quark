// Strings (strings.qk in C): lines formatted, filtered, trimmed into copies, joined, counted.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
int main(void) {
    struct timespec t0, t1; timespec_get(&t0, TIME_UTC);
    long checksum = 0;
    char** out = malloc(20000 * sizeof(char*));
    for (int pass = 0; pass < 150; pass++) {
        int n = 0; size_t total = 0;
        for (int i = 0; i < 20000; i++) {
            int v = i + pass; char l[96];
            int len = snprintf(l, sizeof l, "    mov r%d, [rbp - %d] // slot %d", v % 16, 8 * (v % 32), v);
            if (memcmp(l + 4, "mov", 3) == 0 && !strstr(l, "r15")) { out[n] = strndup(l + 4, len - 4); total += len - 4; n++; }
        }
        char* joined = malloc(total + n + 1); size_t at = 0;
        for (int i = 0; i < n; i++) { size_t k = strlen(out[i]); memcpy(joined + at, out[i], k); at += k; if (i + 1 < n) joined[at++] = '\n'; free(out[i]); }
        joined[at] = 0;
        long nl = n > 0 ? 1 : 0; for (size_t i = 0; i < at; i++) nl += joined[i] == '\n';
        checksum += at + nl; free(joined);
    }
    timespec_get(&t1, TIME_UTC);
    printf("checksum %ld\nms %f\n", checksum, (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
}
