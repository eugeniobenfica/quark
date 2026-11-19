#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
static double now(void){LARGE_INTEGER f,t;QueryPerformanceFrequency(&f);QueryPerformanceCounter(&t);return (double)t.QuadPart/f.QuadPart;}
#else
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
#endif
#include <string.h>
static int cmpI(const void* a, const void* b) { int64_t x = *(const int64_t*)a, y = *(const int64_t*)b; return (x > y) - (x < y); }
static int cmpS(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }
int main(void) {
  int64_t n = 1000000; int64_t* xs = malloc(8 * n);
  for (int64_t i = 0; i < n; i++) xs[i] = (i * 2654435761LL + 12345) % 1000003;
  double start = now(); int64_t check = 0; int64_t* s = malloc(8 * n);
  for (int pass = 0; pass < 10; pass++) { for (int64_t i = 0; i < n; i++) s[i] = xs[i] + pass; qsort(s, n, 8, cmpI); check += s[n / 2]; }
  char** t = malloc(sizeof(char*) * 100000);
  for (int i = 0; i < 100000; i++) { t[i] = malloc(16); snprintf(t[i], 16, "item%d", (i * 7919) % 100000); }
  qsort(t, 100000, sizeof(char*), cmpS);
  printf("checksum %lld %s\nms %.1f\n", (long long)check, t[500], (now() - start) * 1000);
}
