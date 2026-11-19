// Words (words.qk in C): a hash map of strdup'ed keys, qsort for the top ten.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static const char* vocab[] = {"alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta", "iota", "kappa", "lambda", "mu"};
typedef struct { char* key; long count; } Slot;
static unsigned long long hash(const char* s, size_t n) { unsigned long long h = 1469598103934665603ULL; for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 1099511628211ULL; return h; }
static int byCount(const void* a, const void* b) { long x = ((const Slot*)a)->count, y = ((const Slot*)b)->count; return (y > x) - (y < x); }
int main(void) {
    struct timespec t0, t1; timespec_get(&t0, TIME_UTC);
    long checksum = 0;
    for (int pass = 0; pass < 400; pass++) {
        size_t cap = 1 << 16, len = 0; char* text = malloc(cap);
        for (int i = 0; i < 20000; i++) {
            char piece[64]; int n = snprintf(piece, sizeof piece, "%s%d ", vocab[(i * 7 + pass) % 12], i % 97);
            if (len + n + 1 > cap) { cap *= 2; text = realloc(text, cap); }
            memcpy(text + len, piece, n); len += n;
        }
        size_t mcap = 4096; Slot* m = calloc(mcap, sizeof(Slot)); long distinct = 0;
        size_t i = 0;
        while (i < len) {
            while (i < len && text[i] == ' ') i++;
            size_t s = i; while (i < len && text[i] != ' ') i++;
            if (i == s) break;
            size_t n = i - s; unsigned long h = hash(text + s, n) & (mcap - 1);
            while (m[h].key && !(strlen(m[h].key) == n && memcmp(m[h].key, text + s, n) == 0)) h = (h + 1) & (mcap - 1);
            if (!m[h].key) { m[h].key = strndup(text + s, n); distinct++; }
            m[h].count++;
        }
        Slot* all = malloc(distinct * sizeof(Slot)); long k = 0;
        for (size_t j = 0; j < mcap; j++) if (m[j].key) all[k++] = m[j];
        qsort(all, distinct, sizeof(Slot), byCount);
        long top = 0; for (int j = 0; j < 10 && j < distinct; j++) top += all[j].count;
        checksum += distinct + top;
        for (size_t j = 0; j < mcap; j++) free(m[j].key);
        free(m); free(all); free(text);
    }
    timespec_get(&t1, TIME_UTC);
    printf("checksum %ld\nms %f\n", checksum, (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
}
