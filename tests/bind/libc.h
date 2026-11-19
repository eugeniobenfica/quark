/* A few of the C library's own declarations, as its headers write them: bound, then called (tests/run.sh). */
#include <stddef.h>

typedef struct { int quot; int rem; } div_t;

size_t strlen(const char *text);
int abs(int n);
div_t div(int numerator, int denominator);
int atoi(const char *text);
char *getenv(const char *name);
void *malloc(size_t size);
void free(void *memory);
void qsort(void *base, size_t count, size_t size, int (*compare)(const void *, const void *));
double sqrt(double x);
long strtol(const char *text, char **end, int base);
