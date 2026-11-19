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
typedef struct Tree { struct Tree *left, *right; } Tree;
static Tree* make(int d) { Tree* t = malloc(sizeof *t); if (d == 0) { t->left = t->right = NULL; } else { t->left = make(d - 1); t->right = make(d - 1); } return t; }
static int64_t count(Tree* t) { return t->left ? count(t->left) + count(t->right) : 1; }
static void drop(Tree* t) { if (t->left) { drop(t->left); drop(t->right); } free(t); }
int main(void) {
  double start = now(); int64_t total = 0;
  for (int depth = 4; depth < 21; depth++)
    for (int i = 0; i < (1 << (21 - depth)); i++) { Tree* t = make(depth); total += count(t); drop(t); }
  printf("checksum %lld\nms %.1f\n", (long long)total, (now() - start) * 1000);
}
